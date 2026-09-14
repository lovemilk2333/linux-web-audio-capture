/**
 * @file pulse_control.h
 * @brief Sink enumeration and monitor-source resolution.
 *
 * Ported from Sunshine's `pa::server_t` in src/platform/linux/audio.cpp, with
 * Sunshine's boost/safe:: infrastructure replaced by primitives.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from Sunshine (LizardByte), GPL-3.0. See NOTICE.
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

struct pa_context;
struct pa_mainloop;

namespace weba::pulse {

  /// @brief What to record: a sink and the monitor source belonging to it.
  struct target_t {
    std::string sink;     ///< Sink name, e.g. "alsa_output.pci-0000_01_00.1.hdmi-stereo-extra1"
    std::string monitor;  ///< Its monitor source, e.g. that plus ".monitor"
  };

  /**
   * @brief Owns a PulseAudio context and the thread running its main loop.
   *
   * The control API is asynchronous: requests are issued from the caller's
   * thread and their callbacks run on the main loop thread. Each query here
   * bridges the two with an alarm and a timeout, so a missing callback surfaces
   * as an error instead of a hang.
   */
  class control_t {
  public:
    control_t() = default;
    ~control_t();

    control_t(const control_t &) = delete;
    control_t &operator=(const control_t &) = delete;

    /**
     * @brief Connect to the audio server and wait until the context is ready.
     *
     * @return WEBA_OK, or WEBA_ERR_PULSE with the reason in error().
     */
    int init();

    /// @brief Disconnect and stop the main loop thread. Idempotent.
    void shutdown();

    /// @brief Whether the context reached PA_CONTEXT_READY and is still alive.
    bool ready() const;

    /// @brief Last error message, empty if none.
    std::string error() const;

    /**
     * @brief Resolve what to capture.
     *
     * @param requested_sink Sink name, or empty to use the server's default sink.
     * @param out Receives the resolved sink and monitor source.
     * @return True on success, false with the reason in error().
     */
    bool resolve_target(const std::string &requested_sink, target_t &out);

  private:
    bool query_default_sink(std::string &out);
    bool query_monitor(const std::string &sink, std::string &out);
    void set_error(const std::string &message);
    std::string pulse_error(const char *what) const;

    static void state_callback(pa_context *ctx, void *userdata);

    pa_mainloop *_mainloop = nullptr;
    pa_context *_ctx = nullptr;
    std::thread _thread;

    /* Connection state, written by the state callback on the main loop thread
     * and read by callers waiting in init()/shutdown(), so guarded by a mutex
     * and paired with a condition variable. */
    mutable std::mutex _state_lock;
    std::condition_variable _state_cv;
    bool _ready = false;
    bool _terminated = false;
    bool _failed = false;

    mutable std::mutex _error_lock;
    std::string _error;
  };

}  // namespace weba::pulse
