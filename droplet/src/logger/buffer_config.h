#pragma once

#include <cstddef>

namespace droplet::detail {
    
inline constexpr std::size_t LOG_MESSAGE_INLINE_CAPACITY = 128;

inline constexpr std::size_t PRINTF_FORMAT_INLINE_CAPACITY = 128;

inline constexpr std::size_t FORMATTED_RECORD_INLINE_CAPACITY = 256;

}  // namespace droplet::detail