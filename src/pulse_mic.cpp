/**
 * @file pulse_mic.cpp
 * @brief Blocking recording stream on a sink's monitor source.
 *
 * Ported from Sunshine's `mic_attr_t` and `platf::microphone()` in
 * src/platform/linux/audio.cpp. Sunshine opens the stream as float32 only; the
 * format table below is the one generalization made here.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from Sunshine (LizardByte), GPL-3.0. See NOTICE.
 */

#include "pulse_mic.h"

#include "log.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <cstring>

namespace weba {
  void pa_simple_deleter_t::operator()(struct pa_simple *stream) const noexcept {
    if (stream) {
      pa_simple_free(stream);
    }
  }
}  // namespace weba

namespace weba::pulse {
  namespace {
    /* Channel layouts indexed the way Sunshine's `position_mapping[]` is: front
     * to back, matching the speaker order Opus uses. */
    constexpr pa_channel_position_t kStereoMap[] = {
      PA_CHANNEL_POSITION_FRONT_LEFT,
      PA_CHANNEL_POSITION_FRONT_RIGHT
    };

    constexpr pa_channel_position_t kSurround51Map[] = {
      PA_CHANNEL_POSITION_FRONT_LEFT,
      PA_CHANNEL_POSITION_FRONT_RIGHT,
      PA_CHANNEL_POSITION_FRONT_CENTER,
      PA_CHANNEL_POSITION_LFE,
      PA_CHANNEL_POSITION_REAR_LEFT,
      PA_CHANNEL_POSITION_REAR_RIGHT
    };

    constexpr pa_channel_position_t kSurround71Map[] = {
      PA_CHANNEL_POSITION_FRONT_LEFT,
      PA_CHANNEL_POSITION_FRONT_RIGHT,
      PA_CHANNEL_POSITION_FRONT_CENTER,
      PA_CHANNEL_POSITION_LFE,
      PA_CHANNEL_POSITION_REAR_LEFT,
      PA_CHANNEL_POSITION_REAR_RIGHT,
      PA_CHANNEL_POSITION_SIDE_LEFT,
      PA_CHANNEL_POSITION_SIDE_RIGHT
    };

    /// @brief Build the PulseAudio channel map for @p channels.
    bool build_channel_map(uint32_t channels, pa_channel_map &map) {
      const pa_channel_position_t *source = nullptr;
      switch (channels) {
        case 1:
          map.channels = 1;
          map.map[0] = PA_CHANNEL_POSITION_MONO;
          return true;
        case 2:
          source = kStereoMap;
          break;
        case 6:
          source = kSurround51Map;
          break;
        case 8:
          source = kSurround71Map;
          break;
        default:
          return false;
      }

      map.channels = static_cast<uint8_t>(channels);
      std::copy_n(source, channels, map.map);
      return true;
    }

    /// @brief Map our sample format onto PulseAudio's.
    bool map_sample_format(weba_sample_format format, pa_sample_format_t &out, uint32_t &bytes_per_sample) {
      switch (format) {
        case WEBA_SAMPLE_F32LE:
          out = PA_SAMPLE_FLOAT32LE;
          bytes_per_sample = 4;
          return true;
        case WEBA_SAMPLE_S16LE:
          out = PA_SAMPLE_S16LE;
          bytes_per_sample = 2;
          return true;
        case WEBA_SAMPLE_S32LE:
          out = PA_SAMPLE_S32LE;
          bytes_per_sample = 4;
          return true;
      }
      return false;
    }

    std::size_t bytes_per_sample_of(weba_sample_format format) {
      switch (format) {
        case WEBA_SAMPLE_S16LE:
          return 2;
        case WEBA_SAMPLE_F32LE:
        case WEBA_SAMPLE_S32LE:
          return 4;
      }
      return 0;
    }

  }  // namespace

  bool channels_supported(uint32_t channels) {
    return channels == 1 || channels == 2 || channels == 6 || channels == 8;
  }

  mic_t::~mic_t() = default;

  std::size_t mic_t::frame_bytes(const mic_format_t &format) const {
    return static_cast<std::size_t>(format.frame_samples) * format.channels * bytes_per_sample_of(format.format);
  }

  std::unique_ptr<mic_t> mic_t::open(const std::string &monitor, const mic_format_t &format, std::string &error) {
    if (monitor.empty()) {
      error = "empty monitor source name";
      return nullptr;
    }
    if (!channels_supported(format.channels)) {
      error = "unsupported channel count " + std::to_string(format.channels) + " (expected 1, 2, 6 or 8)";
      return nullptr;
    }
    if (format.frame_samples == 0) {
      error = "frame size must not be zero";
      return nullptr;
    }

    pa_sample_format_t pa_format {};
    uint32_t bytes_per_sample = 0;
    if (!map_sample_format(format.format, pa_format, bytes_per_sample)) {
      error = "unsupported sample format";
      return nullptr;
    }

    pa_sample_spec spec {};
    spec.format = pa_format;
    spec.rate = format.sample_rate;
    spec.channels = static_cast<uint8_t>(format.channels);

    pa_channel_map channel_map {};
    if (!build_channel_map(format.channels, channel_map)) {
      error = "no channel map for " + std::to_string(format.channels) + " channels";
      return nullptr;
    }

    /* Fragments are sized to exactly one frame, so a read returns one frame and
     * the stream is paced by the audio clock rather than by a sleep. */
    const auto fragsize = static_cast<uint32_t>(format.frame_samples) * format.channels * bytes_per_sample;
    pa_buffer_attr attr {};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = fragsize;

    int status = PA_OK;
    auto stream = weba::pa_simple_ptr_t {pa_simple_new(nullptr, "webaudio", PA_STREAM_RECORD, monitor.c_str(),
                                                      "webaudio-record", &spec, &channel_map, &attr, &status)};
    if (!stream) {
      error = std::string {"pa_simple_new() failed: "} + pa_strerror(status);
      return nullptr;
    }

    auto mic = std::unique_ptr<mic_t> {new mic_t {}};
    mic->_stream = std::move(stream);

    WEBA_LOG_INFO << "recording " << monitor << " at " << format.sample_rate << " Hz, " << format.channels
                 << " ch, " << format.frame_samples << " frames/read";
    return mic;
  }

  bool mic_t::read(void *dst, std::size_t bytes, std::string &error) {
    if (!_stream) {
      error = "recording stream is not open";
      return false;
    }

    int status = PA_OK;
    if (pa_simple_read(_stream.get(), dst, bytes, &status) < 0) {
      error = std::string {"pa_simple_read() failed: "} + pa_strerror(status);
      return false;
    }
    return true;
  }

}  // namespace weba::pulse
