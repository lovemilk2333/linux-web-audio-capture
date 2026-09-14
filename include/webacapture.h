/*
 * webacapture.h - real-time desktop audio capture, exposed as a pure C ABI.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from Sunshine (LizardByte), GPL-3.0. See NOTICE.
 *
 * The capture backend records the monitor source of a PulseAudio/PipeWire sink,
 * which is how desktop ("what you hear") audio is captured. On a PipeWire
 * system this goes through the pipewire-pulse compatibility layer.
 *
 * Threading: one capture thread blocks reading from the sink, and the frames
 * it produces are handed to the caller through weba_capture_read_frame(). That
 * function is the single consumer entry point and must not be called
 * concurrently from more than one thread.
 */

#ifndef WEBACAPTURE_H
#define WEBACAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEBA_VERSION_MAJOR 1
#define WEBA_VERSION_MINOR 0
#define WEBA_VERSION_PATCH 0

/* The library is built with hidden symbol visibility, so every entry point in
 * this header is explicitly exported. */
#if defined(_WIN32)
#define WEBA_API __declspec(dllexport)
#else
#define WEBA_API __attribute__((visibility("default")))
#endif

/* Sample formats of the interleaved PCM handed back by weba_capture_read_frame().
 * The default is F32LE, matching what Sunshine captures with. */
typedef enum {
    WEBA_SAMPLE_F32LE = 0, /* 32-bit float, native little-endian, [-1.0, 1.0] */
    WEBA_SAMPLE_S16LE = 1, /* signed 16-bit little-endian */
    WEBA_SAMPLE_S32LE = 2  /* signed 32-bit little-endian */
} weba_sample_format;

/* Status codes. Negative values are errors; WEBA_OK is zero. */
typedef enum {
    WEBA_OK = 0,
    WEBA_ERR_INVAL = -1,   /* bad argument */
    WEBA_ERR_NOMEM = -2,   /* allocation failed */
    WEBA_ERR_PULSE = -3,   /* PulseAudio/PipeWire failure */
    WEBA_ERR_FORMAT = -4,  /* unsupported or mismatched audio format */
    WEBA_ERR_STATE = -5    /* wrong lifecycle state for this call */
} weba_status;

/* Frame flags reported per frame in weba_frame_info. */
#define WEBA_FRAME_DISCONTINUITY (1u << 0) /* gap: frames were dropped before this one */
#define WEBA_FRAME_SILENCE (1u << 1)       /* synthesized silence (no data available) */
#define WEBA_FRAME_UNDERRUN (1u << 2)      /* the source produced nothing in time */
#define WEBA_FRAME_REINIT (1u << 3)        /* the capture stream was reopened */

/* Capture configuration. Zero-initialize and override as needed; every field
 * documents its default for the zero value. */
typedef struct {
    /* Sample rate in Hz. 0 => 48000. */
    uint32_t sample_rate;
    /* Channel count. 0 => 2. */
    uint32_t channels;
    /* Sample format. WEBA_SAMPLE_F32LE => 0 is the default. */
    weba_sample_format format;
    /* Frames per delivered frame. 0 => 960 (= 20 ms at 48 kHz). One call to
     * weba_capture_read_frame() returns exactly this many frames. */
    uint32_t frame_samples;
    /* Sink to capture the monitor of. NULL or empty => the current default
     * sink, re-resolved if it changes. */
    const char *sink;
    /* Capacity of the internal frame queue, in frames. 0 => 100 (~2 s). */
    uint32_t ring_frames;
    /* Non-zero (default): when no audio arrives in time, synthesize silence so
     * the caller still gets a frame per period at a fixed rate. This keeps a
     * stream flowing while the desktop is silent or the sink is suspended. */
    int fixed_rate;
    /* How often, in milliseconds, to check whether the default sink has changed
     * while capturing it. 0 => 1000. Only applies when sink is unset: a pinned
     * sink is never followed away from. Set this high to stop checking. */
    uint32_t sink_recheck_ms;
} weba_config;

/* The format and sink actually in use. */
typedef struct {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frame_samples;
    weba_sample_format format;
    /* Resolved sink name, NUL-terminated. */
    char sink_name[256];
    /* Resolved monitor source name being recorded, NUL-terminated. */
    char monitor_name[256];
} weba_format_info;

/* Per-frame metadata. */
typedef struct {
    /* Monotonic delivery timestamp, in nanoseconds. Diagnostics only: because
     * the capture source does not provide a sample-accurate instant, this is
     * not a media clock. */
    uint64_t timestamp_ns;
    /* Bitwise OR of WEBA_FRAME_* */
    uint32_t flags;
    /* Frames dropped due to queue overflow since the previous read. */
    uint32_t dropped_frames;
} weba_frame_info;

/* Opaque capture handle. */
typedef struct weba_capture weba_capture;

/* Library version as "major.minor.patch". Static storage. */
WEBA_API const char *weba_version_string(void);

/* Human-readable description of a weba_status or weba_capture_read_frame() return
 * value. Static storage; never NULL. */
WEBA_API const char *weba_strerror(int status);

/* Default configuration values, as documented on weba_config. Useful to inspect
 * or to start from before overriding fields. */
WEBA_API void weba_config_defaults(weba_config *cfg);

/* Create a capture handle. cfg may be NULL for defaults. On success *out owns a
 * handle that must be released with weba_capture_destroy().
 * Returns WEBA_OK, WEBA_ERR_INVAL, or WEBA_ERR_NOMEM. */
WEBA_API int weba_capture_create(const weba_config *cfg, weba_capture **out);

/* Connect to the audio server and start the capture thread. Returns WEBA_OK or a
 * negative weba_status; on failure weba_capture_last_error() has the detail. */
WEBA_API int weba_capture_start(weba_capture *c);

/* Block until one frame of exactly frame_samples frames is available and copy
 * it, interleaved, into dst.
 *
 * dst must hold at least frame_samples * channels samples of the configured
 * format, and dst_capacity_frames (counted in frames, not samples) must be at
 * least frame_samples.
 *
 * Returns 1 when a frame was written (info filled in, may be NULL), 0 on
 * timeout, or a negative weba_status on error. A timeout is not an error: with
 * fixed_rate set, the caller normally does not see one, because silence is
 * synthesized instead. */
WEBA_API int weba_capture_read_frame(weba_capture *c, void *dst, uint32_t dst_capacity_frames,
                           weba_frame_info *info, int timeout_ms);

/* Fill *out with the format and resolved sink/monitor names. Valid once
 * weba_capture_start() has succeeded. */
WEBA_API int weba_capture_get_format(weba_capture *c, weba_format_info *out);

/* Non-zero while the capture thread is running. */
WEBA_API int weba_capture_is_running(weba_capture *c);

/* Stop the capture thread and release the audio server connection. Safe to call
 * more than once, and safe to call on a handle that never started. */
WEBA_API void weba_capture_stop(weba_capture *c);

/* Stop if needed and free the handle. NULL is ignored. */
WEBA_API void weba_capture_destroy(weba_capture *c);

/* Last error message for this handle, or "" if none. Static storage owned by
 * the handle; valid until the next call on the handle. */
WEBA_API const char *weba_capture_last_error(weba_capture *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEBACAPTURE_H */
