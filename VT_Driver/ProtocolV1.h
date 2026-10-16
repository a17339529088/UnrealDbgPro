#pragma once
#include <stdint.h>

namespace vbs { namespace wire {
struct AuditRecord {
    uint64_t sequence;
    uint64_t raw_tsc;
    uint64_t guest_physical_address;
    uint64_t guest_rip;
    uint64_t exit_qualification;
    uint64_t policy_generation;
    uint32_t flags;
    uint16_t processor_group;
    uint16_t processor_number;
};
static_assert(sizeof(AuditRecord) == 56, "wire ABI");
} }
