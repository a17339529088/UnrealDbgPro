#pragma once
#include <stdint.h>
#include <limits.h>

namespace vbs {
struct OffsetEstimate {
    int64_t remote_minus_reference;
    uint64_t uncertainty_ticks;
    uint64_t bracket_ticks;
};
}
