/**
 * @file capture.cpp
 * @brief Implementation of the public C ABI: lifecycle, the capture thread, the
 *        reinit path and fixed-rate silence generation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from Sunshine (LizardByte), GPL-3.0. See NOTICE.
 */

#include "webacapture.h"

#include "frame_queue.h"
#include "log.h"
#include "primitives.h"
#include "pulse_control.h"
#include "pulse_mic.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
  constexpr uint32_t kDefaultSampleRate = 48000;
  constexpr uint32_t kDefaultChannels = 2;
  constexpr uint32_t kDefaultFrameSamples = 960;  ///< 20 ms at 48 kHz
  constexpr uint32_t kDefaultQueueFrames = 100;   ///< ~2 s at 20 ms
  constexpr uint32_t kDefaultSinkRecheckMs = 1000;

  /* After a failure the stream is reopened on this period, mirroring Sunshine's
   * retry gate: fast enough to recover from a device switch, slow enough not to
   * spin while the device stays away. */
  constexpr auto kReinitDelay = std::chrono::milliseconds {1000};

  /* weba_capture_stop() waits this long for the capture thread. See the note on
   * pa_simple_read in stop() for why the wait is bounded. */
  constexpr auto kJoinTimeout = std::chrono::milliseconds {3000};

  uint32_t frame_duration_ms(uint32_t frame_samples, uint32_t sample_rate) {
    return static_cast<uint32_t>((static_cast<uint64_t>(frame_samples) * 1000) / sample_rate);
  }
}  // namespace

/**
 * @brief The opaque handle behind weba_capture.
 */
struct weba_capture {
  /* Resolved configuration. */
  uint32_t sample_rate = kDefaultSampleRate;
  uint32_t channels = kDefaultChannels;
  uint32_t frame_samples = kDefaultFrameSamples;
  weba_sample_format format = WEBA_SAMPLE_F32LE;
  std::string requested_sink;
  uint32_t queue_frames = kDefaultQueueFrames;
  bool fixed_rate = true;
  uint32_t sink_recheck_ms = kDefaultSinkRecheckMs;

  /* The monitor currently being recorded, used to notice when the default sink
   * has moved somewhere else. */
  std::string current_monitor;
  std::chrono::steady_clock::time_point next_sink_check {};

  weba::pulse::control_t control;
  weba::frame_queue_t queue;

  std::thread thread;
  std::atomic<bool> running {false};
  std::atomic<bool> stop_requested {false};

  /* Flags the capture thread needs to hand to the next frame the consumer sees,
   * such as a stream reopen that happened between two reads. */
  std::atomic<uint32_t> pending_flags {0};

  mutable std::mutex format_lock;
  weba_format_info format_info {};

  mutable std::mutex error_lock;
  std::string error;

  /* Fixed-rate pacing state, owned by the consumer. */
  std::chrono::steady_clock::time_point next_deadline {};
  bool deadline_valid = false;

  std::mutex stop_lock;
  std::condition_variable stop_cv;

  /* Set when the capture thread could not be joined and is therefore still
   * running. The handle is never freed in that case. */
  bool thread_abandoned = false;

  std::size_t frame_bytes() const {
    std::size_t bytes_per_sample = 0;
    switch (format) {
      case WEBA_SAMPLE_S16LE:
        bytes_per_sample = 2;
        break;
      case WEBA_SAMPLE_F32LE:
      case WEBA_SAMPLE_S32LE:
        bytes_per_sample = 4;
        break;
    }
    return static_cast<std::size_t>(frame_samples) * channels * bytes_per_sample;
  }

  void set_error(const std::string &message) {
    {
      std::scoped_lock lock {error_lock};
      error = message;
    }
    WEBA_LOG_ERROR << message;
  }

  std::string get_error() const {
    std::scoped_lock lock {error_lock};
    return error;
  }

  /// @brief Sleep for @p delay, returning true if a stop was requested first.
  bool wait_for_stop(std::chrono::milliseconds delay) {
    std::unique_lock lock {stop_lock};
    return stop_cv.wait_for(lock, delay, [this] { return stop_requested.load(); });
  }

  /// @brief Sleep for @p delay, waking early if a stop is requested.
  void sleep_interruptible(std::chrono::milliseconds delay) {
    std::unique_lock lock {stop_lock};
    stop_cv.wait_for(lock, delay, [this] { return stop_requested.load(); });
  }

  void publish_format(const weba::pulse::target_t &target) {
    std::scoped_lock lock {format_lock};
    format_info.sample_rate = sample_rate;
    format_info.channels = channels;
    format_info.frame_samples = frame_samples;
    format_info.format = format;
    std::snprintf(format_info.sink_name, sizeof(format_info.sink_name), "%s", target.sink.c_str());
    std::snprintf(format_info.monitor_name, sizeof(format_info.monitor_name), "%s", target.monitor.c_str());
  }

