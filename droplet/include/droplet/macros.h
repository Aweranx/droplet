#pragma once

#include <droplet/logger/log.h>
#include <droplet/utils/system_utils.h>

#include <cassert>
#include <print>
#include <sstream>

#define DROPLET_ASSERT(x)                                          \
  if (!(x)) [[unlikely]] {                                         \
    const auto droplet_assert_backtrace = droplet::GetBacktrace(); \
    std::print(stderr, "{}:{} ASSERT FAILED: {}\nStacktrace:\n{}", \
               __FILE__, __LINE__, #x, droplet_assert_backtrace);    \
    DROPLET_LOG_FATAL(DROPLET_LOG_ROOT())                           \
        << "Assert failed: " << #x << "\nStacktrace:\n"           \
        << droplet_assert_backtrace;                                \
    assert(x);                                                       \
  }

#define DROPLET_ASSERT2(x, w)                                      \
  if (!(x)) [[unlikely]] {                                         \
    std::ostringstream droplet_assert_info;                       \
    droplet_assert_info << w;                                     \
    const auto droplet_assert_backtrace = droplet::GetBacktrace(); \
    std::print(stderr, "{}:{} Assert {} failed. [{}].\nStacktrace:\n{}", \
               __FILE__, __LINE__, #x, droplet_assert_info.str(), \
               droplet_assert_backtrace);                          \
    DROPLET_LOG_FATAL(DROPLET_LOG_ROOT())                           \
        << "Assert " << #x << " failed. [" << droplet_assert_info.str() \
        << "].\nStacktrace:\n" << droplet_assert_backtrace;       \
    assert(x);                                                       \
  }

#define ASSERT_RETVAL(x, val) \
  do {                        \
    if (x) [[likely]]         \
      break;                  \
    DROPLET_ASSERT(x);        \
    return val;               \
  } while (0)

#define ASSERT_RETVAL2(x, val, info) \
  do {                               \
    if (x) [[likely]]                \
      break;                         \
    DROPLET_ASSERT2(x, info);        \
    return val;                      \
  } while (0)

#define ASSERT_RETNONE(x) \
  do {                    \
    if (x) [[likely]]     \
      break;              \
    DROPLET_ASSERT(x);    \
    return;               \
  } while (0)

#define ASSERT_RETNONE2(x, info) \
  do {                           \
    if (x) [[likely]]            \
      break;                     \
    DROPLET_ASSERT2(x, info);    \
    return;                      \
  } while (0)

#define ASSERT_NOEFFECT(x) \
  do {                     \
    if (x) [[likely]]      \
      break;               \
    DROPLET_ASSERT(x);     \
  } while (0)

#define ASSERT_NOEFFECT2(x, info) \
  do {                            \
    if (x) [[likely]]             \
      break;                      \
    DROPLET_ASSERT2(x, info);     \
  } while (0)

#define ASSERT_CONTINUE(x) \
  if (!(x)) [[unlikely]] { \
    DROPLET_ASSERT(x);     \
    continue;              \
  } else {                 \
  }

#define ASSERT_CONTINUE2(x, info) \
  if (!(x)) [[unlikely]] {        \
    DROPLET_ASSERT2(x, info);     \
    continue;                     \
  } else {                        \
  }

#define ASSERT_BREAK(x)    \
  if (!(x)) [[unlikely]] { \
    DROPLET_ASSERT(x);     \
    break;                 \
  } else {                 \
  }

#define ASSERT_BREAK2(x, info) \
  if (!(x)) [[unlikely]] {     \
    DROPLET_ASSERT2(x, info);  \
    break;                     \
  } else {                     \
  }
