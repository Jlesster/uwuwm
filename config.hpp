#pragma once

#include <cstdint>

namespace cfg {

inline constexpr int      kTagCount = 9;
inline constexpr uint32_t kTagAll   = (1u << kTagCount) - 1;
}  // namespace cfg
