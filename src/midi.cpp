/* * * * * * * * * * * * * * * * * * jwmidi * * * * * * * * * * * * * * * * * */
/*    Copyright (C) 2022 - 2023 J.W. Jagersma, see COPYING.txt for details    */

#include <jw/midi/message.h>
#include <jw/midi/file.h>
#include <jw/io/realtime_streambuf.h>
#include <list>
#include <mutex>
#include <cxxabi.h>

namespace jw::midi
{
    struct istream_info
    {
        config::rx_mutex mutex { };
        std::vector<byte> pending_msg { };
        clock::time_point pending_msg_time;
        byte last_status { 0 };
    };
    struct ostream_info
    {
        config::tx_mutex mutex { };
        byte last_status { 0 };
        bool realtime { false };
    };

    template<typename T>
    static void delete_pword(std::ios_base::event e, std::ios_base& stream, int i)
    {
        if (e != std::ios_base::erase_event) return;
        void*& p = stream.pword(i);
        if (p == nullptr) return;
        delete static_cast<T*>(p);
        p = nullptr;
    }

    template <typename T, typename S>
    static T* get_pword(int i, S& stream)
    {
        void*& p = stream.pword(i);
        if (p == nullptr) [[unlikely]]
        {
            p = new T { };
            if (stream.iword(i)++ == 0) stream.register_callback(delete_pword<T>, i);
            if constexpr (config::rdbuf_never_changes and std::is_same_v<T, ostream_info>)
                if (dynamic_cast<io::realtime_streambuf*>(stream.rdbuf()) != nullptr)
                    static_cast<ostream_info*>(p)->realtime = true;
        }
        return static_cast<T*>(p);
    }

    static istream_info& rx_state(std::istream& stream)
    {
        static const int i = std::ios_base::xalloc();
        return *get_pword<istream_info>(i, stream);
    }

    static ostream_info& tx_state(std::ostream& stream)
    {
        static const int i = std::ios_base::xalloc();
        return *get_pword<ostream_info>(i, stream);
    }

    std::ostream& clear_status(std::ostream& stream)
    {
        auto& tx = tx_state(stream);
        std::unique_lock lock { tx.mutex };
        tx.last_status = 0;
        return stream;
    }

    static constexpr bool is_status(byte b) { return (b & 0x80) != 0; }
    static constexpr bool is_realtime(byte b) { return b >= 0xf8; }
    static constexpr bool is_system(byte b) { return b >= 0xf0; }

    struct midi_out
    {
        static constexpr std::size_t buffer_size = 4;

        midi_out(std::ostream& o) : out { o }, rdbuf { o.rdbuf() }, tx { tx_state(o) } { }

        void emit(const untimed_message& in)
        {
            if (not in.valid() or in.is_meta_message()) [[unlikely]] return;
            std::unique_lock lock { tx.mutex, std::defer_lock };
            if (not in.is_realtime_message()) lock.lock();
            std::ostream::sentry sentry { out };
            if (not sentry) [[unlikely]] return;
            try
            {
                const auto* begin = data.cbegin();
                if (auto* t = std::get_if<realtime>(&in.category))
                {
                    put_realtime(static_cast<byte>(*t) + 0xf8);
                    return;
                }
                else if (auto* t = std::get_if<channel_message>(&in.category))
                {
                    visit([this, t](auto&& msg) { (*this)(t->channel, msg); }, t->message);
                    bool running_status = tx.last_status == data[0];
                    begin += running_status;
                    size -= running_status;
                    tx.last_status = data[0];
                }
                else if (auto* t = std::get_if<system_message>(&in.category))
                {
                    visit(*this, t->message);
                    if (size > 0) tx.last_status = 0;
                }

                if (size > 0) [[likely]]
                    rdbuf->sputn(reinterpret_cast<const char*>(begin), size);
            }
            catch (const abi::__forced_unwind&) { throw; }
            catch (...) { out._M_setstate(std::ios::badbit); }
        }

