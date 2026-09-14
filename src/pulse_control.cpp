/**
 * @file pulse_control.cpp
 * @brief Sink enumeration and monitor-source resolution.
 *
 * Ported from Sunshine's `pa::server_t` in src/platform/linux/audio.cpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from Sunshine (LizardByte), GPL-3.0. See NOTICE.
 */

#include "pulse_control.h"

#include "log.h"
#include "primitives.h"
#include "wsacapture.h"

#include <pulse/error.h>
#include <pulse/pulseaudio.h>

#include <chrono>
#include <utility>

namespace wsa::pulse {
  namespace {
    /* Sunshine waits on its alarms indefinitely. A bounded wait keeps a lost
     * callback from wedging the caller; 5 s is far longer than any local query
     * should take. */
    constexpr auto kQueryTimeout = std::chrono::milliseconds {5000};

    /// Outcome of an asynchronous PulseAudio query.
    struct query_result_t {
      bool ok = false;
      std::string value;
      std::string error;
    };

    using query_alarm_t = wsa::alarm_t<query_result_t>;

    query_result_t failure(std::string message) {
      query_result_t result;
      result.error = std::move(message);
      return result;
    }

    /**
     * @brief Callback for pa_context_get_server_info().
     *
     * Called exactly once, with a null @p info on failure.
     */
    void server_info_callback(pa_context *, const pa_server_info *info, void *userdata) {
      auto *alarm = static_cast<query_alarm_t *>(userdata);
      if (!info) {
        alarm->try_ring(failure("audio server returned no server info"));
        return;
      }
      if (!info->default_sink_name) {
        alarm->try_ring(failure("audio server reports no default sink"));
        return;
      }
      query_result_t result;
      result.ok = true;
      result.value = info->default_sink_name;
      alarm->try_ring(std::move(result));
    }

    /**
     * @brief Callback for pa_context_get_sink_info_by_name().
     *
     * Unless the sink is missing entirely, this is called once with the sink
     * followed by a final end-of-list call, so only the first result is kept.
     *
     * @param eol Negative on error, 0 for a result, positive at end of list.
     */
    void sink_info_callback(pa_context *ctx, const pa_sink_info *info, int eol, void *userdata) {
      auto *alarm = static_cast<query_alarm_t *>(userdata);
      if (eol < 0) {
        alarm->try_ring(failure(std::string {"sink lookup failed: "} + pa_strerror(pa_context_errno(ctx))));
        return;
      }
      if (eol > 0) {
        return;
      }
      if (!info || !info->monitor_source_name) {
        alarm->try_ring(failure("sink has no monitor source"));
        return;
      }
      query_result_t result;
      result.ok = true;
      result.value = info->monitor_source_name;
      alarm->try_ring(std::move(result));
    }

  }  // namespace

  control_t::~control_t() {
    shutdown();
  }

  void control_t::state_callback(pa_context *ctx, void *userdata) {
    auto *self = static_cast<control_t *>(userdata);
    bool notify = false;

    {
      std::scoped_lock lock {self->_state_lock};
      switch (pa_context_get_state(ctx)) {
        case PA_CONTEXT_READY:
          self->_ready = true;
          notify = true;
          break;
        case PA_CONTEXT_FAILED:
          self->_failed = true;
          notify = true;
          break;
        case PA_CONTEXT_TERMINATED:
          self->_terminated = true;
          notify = true;
          break;
        default:
          break;
      }
    }

    if (notify) {
      self->_state_cv.notify_all();
    }
  }

