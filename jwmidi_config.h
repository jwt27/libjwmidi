#pragma once
#if __has_include(<jw/mutex.h>)
#define JWDPMI
#include <jw/mutex.h>
#include <jw/chrono.h>
#else
#include <mutex>
#include <chrono>
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

#   ifdef JWDPMI
    using clock = jw::chrono::tsc;
    using mutex = jw::mutex;
#   else
    using clock = std::chrono::high_resolution_clock;
    using mutex = std::mutex;
#   endif

    // Assume that rdbuf() never changes on any ostream used for MIDI
    // transmission.  This avoids having to do a dynamic_cast for every
    // outgoing realtime message byte.
    constexpr bool rdbuf_never_changes = true;
}

#undef JWDPMI