        void operator()(byte ch, const note_event& msg)
        {
            const byte on = 0x90 | ch;
            const byte off = 0x80 | ch;
            if ((config::optimize_note_off or msg.velocity == 0x40) and not msg.on and tx.last_status == on)
                put(on, msg.note, 0x00);
            else
                put(msg.on ? on : off, msg.note, msg.velocity);
        }

        void operator()(byte ch, const key_pressure& msg)
        {
            put(0xa0 | ch, msg.note, msg.value);
        }

        void operator()(byte ch, const control_change& msg)
        {
            put(0xb0 | ch, msg.control, msg.value);
        }

        void operator()(byte ch, const program_change& msg)
        {
            put(0xc0 | ch, msg.value);
        }

        void operator()(byte ch, const channel_pressure& msg)
        {
            put(0xd0 | ch, msg.value);
        }

        void operator()(byte ch, const pitch_change& msg)
        {
            put(0xe0 | ch, msg.value.lo, msg.value.hi);
        }

        void operator()(const sysex& msg)
        {
            bool in_sysex = false;
            auto i = msg.data.cbegin();
            const auto end = msg.data.cend();
            while (true)
            {
                if (not in_sysex)
                {
                    for (; i != end; ++i)
                    {
                        if (not is_status(*i)) continue;
                        if (is_realtime(*i)) continue;
                        if (*i == 0xf0) break;
                        if (is_system(*i)) tx.last_status = 0;
                        else tx.last_status = *i;
                    }
                }
                else i = std::find(i, end, 0xf7);
                if (i == end) break;
                in_sysex ^= true;
            }
            rdbuf->sputn(reinterpret_cast<const char*>(msg.data.data()), msg.data.size());
            size = 0;
        }

        void operator()(const mtc_quarter_frame& msg)
        {
            put(0xf1, msg.data);
        }

        void operator()(const song_position& msg)
        {
            put(0xf2, msg.value.lo, msg.value.hi);
        }

        void operator()(const song_select& msg)
        {
            put(0xf3, msg.value);
        }

        void operator()(const tune_request&)
        {
            put(0xf6);
        }

    private:
        void put_realtime(byte a)
        {
            if constexpr (config::rdbuf_never_changes)
            {
                if (tx.realtime) return static_cast<jw::io::realtime_streambuf*>(rdbuf)->put_realtime(a);
            }
            else if (auto* rtbuf = dynamic_cast<io::realtime_streambuf*>(rdbuf))
            {
                return rtbuf->put_realtime(a);
            }
            rdbuf->sputc(a);
        }

        template<unsigned I = 0, typename... T>
        void put(std::uint8_t v, T... list)
        {
            data[I] = v;
            put<I + 1>(list...);
        }

        template<unsigned I> void put()
        {
            static_assert(I <= buffer_size);
            size = I;
        }

        std::ostream& out;
        std::streambuf* const rdbuf;
        ostream_info& tx;
        std::size_t size;
        std::array<byte, buffer_size> data;
    };

    void emit(std::ostream& out, const untimed_message& msg)
    {
        midi_out { out }.emit(msg);
    }

    struct unexpected_status { };

    static constexpr std::size_t msg_size(byte status)
    {
        switch (status & 0xf0)
        {
        case 0x80:
        case 0x90: return 2;
        case 0xa0: return 2;
        case 0xb0: return 2;
        case 0xc0: return 1;
        case 0xd0: return 1;
        case 0xe0: return 2;
        case 0xf0:
            switch (status)
            {
            case 0xf0: return -2;
            case 0xf1: return 1;
            case 0xf2: return 2;
            case 0xf3: return 1;
            case 0xf6: return 0;
            case 0xf4:
            case 0xf5:
            case 0xf7:
            case 0xf9:
            case 0xfd: throw io::failure { "invalid status byte" };
            default: return 0;
            }
        default: __builtin_unreachable();
        }
    }