  int control_t::init() {
    _mainloop = pa_mainloop_new();
    if (!_mainloop) {
      set_error("pa_mainloop_new() failed");
      return WSA_ERR_PULSE;
    }

    pa_mainloop_api *api = pa_mainloop_get_api(_mainloop);
    _ctx = pa_context_new(api, "wsaudio");
    if (!_ctx) {
      set_error("pa_context_new() failed");
      return WSA_ERR_PULSE;
    }

    pa_context_set_state_callback(_ctx, state_callback, this);

    if (pa_context_connect(_ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      set_error(pulse_error("pa_context_connect()"));
      return WSA_ERR_PULSE;
    }

    /* The context only makes progress while its main loop runs, so the loop
     * needs its own thread before we can wait for the connection. */
    _thread = std::thread([this] {
      int retval = 0;
      pa_mainloop_run(_mainloop, &retval);
    });

    {
      std::unique_lock lock {_state_lock};
      const bool signalled = _state_cv.wait_for(lock, kQueryTimeout, [this] {
        return _ready || _failed;
      });
      if (!signalled) {
        set_error("timed out connecting to the audio server");
        return WSA_ERR_PULSE;
      }
      if (!_ready) {
        set_error("audio server connection failed");
        return WSA_ERR_PULSE;
      }
    }

    WSA_LOG_INFO << "connected to audio server";
    return WSA_OK;
  }

  void control_t::shutdown() {
    /* Mirror Sunshine's teardown order: disconnect, wait for the state to
     * settle, stop the loop, then release the objects. */
    if (_ctx) {
      pa_context_disconnect(_ctx);

      std::unique_lock lock {_state_lock};
      _state_cv.wait_for(lock, kQueryTimeout, [this] {
        return _terminated || _failed;
      });
    }

    if (_mainloop) {
      pa_mainloop_quit(_mainloop, 0);
    }

    if (_thread.joinable()) {
      _thread.join();
    }

    if (_ctx) {
      pa_context_unref(_ctx);
      _ctx = nullptr;
    }

    if (_mainloop) {
      pa_mainloop_free(_mainloop);
      _mainloop = nullptr;
    }

    std::scoped_lock lock {_state_lock};
    _ready = false;
  }

  bool control_t::ready() const {
    std::scoped_lock lock {_state_lock};
    return _ready && !_failed && !_terminated;
  }

  std::string control_t::error() const {
    std::scoped_lock lock {_error_lock};
    return _error;
  }

  std::string control_t::pulse_error(const char *what) const {
    return std::string {what} + " failed: " + pa_strerror(pa_context_errno(_ctx));
  }

  void control_t::set_error(const std::string &message) {
    {
      std::scoped_lock lock {_error_lock};
      _error = message;
    }
    WSA_LOG_ERROR << message;
  }

  bool control_t::query_default_sink(std::string &out) {
    auto alarm = wsa::make_alarm<query_result_t>();

    /* Unlike the sink query, the caller owns this operation's lifetime: the
     * callback may already have run by the time we wait on the alarm. */
    pa_operation *op = pa_context_get_server_info(_ctx, server_info_callback, alarm.get());
    if (!op) {
      set_error(pulse_error("pa_context_get_server_info()"));
      return false;
    }
    pa_operation_unref(op);

    query_result_t result;
    if (!alarm->wait(kQueryTimeout, result)) {
      set_error("timed out querying the default sink");
      return false;
    }
    if (!result.ok) {
      set_error(result.error);
      return false;
    }

    out = std::move(result.value);
    return true;
  }

  bool control_t::query_monitor(const std::string &sink, std::string &out) {
    auto alarm = wsa::make_alarm<query_result_t>();

    pa_operation *op = pa_context_get_sink_info_by_name(_ctx, sink.c_str(), sink_info_callback, alarm.get());
    if (!op) {
      set_error(pulse_error("pa_context_get_sink_info_by_name()"));
      return false;
    }
    pa_operation_unref(op);

    query_result_t result;
    if (!alarm->wait(kQueryTimeout, result)) {
      set_error("timed out looking up sink '" + sink + "'");
      return false;
    }
    if (!result.ok) {
      set_error(result.error);
      return false;
    }

    out = std::move(result.value);
    return true;
  }

  bool control_t::resolve_target(const std::string &requested_sink, target_t &out) {
    if (!_ctx || !ready()) {
      set_error("not connected to the audio server");
      return false;
    }

    std::string sink = requested_sink;
    if (sink.empty()) {
      if (!query_default_sink(sink)) {
        return false;
      }
      if (sink.empty()) {
        set_error("audio server reports an empty default sink");
        return false;
      }
    }

    std::string monitor;
    if (!query_monitor(sink, monitor)) {
      return false;
    }

    out.sink = std::move(sink);
    out.monitor = std::move(monitor);
    return true;
  }

}  // namespace wsa::pulse
