#pragma once
#include "Core/Timing.h"
#include <atomic>
extern "C" uint64_t VbsReadSerializedTsc();
namespace vbs { namespace timing {
enum Flags : uint32_t { Valid=1, Regressed=2, Overflow=4, Uncalibrated=8, WrongEpoch=16 };
struct Stamp { uint64_t tsc; uint32_t cpu, epoch; };
struct HardwareEstimate { uint64_t minimum, maximum, midpoint, uncertainty; };
struct Report {
    Stamp begin, end;
    uint64_t delta, estimated_hardware, hardware_uncertainty, estimated_total_observer;
    uint32_t flags;
};
/** Controlled experiment residual; excludes independently measured workload and
 * software. False leaves output unchanged. Not an exact hardware constant. */
bool EstimateHardware(uint64_t total, uint64_t software, uint64_t workload, HardwareEstimate&) noexcept;
/** Merge residual envelope; false for invalid bounds, no output modification. */
bool MergeHardware(HardwareEstimate&, const HardwareEstimate&) noexcept;
/** Same CPU/epoch only; reports errors instead of clamping or changing clocks. */
Report Measure(Stamp, Stamp, const HardwareEstimate*) noexcept;
/** Convert to reference ticks using an externally validated calibration epoch.
 * Uncertainty remains in OffsetEstimate; false leaves output unchanged. */
bool Normalize(uint64_t remote, const OffsetEstimate&, uint64_t& reference) noexcept;
constexpr unsigned Capacity=64;
static_assert(ATOMIC_INT_LOCK_FREE == 2, "requires lock-free unsigned atomics");
/** Resident SPSC channel: one nonreentrant root producer and one host consumer.
 * No concurrent reset; copy completes before slot release; never overwrite.
 * Consumer cannot be nested on the producer's root stack. Allocate off-stack.
 */
class ReportChannel {
    Report slots_[Capacity]{};
    alignas(64) std::atomic<unsigned> head_{0};
    alignas(64) std::atomic<unsigned> tail_{0};
    std::atomic<unsigned> dropped_{0};
public:
    bool Publish(const Report&) noexcept;
    bool Consume(Report&) noexcept;
    unsigned Dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
};
} }