    static untimed_message realtime_msg(byte status)
    {
        switch (status)
        {
        case 0xf8:
        case 0xfa:
        case 0xfb:
        case 0xfc:
        case 0xfe:
        case 0xff: return { static_cast<realtime>(status - 0xf8) };
        case 0xf9:
        case 0xfd: throw io::failure { "invalid status byte" };
        default: __builtin_unreachable();
        }
    }

    template<typename I>
    static untimed_message make_msg(byte status, I i)
    {
        const unsigned ch = status & 0x0f;
        switch (status & 0xf0)
        {
        case 0x80:
        case 0x90:
            {
                byte vel = i[1];
                bool on = (status & 0x10) != 0;
                if (on and vel == 0)
                {
                    on = false;
                    vel = 0x40;
                }
                return { ch, note_event { i[0], vel, on } };
            }
        case 0xa0: return { ch, key_pressure     { i[0], i[1] } };
        case 0xb0: return { ch, control_change   { i[0], i[1] } };
        case 0xc0: return { ch, program_change   { i[0] } };
        case 0xd0: return { ch, channel_pressure { i[0] } };
        case 0xe0: return { ch, pitch_change     { { i[0], i[1] } } };
        case 0xf0:
            switch (status)
            {
            case 0xf0: __builtin_unreachable();
            case 0xf1: return { mtc_quarter_frame { i[0] } };
            case 0xf2: return { song_position     { { i[0], i[1] } } };
            case 0xf3: return { song_select       { i[0] } };
            case 0xf6: return { tune_request      { } };
            case 0xf4:
            case 0xf5:
            case 0xf7: throw io::failure { "invalid status byte" };
            default: return realtime_msg(status);
            }
        default: __builtin_unreachable();
        }
    }

    template<bool dont_block>
    static message do_extract(std::istream& in)
    {
        auto& rx { rx_state(in) };
        std::unique_lock lock { rx.mutex };
        auto* const buf { in.rdbuf() };
        std::istream::sentry sentry { in, true };
        if (not sentry) return { };

        auto peek = [&]() -> std::optional<byte>
        {
            if (dont_block and buf->in_avail() == 0)
            {
                buf->pubsync();
                if (buf->in_avail() == 0) return { };
            }
            auto b = buf->sgetc();
            if (b == std::char_traits<char>::eof()) throw io::end_of_file { };
            return { static_cast<byte>(b) };
        };

        auto get = [&]
        {
            auto b = peek();
            if (b)
            {
                buf->sbumpc();
                if (not is_realtime(*b)) rx.pending_msg.push_back(*b);
            }
            return b;
        };

        try
        {
            byte status = rx.last_status;

            // Wait for data to arrive
            if (rx.pending_msg.empty())
            {
                // Discard data until the first status byte
                if (status == 0) while (true)
                {
                    const auto b = peek();
                    if (not b) return { };
                    if (is_status(*b) and *b != 0xf7) break;
                    buf->sbumpc();
                }
                const auto b = get();
                if (not b) return { };
                rx.pending_msg_time = clock::now();
                if (is_realtime(*b)) return message { realtime_msg(*b), rx.pending_msg_time };
            }

            // Check for new status byte
            bool new_status = false;
            if (is_status(rx.pending_msg.front()))
            {
                status = rx.pending_msg.front();
                new_status = true;
            }

            // Read bytes from streambuf
            const bool is_sysex = status == 0xf0;
            while (rx.pending_msg.size() < msg_size(status) + new_status)
            {
                const auto b = get();
                if (not b) return { };
                if (is_realtime(*b)) return message { realtime_msg(*b), clock::now() };
                if (is_status(*b))
                {
                    if (is_sysex and *b == 0xf7) break;
                    rx.pending_msg_time = clock::now();
                    rx.pending_msg.clear();
                    rx.pending_msg.push_back(*b);
                    throw unexpected_status { };
                }
            }

            // Store running status
            if (is_system(status)) rx.last_status = 0;
            else rx.last_status = status;

            local_destructor clear_pending_on_return { [&rx] { rx.pending_msg.clear(); } };

            // Construct the message
            if (is_sysex) return { sysex { { rx.pending_msg.cbegin(), rx.pending_msg.cend() } }, rx.pending_msg_time };
            else return message { make_msg(status, rx.pending_msg.cbegin() + new_status), rx.pending_msg_time };
        }
        catch (const io::failure&)
        {
            rx.pending_msg.clear();
            rx.last_status = 0;
            in._M_setstate(std::ios::failbit);
        }
        catch (const unexpected_status&)
        {
            try { in._M_setstate(std::ios::failbit); }
            catch (const unexpected_status&) { }
            throw io::failure { "unexpected status byte" };
        }
        catch (const io::end_of_file&) { in._M_setstate(std::ios::eofbit); }
        catch (const abi::__forced_unwind&) { throw; }
        catch (...) { in._M_setstate(std::ios::badbit); }
        return { };
    }

