/*
 * wsacapture.h - real-time desktop audio capture, exposed as a pure C ABI.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from Sunshine (LizardByte), GPL-3.0. See NOTICE.
 *
 * The capture backend records the monitor source of a PulseAudio/PipeWire sink,
 * which is how desktop ("what you hear") audio is captured. On a PipeWire
 * system this goes through the pipewire-pulse compatibility layer.
 *
 * Threading: one capture thread blocks reading from the sink, and the frames
 * it produces are handed to the caller through wsa_capture_read_frame(). That
 * function is the single consumer entry point and must not be called
 * concurrently from more than one thread.
 */

#ifndef WSACAPTURE_H
#define WSACAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WSA_VERSION_MAJOR 1
#define WSA_VERSION_MINOR 0
#define WSA_VERSION_PATCH 0

/* The library is built with hidden symbol visibility, so every entry point in
 * this header is explicitly exported. */
#if defined(_WIN32)
#define WSA_API __declspec(dllexport)
#else
#define WSA_API __attribute__((visibility("default")))
#endif

/* Sample formats of the interleaved PCM handed back by wsa_capture_read_frame().
 * The default is F32LE, matching what Sunshine captures with. */
typedef enum {
    WSA_SAMPLE_F32LE = 0, /* 32-bit float, native little-endian, [-1.0, 1.0] */
    WSA_SAMPLE_S16LE = 1, /* signed 16-bit little-endian */
    WSA_SAMPLE_S32LE = 2  /* signed 32-bit little-endian */
} wsa_sample_format;

/* Status codes. Negative values are errors; WSA_OK is zero. */
typedef enum {
    WSA_OK = 0,
    WSA_ERR_INVAL = -1,   /* bad argument */
    WSA_ERR_NOMEM = -2,   /* allocation failed */
    WSA_ERR_PULSE = -3,   /* PulseAudio/PipeWire failure */
    WSA_ERR_FORMAT = -4,  /* unsupported or mismatched audio format */
    WSA_ERR_STATE = -5    /* wrong lifecycle state for this call */
} wsa_status;

/* Frame flags reported per frame in wsa_frame_info. */
#define WSA_FRAME_DISCONTINUITY (1u << 0) /* gap: frames were dropped before this one */
#define WSA_FRAME_SILENCE (1u << 1)       /* synthesized silence (no data available) */
#define WSA_FRAME_UNDERRUN (1u << 2)      /* the source produced nothing in time */
#define WSA_FRAME_REINIT (1u << 3)        /* the capture stream was reopened */

/* Capture configuration. Zero-initialize and override as needed; every field
 * documents its default for the zero value. */
typedef struct {
    /* Sample rate in Hz. 0 => 48000. */
    uint32_t sample_rate;
    /* Channel count. 0 => 2. */
    uint32_t channels;
    /* Sample format. WSA_SAMPLE_F32LE => 0 is the default. */
    wsa_sample_format format;
    /* Frames per delivered frame. 0 => 960 (= 20 ms at 48 kHz). One call to
     * wsa_capture_read_frame() returns exactly this many frames. */
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
} wsa_config;

/* The format and sink actually in use. */
typedef struct {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frame_samples;
    wsa_sample_format format;
    /* Resolved sink name, NUL-terminated. */
    char sink_name[256];
    /* Resolved monitor source name being recorded, NUL-terminated. */
    char monitor_name[256];
} wsa_format_info;

/* Per-frame metadata. */
typedef struct {
    /* Monotonic delivery timestamp, in nanoseconds. Diagnostics only: because
     * the capture source does not provide a sample-accurate instant, this is
     * not a media clock. */
    uint64_t timestamp_ns;
    /* Bitwise OR of WSA_FRAME_* */
    uint32_t flags;
    /* Frames dropped due to queue overflow since the previous read. */
    uint32_t dropped_frames;
} wsa_frame_info;

/* Opaque capture handle. */
typedef struct wsa_capture wsa_capture;

/* Library version as "major.minor.patch". Static storage. */
WSA_API const char *wsa_version_string(void);

/* Human-readable description of a wsa_status or wsa_capture_read_frame() return
 * value. Static storage; never NULL. */
WSA_API const char *wsa_strerror(int status);

/* Default configuration values, as documented on wsa_config. Useful to inspect
 * or to start from before overriding fields. */
WSA_API void wsa_config_defaults(wsa_config *cfg);

/* Create a capture handle. cfg may be NULL for defaults. On success *out owns a
 * handle that must be released with wsa_capture_destroy().
 * Returns WSA_OK, WSA_ERR_INVAL, or WSA_ERR_NOMEM. */
WSA_API int wsa_capture_create(const wsa_config *cfg, wsa_capture **out);

/* Connect to the audio server and start the capture thread. Returns WSA_OK or a
 * negative wsa_status; on failure wsa_capture_last_error() has the detail. */
WSA_API int wsa_capture_start(wsa_capture *c);

/* Block until one frame of exactly frame_samples frames is available and copy
 * it, interleaved, into dst.
 *
 * dst must hold at least frame_samples * channels samples of the configured
 * format, and dst_capacity_frames (counted in frames, not samples) must be at
 * least frame_samples.
 *
 * Returns 1 when a frame was written (info filled in, may be NULL), 0 on
 * timeout, or a negative wsa_status on error. A timeout is not an error: with
 * fixed_rate set, the caller normally does not see one, because silence is
 * synthesized instead. */
WSA_API int wsa_capture_read_frame(wsa_capture *c, void *dst, uint32_t dst_capacity_frames,
                           wsa_frame_info *info, int timeout_ms);

/* Fill *out with the format and resolved sink/monitor names. Valid once
 * wsa_capture_start() has succeeded. */
WSA_API int wsa_capture_get_format(wsa_capture *c, wsa_format_info *out);

/* Non-zero while the capture thread is running. */
WSA_API int wsa_capture_is_running(wsa_capture *c);

/* Stop the capture thread and release the audio server connection. Safe to call
 * more than once, and safe to call on a handle that never started. */
WSA_API void wsa_capture_stop(wsa_capture *c);

/* Stop if needed and free the handle. NULL is ignored. */
WSA_API void wsa_capture_destroy(wsa_capture *c);

/* Last error message for this handle, or "" if none. Static storage owned by
 * the handle; valid until the next call on the handle. */
WSA_API const char *wsa_capture_last_error(wsa_capture *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WSACAPTURE_H */
