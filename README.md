# jwmidi

This aims to be a complete library for encoding, decoding and manipulating
MIDI data - including the tricky parts.  Reading from standard MIDI files is
also supported.

## Overview

A MIDI message is represented essentially as a `std::variant` of simple
aggregate types, one for each kind of message.  These are defined in
`<jw/midi/message.h>`, near the top.  To avoid unwieldy variant
visitors, these are further separated into categories `channel_message`,
`system_message` and `realtime_message`.

The class that binds these all together is simply called `message`.  These can
be constructed from the basic message types, with a channel number where
applicable - categorization is performed automatically.  It also includes a
timestamp.  If you don't need that, you can use `untimed_message`.

Transmitting and receiving messages is performed through regular iostreams.
You can use the stream operators `<<` and `>>`, or functions `emit()` and
`extract()`.  A non-blocking version of the latter is `try_extract()`.

Time for some brief examples.  Let's send a C5 note on channel 0, followed by a
clock tick:

```c++
my_stream << midi::message { 0, midi::note_event { 72, 100, true } };
my_stream << midi::message { midi::realtime::clock_tick };
```

Manipulating received messages is also quite straightforward.  The following is
a MIDI passthrough that sets the velocity for all note events to 100:

```c++
midi::message msg;
while (true)
{
    my_stream >> msg;
    if (auto* ch_msg = std::get_if<midi::channel_message>(&msg.category))
        if (auto* event = std::get_if<midi::note_event>(&ch_msg->message))
            event->velocity = 100;
    my_stream << msg << std::flush;
}
```

## Details

MIDI may look like a simple protocol at first, but there are a few 'gotchas'.
One is "running status", where the status byte for a channel message may be
omitted if it is identical to the last.  Another is that realtime messages may
appear right in the middle of any other message.

Interpreting MIDI data from a byte stream therefore requires some global state
for context.  This library uses `xalloc()`/`pword()` to store this state inside
the `iostream` itself.

This state also includes a mutex for safe concurrent stream access.  Since
realtime messages may be freely interleaved with other messages, the mutex is
not locked when transmitting these.  The underlying `streambuf` therefore still
needs to be implemented in a thread-safe manner, but does not need to (and
ideally, should not) enforce strict sequencing.

To make realtime messages truly real-time, you can implement a
`realtime_streambuf` (defined in jwutil, `<jw/io/realtime_streambuf.h>`).  This
is essentially a regular `streambuf` with one additional virtual function,
`put_realtime()`, which puts a single byte on the wire immediately, bypassing
any buffers.

When receiving a message that is interrupted by a realtime message, the
realtime message is always returned first.  The next message will then be the
initial message, with a timestamp that precedes the realtime one.

Another 'gotcha' is timed sysex messages, where the timing of individual sysex
bytes is important.  Handling these is currently not implemented - receiving
such a message may block for an extended period of time, and sending them may
result in protocol errors.  Fortunately, they are only very rarely encountered.

## Compiling

There is one dependency: [jwutil](https://github.com/jwt27/libjwutil).  Both
this library and jwutil are meant to be added as a submodule in your project.

Building involves calling the `configure` script, then running `make` - pretty
straightforward.  The configure script needs to know the path to the jwutil
build directory, specify this via `--with-jwutil=...`.
