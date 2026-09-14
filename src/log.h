/**
 * @file log.h
 * @brief Minimal leveled logging to stderr, replacing Sunshine's boost.log use.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sstream>
#include <string>

namespace wsa::log {

  enum class level_t {
    error = 0,
    warning = 1,
    info = 2,
    debug = 3
  };

  /// @brief Set the minimum level that is emitted.
  void set_level(level_t level);

  /// @brief Current minimum level. Defaults to info, or to $WSA_LOG_LEVEL.
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

}  // namespace wsa::log

/* Logging must never run on the real-time path; these are only used from the
 * control layer, the capture thread and the public entry points. */
#define WSA_LOG_ERROR \
  if (wsa::log::enabled(wsa::log::level_t::error)) \
  wsa::log::line_t {wsa::log::level_t::error}
#define WSA_LOG_WARNING \
  if (wsa::log::enabled(wsa::log::level_t::warning)) \
  wsa::log::line_t {wsa::log::level_t::warning}
#define WSA_LOG_INFO \
  if (wsa::log::enabled(wsa::log::level_t::info)) \
  wsa::log::line_t {wsa::log::level_t::info}
#define WSA_LOG_DEBUG \
  if (wsa::log::enabled(wsa::log::level_t::debug)) \
  wsa::log::line_t {wsa::log::level_t::debug}
