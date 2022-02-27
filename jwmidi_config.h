#pragma once
#if __has_include(<jw/mutex.h>)
# define JWDPMI
# include <jw/mutex.h>
# include <jw/chrono.h>
#else
# include <mutex>
# include <chrono>
#endif

namespace jw::midi::config
{
    // Use this if you don't intend to read from / write to the same iostream
    // from multiple threads.
    struct dummy_mutex
    {
        constexpr void lock() noexcept { }
        constexpr bool try_lock() noexcept { return true; }
        constexpr void unlock() noexcept { }
    };

#   ifndef JWDPMI
    // Clock used to timestamp incoming messages.
    using clock = std::chrono::high_resolution_clock;

    // Mutex used for stream input operations.
    using rx_mutex = std::mutex;

    // Mutex used for stream output operations.
    using tx_mutex = std::mutex;
#   else
    using clock = jw::chrono::tsc;
    using rx_mutex = jw::mutex;
    using tx_mutex = jw::mutex;
#   endif

    // Assume that rdbuf() never changes on any ostream used for MIDI
    // transmission.  This avoids having to do a dynamic_cast for every
    // outgoing realtime message byte.
    constexpr bool rdbuf_never_changes = true;
}

#undef JWDPMI
