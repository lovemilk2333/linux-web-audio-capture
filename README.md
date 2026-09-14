# linux-web-audio-capture

Real-time desktop audio capture for Linux, built as a shared library with a pure
C ABI. It records what the desktop is playing and hands it to a caller one
fixed-size frame at a time.

This is the capture half of [linux-web-audio](../linux-web-audio), which wraps it
in a Go HTTP server. It is usable on its own.

## What it does

Records the **monitor source of a PulseAudio/PipeWire sink** — the standard way
to capture "what you hear" on Linux. On a PipeWire system this goes through the
`pipewire-pulse` compatibility layer, which is the same path Sunshine uses.

By default it follows the current **default sink**, re-resolving it whenever the
capture stream is reopened, so switching output devices is picked up.

## License

GPL-3.0-or-later. This library is a derivative work of
[Sunshine](https://github.com/LizardByte/Sunshine); see [NOTICE](NOTICE) for the
full derivation and attribution.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Produces `build/libwebaudio.so`, the `webacap-dump` example and the self test.

Dependencies: `libpulse-simple` (which pulls in `libpulse`), a C++17 compiler and
CMake 3.16+. Nothing else — no boost, no PipeWire headers.

```sh
ctest --test-dir build --output-on-failure   # needs a running audio server
```

## The ABI

The whole interface is in [`include/webacapture.h`](include/webacapture.h) and is
C99-callable; `examples/webacap-dump.c` is written in C on purpose as proof.

Only the `weba_*` entry points are exported (the library is built with hidden
symbol visibility).

```c
weba_config cfg;
weba_config_defaults(&cfg);          /* 48 kHz, stereo, float32, 20 ms frames */
cfg.sink = NULL;                    /* NULL => the default sink */

weba_capture *capture;
weba_capture_create(&cfg, &capture);
weba_capture_start(capture);

weba_format_info info;
weba_capture_get_format(capture, &info);

float frame[960 * 2];
weba_frame_info meta;
while (weba_capture_read_frame(capture, frame, 960, &meta, 1000) == 1) {
    /* exactly 20 ms of interleaved float32 audio */
}

weba_capture_destroy(capture);
```

### The contract that matters

- **`weba_capture_read_frame()` returns exactly `frame_samples` frames**, so one
  call maps one-to-one onto one Opus packet. No re-framing is needed.
- **The library owns the cadence.** With `fixed_rate` on (the default), frames
  come out at one per audio period regardless of the timeout passed in — even
  when nothing is playing, in which case the frame is zeros flagged
  `WEBA_FRAME_SILENCE`. A stream therefore never stalls while the desktop is
  silent or the sink is suspended.
- **Bursts are absorbed.** If the source delivers a backlog at once, the extra
  frames wait in the queue and are released one per period, so the output rate
  is not dragged above real time.
- **Timeouts are honest.** With `fixed_rate` off, a timeout returns 0 and no
  audio is invented.

### Frame flags

| Flag | Meaning |
| --- | --- |
| `WEBA_FRAME_SILENCE` | zeros, synthesized because nothing was available |
| `WEBA_FRAME_UNDERRUN` | the source produced nothing when a frame was due |
| `WEBA_FRAME_DISCONTINUITY` | frames were dropped before this one |
| `WEBA_FRAME_REINIT` | the capture stream was reopened (device changed or failed) |

### Threading

One capture thread blocks in `pa_simple_read()`. The caller's thread consumes
through `weba_capture_read_frame()`, which is the single consumer entry point and
must not be called concurrently from more than one thread.

The queue between them is mutex-guarded rather than lock-free, deliberately: the
producer is not a real-time callback, it is a thread already blocking in
libpulse (which locks internally), producing 50 frames per second. A lock costs
nothing measurable here and makes drop-oldest overflow trivially correct.

### Reopen behaviour

If a read fails — the sink disappeared, the device was unplugged — the stream is
reopened on a retry gate, re-resolving the monitor name each time. That is what
makes following a changed default sink work, and it mirrors Sunshine's reinit
path. During the gap the consumer keeps receiving silence, so the stream keeps
its cadence throughout.

## Deliberate differences from Sunshine

The capture path is a port, but not a byte-for-byte one. The differences, all
intentional:

1. **Pure C ABI** instead of Sunshine's C++ `mic_t` / `audio_control_t` class
   hierarchy, so it can be linked from Go, C or anything else.
2. **No boost.** Sunshine's `boost.log`, `safe::alarm_raw_t`, `safe::event_t`,
   `util::safe_ptr` and `util::fail_guard` are replaced by `src/primitives.h`.
3. **Silence generation.** Sunshine defines a `CONTINUOUS_AUDIO` flag meaning
   "emit silence so the stream never stalls", but **its Linux backend ignores
   it** (its `continuous` and `host_audio_enabled` parameters are marked
   `[[maybe_unused]]`). Here it is a first-class feature. Without it, capturing
   a sink that is suspended — the normal state of an idle output device —
   produces nothing to send.
4. **Cadence gating.** Sunshine's capture loop emits a frame as soon as one is
   available, so a burst from the source goes out as a burst. Here the frame
   deadline gates every frame.
5. **Bounded waits.** Sunshine waits on its alarms indefinitely; the control
   queries here time out after 5 s, so a lost callback surfaces as an error
   instead of a hang.
6. **Format table.** Sunshine opens the stream as float32 only; the sample
   format here is selectable (float32 / s16 / s32). Float32 remains the default
   and the only one Sunshine uses.
7. **Channel layouts** are limited to 1, 2, 6 and 8 channels, matching what Opus
   encodes.
8. **Sink selection** covers Sunshine's second and third priorities (a
   configured sink, then the default). Sunshine's first priority, a virtual
   null sink it creates itself, is not implemented here.

## Known limitation

`pa_simple` cannot cancel a pending `pa_simple_read()`, so a capture thread
blocked on a source that never delivers cannot be interrupted. On a working
source the read returns every audio period and stopping is immediate. If the
thread cannot be joined within 3 s, `weba_capture_stop()` logs the condition and
leaks the handle rather than freeing memory the thread is still using. Using the
asynchronous `pa_stream` API would remove this limitation at the cost of
departing from Sunshine's `pa_simple` path.
