/*
 * wsacap-dump.c - record the desktop to a WAV file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is written in C99 on purpose: it is the proof that wsacapture.h is a
 * pure C ABI and can be consumed without a C++ compiler.
 *
 * Usage: wsacap-dump [output.wav] [seconds] [sink]
 *
 * Prints a per-second summary including how many frames were synthesized
 * silence, which is the interesting number when nothing is playing.
 */

#include "wsacapture.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void put_u32le(FILE *f, uint32_t value) {
  fputc((int) (value & 0xffu), f);
  fputc((int) ((value >> 8) & 0xffu), f);
  fputc((int) ((value >> 16) & 0xffu), f);
  fputc((int) ((value >> 24) & 0xffu), f);
}

static void put_u16le(FILE *f, uint16_t value) {
  fputc((int) (value & 0xffu), f);
  fputc((int) ((value >> 8) & 0xffu), f);
}

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

/* Size of one sample of a given format, in bytes. */
static size_t sample_bytes(wsa_sample_format format) {
  return format == WSA_SAMPLE_S16LE ? 2u : 4u;
}

/* WAV uses format 3 for IEEE float and 1 for integer PCM. */
static uint16_t wav_format_code(wsa_sample_format format) {
  return format == WSA_SAMPLE_F32LE ? 3u : 1u;
}

static const char *flag_names(uint32_t flags, char *buffer, size_t size) {
  buffer[0] = '\0';
  if (flags & WSA_FRAME_DISCONTINUITY) {
    strncat(buffer, "discontinuity ", size - strlen(buffer) - 1);
  }
  if (flags & WSA_FRAME_SILENCE) {
    strncat(buffer, "silence ", size - strlen(buffer) - 1);
  }
  if (flags & WSA_FRAME_UNDERRUN) {
    strncat(buffer, "underrun ", size - strlen(buffer) - 1);
  }
  if (flags & WSA_FRAME_REINIT) {
    strncat(buffer, "reinit ", size - strlen(buffer) - 1);
  }
  return buffer;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "out.wav";
  const double seconds = argc > 2 ? atof(argv[2]) : 5.0;
  const char *sink = argc > 3 ? argv[3] : NULL;

  wsa_config cfg;
  wsa_config_defaults(&cfg);
  cfg.sink = sink;

  printf("wsacapture %s\n", wsa_version_string());

  wsa_capture *capture = NULL;
  int status = wsa_capture_create(&cfg, &capture);
  if (status != WSA_OK) {
    fprintf(stderr, "wsa_capture_create: %s\n", wsa_strerror(status));
    return 1;
  }

  status = wsa_capture_start(capture);
  if (status != WSA_OK) {
    fprintf(stderr, "wsa_capture_start: %s: %s\n", wsa_strerror(status), wsa_capture_last_error(capture));
    wsa_capture_destroy(capture);
    return 1;
  }

  wsa_format_info info;
  memset(&info, 0, sizeof(info));
  if (wsa_capture_get_format(capture, &info) != WSA_OK) {
    fprintf(stderr, "wsa_capture_get_format failed\n");
    wsa_capture_destroy(capture);
    return 1;
  }

  printf("sink:    %s\n", info.sink_name);
  printf("monitor: %s\n", info.monitor_name);
  printf("format:  %u Hz, %u ch, %u frames/frame\n", info.sample_rate, info.channels, info.frame_samples);

  const size_t frame_bytes = (size_t) info.frame_samples * info.channels * sample_bytes(info.format);
  uint8_t *buffer = malloc(frame_bytes);
  if (!buffer) {
    fprintf(stderr, "out of memory\n");
    wsa_capture_destroy(capture);
    return 1;
  }

  FILE *out = fopen(path, "wb");
  if (!out) {
    fprintf(stderr, "cannot open %s\n", path);
    free(buffer);
    wsa_capture_destroy(capture);
    return 1;
  }

  /* Header is written with placeholder sizes and patched once the length is
   * known, which is the usual way to stream a WAV. */
  const uint16_t channels = (uint16_t) info.channels;
  const uint16_t bits = (uint16_t) (sample_bytes(info.format) * 8);
  fwrite("RIFF", 1, 4, out);
  put_u32le(out, 0);
  fwrite("WAVEfmt ", 1, 8, out);
  put_u32le(out, 16);
  put_u16le(out, wav_format_code(info.format));
  put_u16le(out, channels);
  put_u32le(out, info.sample_rate);
  put_u32le(out, info.sample_rate * channels * (uint32_t) sample_bytes(info.format));
  put_u16le(out, (uint16_t) (channels * sample_bytes(info.format)));
  put_u16le(out, bits);
  fwrite("data", 1, 4, out);
  put_u32le(out, 0);
  const long data_offset = ftell(out);

  const double start = now_seconds();
  const double deadline = start + seconds;
  uint64_t frames = 0;
  uint64_t silent = 0;
  uint64_t gaps = 0;
  uint64_t dropped = 0;
  double last_report = start;

  while (now_seconds() < deadline) {
    wsa_frame_info frame;
    memset(&frame, 0, sizeof(frame));

    /* A timeout larger than one frame period is right here: the library paces
     * the output itself, so this only bounds how long we block. */
    status = wsa_capture_read_frame(capture, buffer, info.frame_samples, &frame, 1000);
    if (status < 0) {
      fprintf(stderr, "wsa_capture_read_frame: %s: %s\n", wsa_strerror(status), wsa_capture_last_error(capture));
      break;
    }
    if (status == 0) {
      continue;
    }

    fwrite(buffer, 1, frame_bytes, out);
    ++frames;
    if (frame.flags & WSA_FRAME_SILENCE) {
      ++silent;
    }
    if (frame.flags & WSA_FRAME_DISCONTINUITY) {
      ++gaps;
    }
    dropped += frame.dropped_frames;

    const double now = now_seconds();
    if (now - last_report >= 1.0) {
      char names[128];
      printf("t=%4.1fs frames=%" PRIu64 " (%.1f/s) silent=%" PRIu64 " gaps=%" PRIu64
             " dropped=%" PRIu64 " flags=[%s]\n",
             now - start, frames, (double) frames / (now - start), silent, gaps, dropped,
             flag_names(frame.flags, names, sizeof(names)));
      last_report = now;
    }
  }

  const long data_end = ftell(out);
  const uint32_t data_bytes = (uint32_t) (data_end - data_offset);
  fseek(out, 4, SEEK_SET);
  put_u32le(out, 36u + data_bytes);
  fseek(out, data_offset - 4, SEEK_SET);
  put_u32le(out, data_bytes);
  fclose(out);

  const double elapsed = now_seconds() - start;
  const double audio_seconds = info.sample_rate ? (double) frames * info.frame_samples / info.sample_rate : 0.0;
  printf("\nwrote %s: %" PRIu64 " frames (%.3f s of audio) in %.3f s wall clock\n",
         path, frames, audio_seconds, elapsed);
  printf("  synthesized silence: %" PRIu64 " frames\n", silent);
  printf("  discontinuities:     %" PRIu64 "\n", gaps);
  printf("  frames dropped:      %" PRIu64 "\n", dropped);
  if (elapsed > 0.0) {
    printf("  audio/wall ratio:    %.4f (1.0 means the cadence held)\n", audio_seconds / elapsed);
  }

  free(buffer);
  wsa_capture_destroy(capture);
  return 0;
}