    message extract(std::istream& in) { return do_extract<false>(in); }
    message try_extract(std::istream& in) { return do_extract<true>(in); }

    struct file_buffer
    {
        file_buffer(std::streambuf* buf, std::size_t s)
            : size { s }, data { new byte[size] }, i { begin() }
        {
            const std::size_t bytes_read = buf->sgetn(reinterpret_cast<char*>(data.get()), size);
            if (bytes_read < size) throw io::end_of_file { };
        }

        template<typename T>
        void read(T* dst, std::size_t n)
        {
            if (i + n > end()) throw io::failure { "read past end of chunk" };
            for (unsigned j = 0; j < n; ++j)
                reinterpret_cast<byte*>(dst)[j] = i[j];
            i += n;
            return;
        }

        std::uint32_t read_32()
        {
            union
            {
                std::array<byte, 4> raw;
                std::uint32_t value;
            };
            read(raw.data(), 4);
            return __builtin_bswap32(value);
        }

        std::uint32_t read_24()
        {
            union
            {
                std::array<byte, 4> raw;
                std::uint32_t value;
            };
            raw[0] = 0;
            read(raw.data() + 1, 3);
            return __builtin_bswap32(value);
        }

        std::uint16_t read_16()
        {
            union
            {
                std::array<byte, 2> raw;
                std::uint16_t value;
            };
            read(raw.data(), 2);
            return __builtin_bswap16(value);
        }

        std::uint8_t read_8()
        {
            if (i == end()) throw io::failure { "read past end of chunk" };
            return *i++;
        }

        std::uint32_t read_vlq()
        {
            std::uint32_t value { };
            byte b;
            do
            {
                b = read_8();
                value <<= 7;
                value |= b & 0x7f;
            } while ((b & 0x80) != 0);
            return value;
        }

        const byte* begin() const noexcept { return data.get(); }
        const byte* end() const noexcept { return data.get() + size; }

    private:
        const std::size_t size;
        std::unique_ptr<byte[]> data;
        const byte* i;
    };

    static std::size_t find_chunk(std::streambuf* buf, std::string_view want)
    {
        auto read = [buf](char* data, std::size_t size)
        {
            if (size == 0) return;
            const std::size_t bytes_read = buf->sgetn(data, size);
            if (bytes_read < size) throw io::end_of_file { };
        };

        auto read_32 = [read]()
        {
            union
            {
                std::array<char, 4> raw;
                std::uint32_t value;
            };
            read(raw.data(), 4);
            return __builtin_bswap32(value);
        };

        std::array<char, 4> raw;
        do
        {
            read(raw.data(), 4);
            const std::size_t size = read_32();
            const std::string_view have { raw.data(), 4 };
            if (have == want) return size;
            buf->pubseekoff(size, std::ios::cur, std::ios::in);
        } while (true);
    }

