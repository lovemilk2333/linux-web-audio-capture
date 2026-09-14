/**
 * @file log.cpp
 * @brief Minimal leveled logging to stderr.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace weba::log {
  namespace {
    std::atomic<level_t> g_level {level_t::info};
    std::atomic<bool> g_level_initialized {false};
    std::mutex g_write_lock;

    const char *to_string(level_t level) {
      switch (level) {
        case level_t::error:
          return "error";
        case level_t::warning:
          return "warning";
        case level_t::info:
          return "info";
        case level_t::debug:
          return "debug";
      }
      return "?";
    }

    /// Read $WEBA_LOG_LEVEL once, so the level can be set without a C API for it.
    void init_from_env() {
      bool expected = false;
      if (!g_level_initialized.compare_exchange_strong(expected, true)) {
        return;
      }
      if (const char *env = std::getenv("WEBA_LOG_LEVEL")) {
        level_t parsed {};
        if (parse_level(env, parsed)) {
          g_level.store(parsed);
        }
      }
    }
  }  // namespace

  void set_level(level_t level) {
    g_level_initialized.store(true);
    g_level.store(level);
  }

  level_t level() {
    init_from_env();
    return g_level.load();
  }

  bool enabled(level_t level) {
    return static_cast<int>(level) <= static_cast<int>(weba::log::level());
  }

  bool parse_level(const std::string &name, level_t &out) {
    if (name == "error") {
      out = level_t::error;
    }
    else if (name == "warning" || name == "warn") {
      out = level_t::warning;
    }
    else if (name == "info") {
      out = level_t::info;
    }
    else if (name == "debug") {
      out = level_t::debug;
    }
    else {
      return false;
    }
    return true;
  }

  line_t::line_t(level_t level): _level(level) {}

  line_t::~line_t() {
    std::scoped_lock lock {g_write_lock};
    std::fprintf(stderr, "[webaudio] %-7s %s\n", to_string(_level), _stream.str().c_str());
    std::fflush(stderr);
  }

}  // namespace weba::log
