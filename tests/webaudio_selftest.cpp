/**
 * @file webaudio_selftest.cpp
 * @brief Exercises the public ABI against a live audio server.
 *
 * Checks the properties that matter for a streaming consumer: fixed-length
 * frames, a frame cadence that matches the audio clock, a populated format, and
 * clean lifecycle transitions. A live PulseAudio/PipeWire server is required.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "webacapture.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
  int g_failures = 0;

  void check(bool condition, const std::string &what) {
    std::printf("%s %s\n", condition ? "  ok  " : "FAIL  ", what.c_str());
    if (!condition) {
      ++g_failures;
    }
  }

  int read_channel_count_with(uint32_t channels) {
    weba_config cfg;
    weba_config_defaults(&cfg);
    cfg.channels = channels;

    weba_capture *capture = nullptr;
    const int status = weba_capture_create(&cfg, &capture);
    if (capture) {
      weba_capture_destroy(capture);
    }
    return status;
  }

  /**
   * @brief Read @p count frames, reporting whether every read returned a frame.
   *
   * Returns the number of frames that carried the silence flag.
   */
  std::size_t read_frames(weba_capture *capture, const weba_format_info &info, int count, int timeout_ms,
                          std::size_t &discontinuities, std::size_t &frames_read) {
    const std::size_t bytes_per_sample = info.format == WEBA_SAMPLE_S16LE ? 2u : 4u;
    const std::size_t bytes = static_cast<std::size_t>(info.frame_samples) * info.channels * bytes_per_sample;
    std::vector<unsigned char> buffer(bytes);

    std::size_t silent = 0;
    int frame_index = 0;
    for (; frame_index < count; ++frame_index) {
      weba_frame_info frame {};
      const int status = weba_capture_read_frame(capture, buffer.data(), info.frame_samples, &frame, timeout_ms);
      if (status != 1) {
        check(false, "read_frame returned " + std::to_string(status) + " at frame " +
                       std::to_string(frame_index) + " of " + std::to_string(count));
        break;
      }
      if (frame.flags & WEBA_FRAME_SILENCE) {
        ++silent;
      }
      if (frame.flags & WEBA_FRAME_DISCONTINUITY) {
        ++discontinuities;
      }
    }
    frames_read = static_cast<std::size_t>(frame_index);
    return silent;
  }
}  // namespace