  void capture_loop();
};

void weba_capture::capture_loop() {
  const weba::pulse::mic_format_t mic_format {sample_rate, channels, format, frame_samples};
  const std::size_t bytes = frame_bytes();

  std::vector<std::uint8_t> buffer(bytes, 0);
  std::unique_ptr<weba::pulse::mic_t> mic;
  bool had_stream = false;

  while (!stop_requested.load()) {
    if (!mic) {
      /* Resolve on every (re)open, so switching the default sink is picked up
       * the same way Sunshine picks it up on its reinit path. */
      weba::pulse::target_t target;
      if (!control.resolve_target(requested_sink, target)) {
        set_error(control.error());
        if (wait_for_stop(kReinitDelay)) {
          break;
        }
        continue;
      }

      publish_format(target);

      std::string open_error;
      mic = weba::pulse::mic_t::open(target.monitor, mic_format, open_error);
      if (!mic) {
        set_error(open_error);
        pending_flags.fetch_or(WEBA_FRAME_REINIT | WEBA_FRAME_DISCONTINUITY);
        if (wait_for_stop(kReinitDelay)) {
          break;
        }
        continue;
      }

      current_monitor = target.monitor;
      next_sink_check = std::chrono::steady_clock::now() + std::chrono::milliseconds {sink_recheck_ms};

      /* Only a reopen is a discontinuity; the first stream has nothing before
       * it to be discontinuous with. */
      if (had_stream) {
        pending_flags.fetch_or(WEBA_FRAME_REINIT | WEBA_FRAME_DISCONTINUITY);
      }
      had_stream = true;
    }

    std::string read_error;
    if (!mic->read(buffer.data(), bytes, read_error)) {
      set_error(read_error);
      mic.reset();
      pending_flags.fetch_or(WEBA_FRAME_REINIT | WEBA_FRAME_DISCONTINUITY);
      if (wait_for_stop(kReinitDelay)) {
        break;
      }
      continue;
    }

    queue.push(buffer.data());

    /* Following the default sink needs polling: switching the default does not
     * disturb the stream we already hold, because the monitor of the old sink
     * keeps delivering, so nothing fails and the reopen path never runs. Only
     * done when the sink is unpinned, since a pinned one must not be followed
     * away from. */
    if (requested_sink.empty() && sink_recheck_ms > 0 && std::chrono::steady_clock::now() >= next_sink_check) {
      next_sink_check = std::chrono::steady_clock::now() + std::chrono::milliseconds {sink_recheck_ms};

      weba::pulse::target_t target;
      if (control.resolve_target("", target) && target.monitor != current_monitor) {
        WEBA_LOG_INFO << "default sink moved to " << target.sink << ", reopening capture";
        /* Flagged by the reopen path below, which is also what reports a
         * reopen caused by a failure, so this is not flagged here: doing both
         * would report a single reopen twice. */
        mic.reset();
      }
    }
  }

  /* Wakes whoever is waiting in weba_capture_stop(). The wait predicate reads
   * this atomic, so notifying after the store cannot be lost. */
  running.store(false);
  stop_cv.notify_all();

  WEBA_LOG_INFO << "capture thread exiting";
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void weba_config_defaults(weba_config *cfg) {
  if (!cfg) {
    return;
  }
  cfg->sample_rate = kDefaultSampleRate;
  cfg->channels = kDefaultChannels;
  cfg->format = WEBA_SAMPLE_F32LE;
  cfg->frame_samples = kDefaultFrameSamples;
  cfg->sink = nullptr;
  cfg->ring_frames = kDefaultQueueFrames;
  cfg->fixed_rate = 1;
  cfg->sink_recheck_ms = kDefaultSinkRecheckMs;
}

int weba_capture_create(const weba_config *cfg, weba_capture **out) {
  if (!out) {
    return WEBA_ERR_INVAL;
  }
  *out = nullptr;

  auto capture = std::unique_ptr<weba_capture> {new (std::nothrow) weba_capture {}};
  if (!capture) {
    return WEBA_ERR_NOMEM;
  }

  /* Apply the configuration, resolving the documented zero-value defaults. */
  if (cfg) {
    capture->sample_rate = cfg->sample_rate ? cfg->sample_rate : kDefaultSampleRate;
    capture->channels = cfg->channels ? cfg->channels : kDefaultChannels;
    capture->frame_samples = cfg->frame_samples ? cfg->frame_samples : kDefaultFrameSamples;
    capture->format = cfg->format;
    capture->requested_sink = cfg->sink ? cfg->sink : "";
    capture->queue_frames = cfg->ring_frames ? cfg->ring_frames : kDefaultQueueFrames;
    capture->fixed_rate = cfg->fixed_rate != 0;
    capture->sink_recheck_ms = cfg->sink_recheck_ms ? cfg->sink_recheck_ms : kDefaultSinkRecheckMs;
  }

  if (capture->sample_rate == 0 || capture->frame_samples == 0 || capture->channels == 0) {
    return WEBA_ERR_INVAL;
  }
  if (!weba::pulse::channels_supported(capture->channels)) {
    return WEBA_ERR_FORMAT;
  }
  if (capture->format != WEBA_SAMPLE_F32LE && capture->format != WEBA_SAMPLE_S16LE &&
      capture->format != WEBA_SAMPLE_S32LE) {
    return WEBA_ERR_FORMAT;
  }

  if (!capture->queue.init(capture->frame_bytes(), capture->queue_frames)) {
    return WEBA_ERR_NOMEM;
  }

  *out = capture.release();
  return WEBA_OK;
}

int weba_capture_start(weba_capture *c) {
  if (!c) {
    return WEBA_ERR_INVAL;
  }
  if (c->running.load()) {
    return WEBA_ERR_STATE;
  }

  const int status = c->control.init();
  if (status != WEBA_OK) {
    c->set_error(c->control.error());
    return status;
  }

  /* Resolve the sink here rather than leaving it to the capture thread, so a
   * bad sink name fails the start and the format is readable immediately
   * afterwards. The thread resolves again on every (re)open, which is what
   * makes it follow a changed default sink. */
  weba::pulse::target_t target;
  if (!c->control.resolve_target(c->requested_sink, target)) {
    c->set_error(c->control.error());
    c->control.shutdown();
    return WEBA_ERR_PULSE;
  }
  c->publish_format(target);

  c->stop_requested.store(false);
  c->running.store(true);
  c->thread = std::thread([c] { c->capture_loop(); });

  WEBA_LOG_INFO << "capture started: " << c->sample_rate << " Hz, " << c->channels << " ch, "
               << c->frame_samples << " frames/frame (" << frame_duration_ms(c->frame_samples, c->sample_rate)
               << " ms), sink '" << (c->requested_sink.empty() ? "default" : c->requested_sink) << "'";
  return WEBA_OK;
}

int weba_capture_read_frame(weba_capture *c, void *dst, uint32_t dst_capacity_frames, weba_frame_info *info,
                           int timeout_ms) {
  if (!c || !dst) {
    return WEBA_ERR_INVAL;
  }
  if (!c->running.load()) {
    return WEBA_ERR_STATE;
  }
  if (dst_capacity_frames < c->frame_samples) {
    return WEBA_ERR_INVAL;
  }
  if (timeout_ms < 0) {
    timeout_ms = 0;
  }

  const std::size_t bytes = c->frame_bytes();
  const auto period = std::chrono::milliseconds {frame_duration_ms(c->frame_samples, c->sample_rate)};
  const auto call_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds {timeout_ms};
  const auto zero = std::chrono::milliseconds {0};
  const auto one_ms = std::chrono::milliseconds {1};

  std::size_t total_discarded = 0;

  for (;;) {
    const auto now = std::chrono::steady_clock::now();

    if (!c->fixed_rate) {
      /* Without fixed-rate output the caller's timeout is the only deadline,
       * and a timeout is reported honestly rather than filled in. */
      auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(call_deadline - now);
      if (wait < zero) {
        wait = zero;
      }

      std::size_t discarded = 0;
      const int result = c->queue.pop(dst, &discarded, wait);
      if (result < 0) {
        return WEBA_ERR_STATE;
      }
      total_discarded += discarded;

      if (result == 0) {
        if (std::chrono::steady_clock::now() >= call_deadline) {
          return 0;
        }
        continue;
      }
      break;
    }

    if (!c->deadline_valid) {
      c->next_deadline = now + period;
      c->deadline_valid = true;
    }

    /* The frame is not due yet. Waiting here rather than returning whatever is
     * at the head of the queue is what holds the cadence when the source
     * delivers a burst: the burst stays queued and is drained one frame per
     * period, instead of being handed out as fast as it arrives. */
    if (now < c->next_deadline) {
      auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(c->next_deadline - now) + one_ms;
      const auto until_call_deadline = std::chrono::duration_cast<std::chrono::milliseconds>(call_deadline - now);
      if (until_call_deadline < wait) {
        wait = until_call_deadline;
      }
      if (wait <= zero) {
        return 0;
      }
      c->sleep_interruptible(wait);
      continue;
    }

    /* The frame is due: take real audio if any has arrived, otherwise
     * synthesize it so the stream keeps flowing while the desktop is silent. */
    std::size_t discarded = 0;
    const int result = c->queue.pop(dst, &discarded, zero);
    if (result < 0) {
      return WEBA_ERR_STATE;
    }
    total_discarded += discarded;

    /* Built locally rather than OR-ed into *info, which the caller is not
     * required to initialize. */
    uint32_t flags = c->pending_flags.exchange(0);
    if (total_discarded > 0) {
      flags |= WEBA_FRAME_DISCONTINUITY;
    }
    if (result == 0) {
      std::memset(dst, 0, bytes);
      flags |= WEBA_FRAME_SILENCE | WEBA_FRAME_UNDERRUN;
    }

    /* Advance by exactly one period so the cadence does not drift, but resync
     * rather than burst through a backlog if we fell far behind. */
    c->next_deadline += period;
    const auto after = std::chrono::steady_clock::now();
    if (c->next_deadline < after) {
      c->next_deadline = after + period;
    }

    if (info) {
      info->timestamp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after.time_since_epoch()).count());
      info->flags = flags;
      info->dropped_frames = static_cast<uint32_t>(total_discarded);
    }
    return 1;
  }

  if (info) {
    info->timestamp_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count());
    info->flags = c->pending_flags.exchange(0) |
                  (total_discarded > 0 ? WEBA_FRAME_DISCONTINUITY : 0u);
    info->dropped_frames = static_cast<uint32_t>(total_discarded);
  }

  return 1;
}