    static auto text_type(byte type) noexcept
    {
        switch (type)
        {
        case 0x01: return meta::text::any;
        case 0x02: return meta::text::copyright;
        case 0x03: return meta::text::track_name;
        case 0x04: return meta::text::instrument_name;
        case 0x05: return meta::text::lyric;
        case 0x06: return meta::text::marker;
        case 0x07: return meta::text::cue_point;
        default: __builtin_unreachable();
        }
    }

    static void read_track(file::track& trk, file_buffer& buf)
    {
        std::array<byte, 8> v;
        bool in_sysex = false;
        byte last_status = 0;
        std::uint64_t time = 0;
        decltype(meta::channel) meta_ch { };

        while (true)
        {
            time += buf.read_vlq();
            auto& pos = trk.emplace_hint(trk.end(), std::piecewise_construct, std::make_tuple(time), std::make_tuple())->second;
            const byte b = buf.read_8();
            switch (b)
            {
            case 0xff:  // Meta message
                {
                    last_status = 0;
                    const byte type = buf.read_8();
                    const std::size_t size = buf.read_vlq();
                    switch (type)
                    {
                    case 0x00:
                        if (size != 2) throw io::failure { "incorrect message size" };
                        pos.emplace_back(meta_ch, meta::sequence_number { buf.read_16() });
                        break;

                    case 0x01: case 0x02: case 0x03: case 0x04:
                    case 0x05: case 0x06: case 0x07:
                        {
                            meta::text msg { text_type(type), { } };
                            msg.text.resize(size);
                            buf.read(msg.text.data(), size);
                            pos.emplace_back(meta_ch, std::move(msg));
                            break;
                        }

                    case 0x20:
                        {
                            if (size != 1) throw io::failure { "incorrect message size" };
                            auto ch = buf.read_8();
                            if (ch > 15) throw io::failure { "invalid channel number" };
                            meta_ch = ch;
                            break;
                        }

                    case 0x2f:
                        return;

                    case 0x51:
                        if (size != 3) throw io::failure { "incorrect message size" };
                        pos.emplace_back(meta_ch, meta::tempo_change { std::chrono::microseconds { buf.read_24() } });
                        break;

                    case 0x54:
                        {
                            if (size != 5) throw io::failure { "incorrect message size" };
                            buf.read(v.data(), 5);
                            pos.emplace_back(meta_ch, meta::smpte_offset { v[0], v[1], v[2], v[3], v[4] });
                            break;
                        }

                    case 0x58:
                        {
                            if (size != 4) throw io::failure { "incorrect message size" };
                            buf.read(v.data(), 4);
                            pos.emplace_back(meta_ch, meta::time_signature { v[0], v[1], v[2], v[3] });
                            break;
                        }

                    case 0x59:
                        {
                            if (size != 2) throw io::failure { "incorrect message size" };
                            buf.read(v.data(), 2);
                            pos.emplace_back(meta_ch, meta::key_signature { v[0], v[1] != 0 });
                            break;
                        }

                    default:
                        {
                            meta::unknown msg { type, { } };
                            msg.data.resize(size);
                            buf.read(msg.data.data(), size);
                            pos.emplace_back(meta_ch, std::move(msg));
                            break;
                        }
                    }
                    break;
                }

            case 0xf7:  // Either a sysex, part of a sysex, or escape sequence (may be any message, or multiple)
                {
                    last_status = 0;
                    meta_ch.reset();
                    std::vector<byte> data { };
                    const std::size_t size = buf.read_vlq();
                    data.reserve(size);
                    for (unsigned i = 0; i < size; ++i)
                    {
                        const byte b = buf.read_8();
                        data.push_back(b);

                        switch (b)
                        {
                        case 0xf0:
                            last_status = 0;
                            in_sysex = true;
                            for (; i < size; ++i)
                            {
                                const byte b = buf.read_8();
                                data.push_back(b);
                                if (b == 0xf7) break;
                            }
                            if (i == size) break;
                            [[fallthrough]];
                        case 0xf7:
                            pos.emplace_back(sysex { std::move(data) });
                            if (i < size)
                            {
                                data = { };
                                data.reserve(size - i);
                            }
                            last_status = 0;
                            in_sysex = false;
                            break;

                        default:
                            if (not in_sysex)
                            {
                                byte status = last_status;
                                if (is_status(b)) status = b;
                                if (status == 0) throw io::failure { "no status byte" };

                                for (unsigned j = not is_status(b); j < msg_size(status); ++i, ++j)
                                {
                                    if (i == size) throw io::failure { "message extends past end of escape" };
                                    const byte b = buf.read_8();
                                    data.push_back(b);
                                }

                                if (not is_realtime(status))
                                {
                                    if (is_system(status)) last_status = 0;
                                    else last_status = status;
                                }

                                pos.emplace_back(make_msg(status, data.data() + is_status(b)));
                                data.clear();
                            }
                        }
                    }
                    if (data.size() > 0) pos.emplace_back(sysex { std::move(data) });
                    last_status = 0;
                    break;
                }

            case 0xf0:  // Complete sysex or first part of a timed sysex
                {
                    last_status = 0;
                    meta_ch.reset();
                    const std::size_t size = buf.read_vlq();
                    sysex msg { };
                    msg.data.resize(size + 1);
                    msg.data[0] = 0xf0;
                    buf.read(msg.data.data() + 1, size);
                    in_sysex = true;
                    if (msg.data.back() == 0xf7) in_sysex = false;
                    pos.emplace_back(std::move(msg));
                    break;
                }

            default:    // Channel message
                {
                    in_sysex = false;
                    meta_ch.reset();
                    auto* i = v.data();
                    byte status = last_status;
                    if (is_status(b)) status = b;
                    else *i++ = b;
                    switch (status)
                    {
                    case 0x00: case 0xf0: case 0xf7:
                        throw io::failure { "invalid status byte" };
                    }

                    const std::size_t size = msg_size(status);
                    if (size > 0) buf.read(i, size - (not is_status(b)));

                    // Also accept realtime and system messages here (non-standard)
                    if (not is_realtime(status))
                    {
                        if (is_system(status)) last_status = 0;
                        else last_status = status;
                    }

                    pos.emplace_back(make_msg(status, i));
                    break;
                }
            }
        }
    }

