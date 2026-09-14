/**
 * @file primitives.h
 * @brief Minimal stand-ins for the Sunshine infrastructure helpers the audio
 *        backend relies on (boost.log, safe::alarm_raw_t, util::safe_ptr,
 *        util::fail_guard), so that this library depends only on libpulse and
 *        the C++ standard library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from Sunshine (LizardByte), GPL-3.0. See NOTICE.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

/* libpulse-simple's opaque handle. Declared at global scope because that is
 * where libpulse declares it: a forward declaration inside namespace wsa would
 * introduce a distinct, incompatible type. This header stays free of
 * PulseAudio includes on purpose. */
struct pa_simple;

namespace wsa {

  /**
   * @brief A one-shot, single-value handoff from a callback to a waiting thread.
   *
   * This is the same contract as Sunshine's `safe::alarm_raw_t<T>`: PulseAudio's
   * control API is asynchronous, so its callbacks ring the alarm and the calling
   * thread waits on it. `ring()` may be called from any thread; only one waiter
   * is supported, which matches how the audio control layer uses it.
   *
   * Unlike Sunshine's version, `wait()` takes a timeout, so a missing callback
   * surfaces as a timeout instead of hanging the caller forever.
   */
  template<class T>
  class alarm_t {
  public:
    /**
     * @brief Deliver a value and wake the waiter.
     *
     * Safe to call from a PulseAudio callback thread.
     */
    void ring(T value) {
      {
        std::scoped_lock lock {_lock};
        _value = std::move(value);
        _raised = true;
      }
      _cv.notify_all();
    }

    /**
     * @brief Deliver a value only if none is pending yet.
     *
     * PulseAudio may invoke a query callback more than once (the common case is
     * one call with the result and a final end-of-list call). This lets a
     * callback ring exactly once and ignore the rest.
     *
     * @return True if this call delivered the value, false if one was pending.
     */
    bool try_ring(T value) {
      {
        std::scoped_lock lock {_lock};
        if (_raised) {
          return false;
        }
        _value = std::move(value);
        _raised = true;
      }
      _cv.notify_all();
      return true;
    }

    /**
     * @brief Wait for a value to be rung.
     *
     * @param timeout How long to wait; negative waits indefinitely.
     * @param out Receives the value when one arrived.
     * @return True if a value was delivered, false on timeout.
     */
    bool wait(std::chrono::milliseconds timeout, T &out) {
      std::unique_lock lock {_lock};
      const bool ok = timeout.count() < 0 ?
                        (_cv.wait(lock, [this] { return _raised; }), true) :
                        _cv.wait_for(lock, timeout, [this] { return _raised; });
      if (!ok) {
        return false;
      }
      out = std::move(_value);
      _raised = false;
      return true;
    }

    /**
     * @brief Discard any pending value so the next wait() blocks afresh.
     *
     * Used before re-issuing a request on an alarm that may already have been
     * rung by a stale callback.
     */
    void reset() {
      std::scoped_lock lock {_lock};
      _raised = false;
    }

  private:
    std::mutex _lock;
    std::condition_variable _cv;
    bool _raised = false;
    T _value {};
  };

  /**
   * @brief Allocate an alarm and return it shared, so raw pointers handed to
   *        PulseAudio callbacks stay valid for the lifetime of the request.
   */
  template<class T>
  std::shared_ptr<alarm_t<T>> make_alarm() {
    return std::make_shared<alarm_t<T>>();
  }

  /**
   * @brief RAII scope guard, the equivalent of Sunshine's `util::fail_guard`.
   *
   * Runs the callable on destruction unless dismiss() was called.
   */
  template<class F>
  class scope_guard_t {
  public:
    explicit scope_guard_t(F fn): _fn(std::move(fn)) {}
    scope_guard_t(const scope_guard_t &) = delete;
    scope_guard_t &operator=(const scope_guard_t &) = delete;
    scope_guard_t(scope_guard_t &&other) noexcept: _fn(std::move(other._fn)), _armed(std::exchange(other._armed, false)) {}
    ~scope_guard_t() {
      if (_armed) {
        _fn();
      }
    }
    /// @brief Disarm the guard so it does not run at scope exit.
    void dismiss() { _armed = false; }

  private:
    F _fn;
    bool _armed = true;
  };

  template<class F>
  scope_guard_t<F> on_scope_exit(F fn) {
    return scope_guard_t<F> {std::move(fn)};
  }

  /**
   * @brief Deleter for libpulse-simple handles, for use with std::unique_ptr.
   *
   * Sunshine uses `util::safe_ptr<pa_simple, pa_simple_free>` for this.
   */
  struct pa_simple_deleter_t {
    void operator()(::pa_simple *p) const noexcept;
  };

  /// @brief Owning pointer to a pa_simple recording stream.
  using pa_simple_ptr_t = std::unique_ptr<::pa_simple, pa_simple_deleter_t>;

}  // namespace wsa