int weba_capture_get_format(weba_capture *c, weba_format_info *out) {
  if (!c || !out) {
    return WEBA_ERR_INVAL;
  }
  std::scoped_lock lock {c->format_lock};
  *out = c->format_info;
  return WEBA_OK;
}

int weba_capture_is_running(weba_capture *c) {
  return c && c->running.load() ? 1 : 0;
}

void weba_capture_stop(weba_capture *c) {
  if (!c) {
    return;
  }

  {
    std::scoped_lock lock {c->stop_lock};
    c->stop_requested.store(true);
  }
  c->stop_cv.notify_all();

  /* Unblock a consumer waiting in read_frame(). */
  c->queue.close();

  if (c->thread.joinable()) {
    /* pa_simple has no way to cancel a pending pa_simple_read, so a capture
     * thread blocked on a source that never delivers cannot be interrupted. On
     * a normal source the read returns every audio period and this join is
     * immediate; the timeout only covers a source that has gone silent in a way
     * that does not error out the stream.
     *
     * If the thread cannot be joined it is still running and owns this object's
     * state, so the handle is deliberately not freed below. Leaking a handle on
     * that path is the safe outcome; freeing it would be a use-after-free. */
    std::unique_lock lock {c->stop_lock};
    const bool joined = c->stop_cv.wait_for(lock, kJoinTimeout, [c] { return !c->running.load(); });
    lock.unlock();

    if (joined) {
      c->thread.join();
    }
    else {
      WEBA_LOG_ERROR << "capture thread did not exit within " << kJoinTimeout.count()
                    << " ms; it is blocked in pa_simple_read() on a source that is not delivering audio. "
                       "Abandoning the thread and leaking this handle rather than freeing memory in use.";
      c->thread.detach();
      c->thread_abandoned = true;
    }
  }

  if (!c->thread_abandoned) {
    c->control.shutdown();
    c->running.store(false);
  }
}

void weba_capture_destroy(weba_capture *c) {
  if (!c) {
    return;
  }

  weba_capture_stop(c);

  if (c->thread_abandoned) {
    /* The capture thread is still using this object. See weba_capture_stop(). */
    return;
  }

  delete c;
}

const char *weba_capture_last_error(weba_capture *c) {
  static const std::string empty;
  if (!c) {
    return empty.c_str();
  }
  /* Stored per handle, so the string stays valid until the next call. */
  static thread_local std::string buffer;
  buffer = c->get_error();
  return buffer.c_str();
}

const char *weba_version_string(void) {
  return "1.0.0";
}

const char *weba_strerror(int status) {
  switch (status) {
    case WEBA_OK:
      return "success";
    case WEBA_ERR_INVAL:
      return "invalid argument";
    case WEBA_ERR_NOMEM:
      return "out of memory";
    case WEBA_ERR_PULSE:
      return "audio server error";
    case WEBA_ERR_FORMAT:
      return "unsupported audio format";
    case WEBA_ERR_STATE:
      return "invalid state for this operation";
    default:
      break;
  }
  return "unknown error";
}
