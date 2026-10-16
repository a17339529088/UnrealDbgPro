#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ProtocolV1.h"

namespace vbs { namespace ept {
constexpr uint64_t PageMask = ~uint64_t(0xfff);
constexpr uint64_t Permissions = 7;
constexpr uint64_t ReadOnly = 1;
constexpr uint32_t Mtf = uint32_t(1) << 27;
constexpr size_t MaxPages = 64, MaxPending = 16, MaxEvents = 256;
constexpr uint64_t PrimaryControls = 0x4002, EptPointer = 0x201a;
constexpr uint64_t ExitQualification = 0x6400, GuestRip = 0x681e;
constexpr uint64_t GuestPhysicalAddress = 0x2400;

enum class Result { RetryInstruction, Rearmed, NotHandled, StopSession };
enum class State { Uninitialized, Armed, Window, Failed };
enum EventFlags : uint32_t {
    Violation = 1u << 0, WindowBegin = 1u << 1,
    WindowEnd = 1u << 2, Interrupted = 1u << 3, Failure = 1u << 4
};
/** All callbacks execute on the owning CPU in root mode, must be nonpageable,
 * nonthrowing, allocation-free and bounded. No locks, blocking, or OS dispatch.
 * CompareExchange returns the original value, with full memory ordering.
 * ReadTsc must return a serialized host timestamp. IsOwner verifies the current
 * logical processor AND the expected current VMCS, not merely an OS thread ID.
 */
struct Operations {
    void* context;
    bool (*is_owner)(void*);
    bool (*read_vmcs)(void*, uint64_t field, uint64_t* value);
    bool (*write_vmcs)(void*, uint64_t field, uint64_t value);
    uint64_t (*compare_exchange)(void*, volatile uint64_t*, uint64_t expected, uint64_t desired);
    bool (*invept)(void*, uint64_t type, uint64_t eptp);
    uint64_t (*read_tsc)(void*);
};
/** Supplied from capability MSRs on THIS processor, not CPUID model guesses.
 * ept_verified covers valid EPTP, 4K leaf ownership, memory types, permissions
 * at every level, and unsupported EPT features disabled (including MBEC).
 */
struct Capabilities {
    uint32_t primary_must_be_one;
    uint32_t primary_may_be_one;
    bool ept_verified;
    bool invept_single_context;
};
struct Page {
    uint64_t gpa;
    volatile uint64_t* leaf;
    uint64_t armed_entry;
};
struct Pending { Page* page; };
/** Preallocated in resident root-mapped memory. No concurrent consumers/writers.
 * Drain journal only while this vCPU is quiescent; never share leaf pointers
 * across vCPUs. Policy/EPTE mapping is immutable until global quiescence.
 */
struct Vcpu {
    Operations ops{};
    State state = State::Uninitialized;
    uint64_t eptp = 0, generation = 0;
    uint16_t group = 0, number = 0;
    Page pages[MaxPages]{};
    size_t page_count = 0;
    Pending pending[MaxPending]{};
    size_t pending_count = 0;
    wire::AuditRecord events[MaxEvents]{};
    size_t event_count = 0;
    uint64_t lost_events = 0;
    uint64_t next_sequence = 0;
    bool owns_mtf = false;
};

/** Initialize a fresh, quiescent context. Pages must already be armed R--, 4K,
 * unique in this context, resident and private to this vCPU. Verifies current
 * VMCS EPTP/MTF and flushes locally before admitting use. Does not allocate EPT.
 */
bool Initialize(Vcpu&, const Operations&, const Capabilities&, uint64_t eptp,
                uint64_t generation, uint16_t group, uint16_t number,
                const Page* pages, size_t count) noexcept;
/** Invalidate only the owning physical CPU's EPTP; never a cross-core broadcast.
 * False forbids VMRESUME. Caller must be in VMX root on the owning VMCS.
 */
bool InvalidateLocal(Vcpu&) noexcept;
/** Read VMCS fault context, journal it, grant local permissions, enable MTF.
 * Call only for basic exit reason 48. Never advances guest RIP. StopSession
 * MUST route to the host's quiescence/teardown path, never VMRESUME.
 */
Result HandleEptViolation(Vcpu&) noexcept;
/** Call for basic exit reason 37; restore all tracked leaves and clear owned
 * MTF. False hardware operations invalidate the session, never imply success.
 */
Result HandleMonitorTrapFlag(Vcpu&) noexcept;
/** Mandatory before dispatching ANY exit except EPT/MTF, and before injecting
 * ANY guest event. Closes temporary windows before unrelated guest work runs.
 * Also required after a VM-entry failure. No guest resume on StopSession.
 */
Result CloseWindowBeforeOtherExit(Vcpu&) noexcept;
/** Final VM-entry gate, after other exit handlers have run. Enforces owner,
 * EPTP and MTF ownership; no event injection is permitted with a live window.
 */
bool CanResume(Vcpu&, bool injecting_event) noexcept;
} }