int main() {
  std::printf("webacapture selftest, library %s\n\n", weba_version_string());

  check(std::strlen(weba_version_string()) > 0, "version string is not empty");
  check(std::strcmp(weba_strerror(WEBA_OK), "success") == 0, "strerror(WEBA_OK)");

  /* Configuration validation happens before any audio server is contacted. */
  check(read_channel_count_with(1) == WEBA_OK, "1 channel is accepted");
  check(read_channel_count_with(2) == WEBA_OK, "2 channels are accepted");
  check(read_channel_count_with(6) == WEBA_OK, "6 channels are accepted");
  check(read_channel_count_with(8) == WEBA_OK, "8 channels are accepted");
  check(read_channel_count_with(3) == WEBA_ERR_FORMAT, "3 channels are rejected");

  weba_config cfg;
  weba_config_defaults(&cfg);
  check(cfg.sample_rate == 48000, "default sample rate is 48000");
  check(cfg.channels == 2, "default channel count is 2");
  check(cfg.format == WEBA_SAMPLE_F32LE, "default format is float32");
  check(cfg.frame_samples == 960, "default frame size is 960 samples (20 ms)");
  check(cfg.fixed_rate == 1, "fixed-rate output is on by default");

  weba_capture *capture = nullptr;
  check(weba_capture_create(&cfg, &capture) == WEBA_OK, "create with defaults");
  check(capture != nullptr, "handle was returned");
  check(weba_capture_is_running(capture) == 0, "a fresh handle is not running");

  /* Reading before start must be refused rather than crash. */
  {
    std::vector<float> scratch(cfg.frame_samples * cfg.channels);
    check(weba_capture_read_frame(capture, scratch.data(), cfg.frame_samples, nullptr, 0) == WEBA_ERR_STATE,
          "read before start is refused");
  }

  const int start_status = weba_capture_start(capture);
  check(start_status == WEBA_OK, std::string {"start: "} + weba_strerror(start_status) +
                                  " (" + weba_capture_last_error(capture) + ")");
  if (start_status != WEBA_OK) {
    std::printf("\n%d failure(s); no audio server, skipping the live checks\n", g_failures);
    weba_capture_destroy(capture);
    return 1;
  }

  check(weba_capture_is_running(capture) == 1, "handle reports running");

  /* A sink that does not exist must fail the start rather than quietly stream
   * nothing forever. */
  {
    weba_config bad;
    weba_config_defaults(&bad);
    bad.sink = "no-such-sink-webaudio-selftest";

    weba_capture *handle = nullptr;
    const int create_status = weba_capture_create(&bad, &handle);
    int start_status = create_status;
    if (create_status == WEBA_OK) {
      start_status = weba_capture_start(handle);
    }
    check(create_status == WEBA_OK && start_status == WEBA_ERR_PULSE,
          "an unknown sink name fails at start, not silently");
    check(handle && std::strlen(weba_capture_last_error(handle)) > 0, "the failure carries a message");
    if (handle) {
      weba_capture_destroy(handle);
    }
  }

  weba_format_info info {};
  check(weba_capture_get_format(capture, &info) == WEBA_OK, "get_format succeeds");
  std::printf("        sink=%s monitor=%s\n", info.sink_name, info.monitor_name);
  check(info.sample_rate == cfg.sample_rate, "reported rate matches the request");
  check(info.channels == cfg.channels, "reported channel count matches the request");
  check(info.frame_samples == cfg.frame_samples, "reported frame size matches the request");
  check(std::strlen(info.monitor_name) > 0, "a monitor source was resolved");
  check(std::strstr(info.monitor_name, ".monitor") != nullptr, "the resolved source is a monitor");

  /* A destination smaller than one frame is a caller error. */
  {
    std::vector<float> scratch(cfg.frame_samples * cfg.channels);
    check(weba_capture_read_frame(capture, scratch.data(), cfg.frame_samples - 1, nullptr, 0) == WEBA_ERR_INVAL,
          "an undersized destination is refused");
  }

  /* The real check: 50 frames should take about one second of wall clock. */
  const int frame_count = 50;
  const double expected_seconds =
    static_cast<double>(frame_count) * cfg.frame_samples / static_cast<double>(cfg.sample_rate);

  std::size_t discontinuities = 0;
  std::size_t frames_read = 0;
  const auto started = std::chrono::steady_clock::now();
  const std::size_t silent = read_frames(capture, info, frame_count, 1000, discontinuities, frames_read);
  const auto elapsed =
    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  std::printf("        %zu of %d frames in %.3f s wall clock (%.3f s of audio expected), %zu silent, "
              "%zu discontinuities\n",
              frames_read, frame_count, elapsed, expected_seconds, silent, discontinuities);
  check(frames_read == static_cast<std::size_t>(frame_count), "every frame was delivered");

  const double ratio = expected_seconds > 0.0 ? elapsed / expected_seconds : 0.0;
  check(ratio > 0.9 && ratio < 1.15,
        "frame cadence matches the audio clock (ratio " + std::to_string(ratio) + ")");
  check(discontinuities == 0, "no spurious discontinuities on a healthy first run");

  weba_capture_stop(capture);
  check(weba_capture_is_running(capture) == 0, "stop clears the running flag");

  /* Stop is required to be idempotent. */
  weba_capture_stop(capture);
  check(weba_capture_is_running(capture) == 0, "stop is idempotent");

  weba_capture_destroy(capture);
  std::printf("\ndestroy returned cleanly\n");

  if (g_failures > 0) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
