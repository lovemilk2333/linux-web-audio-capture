/**
 * @file log.h
 * @brief Minimal leveled logging to stderr, replacing Sunshine's boost.log use.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sstream>
#include <string>

namespace weba::log {

  enum class level_t {
    error = 0,
    warning = 1,
    info = 2,
    debug = 3
  };

  /// @brief Set the minimum level that is emitted.
  void set_level(level_t level);

  /// @brief Current minimum level. Defaults to info, or to $WEBA_LOG_LEVEL.
  level_t level();

  /// @brief Whether a level would be emitted. Used to skip formatting work.
  bool enabled(level_t level);

  /// @brief Parse "error", "warning", "info" or "debug". Returns false if unknown.
  bool parse_level(const std::string &name, level_t &out);

  /**
   * @brief One log line, built with operator<< and emitted on destruction.
   *
   * Not atomic across threads for a single line's ordering guarantees beyond
   * what the underlying stream write provides, which is enough here.
   */
  class line_t {
  public:
    explicit line_t(level_t level);
    ~line_t();

    line_t(const line_t &) = delete;
    line_t &operator=(const line_t &) = delete;

    template<class T>
    line_t &operator<<(T &&value) {
      _stream << std::forward<T>(value);
      return *this;
    }

  private:
    level_t _level;
    std::ostringstream _stream;
  };

}  // namespace weba::log

/* Logging must never run on the real-time path; these are only used from the
 * control layer, the capture thread and the public entry points. */
#define WEBA_LOG_ERROR \
  if (weba::log::enabled(weba::log::level_t::error)) \
  weba::log::line_t {weba::log::level_t::error}
#define WEBA_LOG_WARNING \
  if (weba::log::enabled(weba::log::level_t::warning)) \
  weba::log::line_t {weba::log::level_t::warning}
#define WEBA_LOG_INFO \
  if (weba::log::enabled(weba::log::level_t::info)) \
  weba::log::line_t {weba::log::level_t::info}
#define WEBA_LOG_DEBUG \
  if (weba::log::enabled(weba::log::level_t::debug)) \
  weba::log::line_t {weba::log::level_t::debug}
