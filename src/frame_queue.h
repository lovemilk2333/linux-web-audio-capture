/**
 * @file frame_queue.h
 * @brief Bounded queue of audio frames, handed from the capture thread to the
 *        consumer of weba_capture_read_frame().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace weba {

  /**
   * @brief A queue of fixed-size frames with drop-oldest overflow.
   *
   * Why a mutex rather than a lock-free ring: the producer is not a real-time
   * callback. It is a dedicated thread blocked in pa_simple_read(), which
   * already takes locks inside libpulse, and it produces one frame per audio
   * period (50 per second at the default 20 ms). A lock here costs nothing
   * measurable and makes drop-oldest overflow trivially correct.
   *
   * The producer never blocks: when the queue is full the oldest frame is
   * discarded, and the number discarded is reported to the consumer so it can
   * flag the gap.
   */
  class frame_queue_t {
  public:
    frame_queue_t() = default;
    frame_queue_t(const frame_queue_t &) = delete;
    frame_queue_t &operator=(const frame_queue_t &) = delete;

    /**
     * @brief Size the queue, discarding anything held. Safe to call again.
     *
     * @param slot_bytes Bytes per frame.
     * @param capacity Frames the queue can hold. Zero is raised to one.
     * @return False if the requested storage could not be allocated.
     */
    bool init(std::size_t slot_bytes, std::size_t capacity) {
      if (slot_bytes == 0) {
        return false;
      }
      std::scoped_lock lock {_lock};
      try {
        _storage.assign(std::max<std::size_t>(capacity, 1) * slot_bytes, 0);
      }
      catch (const std::bad_alloc &) {
        return false;
      }
      _slot_bytes = slot_bytes;
      _capacity = std::max<std::size_t>(capacity, 1);
      _count = 0;
      _read_index = 0;
      _dropped = 0;
      _closed = false;
      return true;
    }

    /// @brief Byte size of one frame.
    std::size_t slot_bytes() const {
      std::scoped_lock lock {_lock};
      return _slot_bytes;
    }

    /**
     * @brief Append a frame, discarding the oldest if the queue is full.
     *
     * Never blocks. Called from the capture thread.
     *
     * @return Frames discarded to make room, normally zero.
     */
    std::size_t push(const void *frame) {
      std::unique_lock lock {_lock};
      if (_closed || _slot_bytes == 0) {
        return 0;
      }

      std::size_t discarded = 0;
      if (_count == _capacity) {
        _read_index = (_read_index + 1) % _capacity;
        --_count;
        discarded = 1;
        ++_dropped;
      }

      const std::size_t write_index = (_read_index + _count) % _capacity;
      std::memcpy(_storage.data() + write_index * _slot_bytes, frame, _slot_bytes);
      ++_count;

      lock.unlock();
      _cv.notify_one();
      return discarded;
    }

    /**
     * @brief Wait for a frame and copy it out.
     *
     * Called from the consumer thread only.
     *
     * @param out Destination, holding at least slot_bytes().
     * @param discarded Receives frames dropped since the previous pop.
     * @param timeout How long to wait when empty.
     * @return 1 on a frame, 0 on timeout, -1 if the queue was closed.
     */
    int pop(void *out, std::size_t *discarded, std::chrono::milliseconds timeout) {
      std::unique_lock lock {_lock};
      _cv.wait_for(lock, timeout, [this] { return _count > 0 || _closed; });

      const std::size_t dropped = std::exchange(_dropped, 0);
      if (discarded) {
        *discarded = dropped;
      }

      if (_count == 0) {
        return _closed ? -1 : 0;
      }

      std::memcpy(out, _storage.data() + _read_index * _slot_bytes, _slot_bytes);
      _read_index = (_read_index + 1) % _capacity;
      --_count;
      return 1;
    }

    /// @brief Wake any waiter and refuse further pushes. Idempotent.
    void close() {
      {
        std::scoped_lock lock {_lock};
        _closed = true;
      }
      _cv.notify_all();
    }

    /// @brief Frames currently queued.
    std::size_t size() const {
      std::scoped_lock lock {_lock};
      return _count;
    }

  private:
    mutable std::mutex _lock;
    std::condition_variable _cv;
    std::vector<std::uint8_t> _storage;
    std::size_t _slot_bytes = 0;
    std::size_t _capacity = 0;
    std::size_t _count = 0;
    std::size_t _read_index = 0;
    std::size_t _dropped = 0;
    bool _closed = false;
  };

}  // namespace weba