    file file::read(std::istream& in)
    {
        file output { };
        auto* const rdbuf { in.rdbuf() };
        std::istream::sentry sentry { in, true };
        if (not sentry) return output;

        try
        {
            file_buffer buf { rdbuf, find_chunk(rdbuf, "MThd") };
            const std::uint16_t format = buf.read_16();
            const std::size_t num_tracks = buf.read_16();
            const split_uint16_t division = buf.read_16();

            if (format == 0 and num_tracks != 1) throw io::failure { "incorrect number of tracks" };
            if (format > 2) throw io::failure { "invalid format" };
            output.asynchronous_tracks = format == 2;
            output.tracks.resize(num_tracks);

            if ((division & 0x8000) == 0) output.time_division.emplace<unsigned>(division);
            else output.time_division.emplace<smpte_format>(-static_cast<int8_t>(division.hi), division.lo);

            for (auto& trk : output.tracks)
            {
                file_buffer buf { rdbuf, find_chunk(rdbuf, "MTrk") };
                read_track(trk, buf);
            }
        }
        catch (const io::failure&) { in._M_setstate(std::ios::failbit); }
        catch (const io::end_of_file&) { in._M_setstate(std::ios::eofbit); }
        catch (const abi::__forced_unwind&) { throw; }
        catch (...) { in._M_setstate(std::ios::badbit); }
        return output;
    }
}
