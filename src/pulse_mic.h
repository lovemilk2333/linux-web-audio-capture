/**
 * @file pulse_mic.h
 * @brief Blocking recording stream on a sink's monitor source.
 *
 * Ported from Sunshine's `mic_attr_t` and `platf::microphone()` in
 * src/platform/linux/audio.cpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from Sunshine (LizardByte), GPL-3.0. See NOTICE.
 */

#pragma once

#include "primitives.h"
#include "wsacapture.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace wsa::pulse {

  /// @brief The format a recording stream is opened with.
  struct mic_format_t {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    wsa_sample_format format = WSA_SAMPLE_F32LE;
    uint32_t frame_samples = 960;
  };

  /**
   * @brief A recording stream, recording exactly one monitor source.
   *
   * read_frame() blocks until the requested amount of audio is available, which
   * is the same contract as Sunshine's `mic_t::sample()`. There is no timeout:
   * a silent or suspended source blocks here indefinitely, which is why the
   * capture thread lives on its own and the caller consumes through a queue.
   */
  class mic_t {
  public:
    ~mic_t();

    mic_t(const mic_t &) = delete;
    mic_t &operator=(const mic_t &) = delete;

    /**
     * @brief Open a recording stream on @p monitor.
     *
     * @param monitor Monitor source name to record.
     * @param format Format to open the stream with.
     * @param error Receives the reason on failure.
     * @return The stream, or nullptr on failure.
     */
    static std::unique_ptr<mic_t> open(const std::string &monitor, const mic_format_t &format, std::string &error);

    /**
     * @brief Block until @p bytes of interleaved audio have been read.
     *
     * @param dst Destination buffer, holding at least @p bytes.
     * @param bytes Bytes to read; normally one whole frame.
     * @param error Receives the reason on failure.
     */
    bool read(void *dst, std::size_t bytes, std::string &error);

    /// @brief Size in bytes of one frame in the configured format.
    std::size_t frame_bytes(const mic_format_t &format) const;

  private:
    mic_t() = default;

    wsa::pa_simple_ptr_t _stream;
  };

  /**
   * @brief Whether @p channels is a layout this backend can open.
   *
   * The supported counts are the ones Opus can encode, which is also what the
   * channel map below is defined for.
   */
  bool channels_supported(uint32_t channels);

}  // namespace wsa::pulse
