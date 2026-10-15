#include "Core/EptHandler.h"

namespace vbs { namespace ept {
namespace {
constexpr uint64_t HardwareAd = (uint64_t(1) << 8) | (uint64_t(1) << 9);

/** Record a conservative window boundary; overflow invalidates completeness. */
bool Record(Vcpu& c, uint32_t flags, uint64_t gpa, uint64_t rip,
            uint64_t qualification) noexcept {
    if (c.event_count == MaxEvents || c.next_sequence == UINT64_MAX) {
        if (c.lost_events != UINT64_MAX) ++c.lost_events;
        return false;
    }
    auto& out = c.events[c.event_count++];
    out.sequence = c.next_sequence++;
    out.raw_tsc = c.ops.read_tsc(c.ops.context);
    out.guest_physical_address = gpa;
    out.guest_rip = rip;
    out.exit_qualification = qualification;
    out.policy_generation = c.generation;
    out.flags = flags;
    out.processor_group = c.group;
    out.processor_number = c.number;
    return true;
}

/** Bounded atomic permission update; preserves PFN/memory type and hardware A/D.
 * Mapping mutation is a contract violation. Never repairs unexpected mappings.
 */
bool SetPermissions(Vcpu& c, Page& p, uint64_t permissions) noexcept {
    uint64_t old = c.ops.compare_exchange(c.ops.context, p.leaf, 0, 0);
    for (unsigned attempt = 0; attempt != 4; ++attempt) {
        if ((old & ~(Permissions | HardwareAd)) !=
            (p.armed_entry & ~(Permissions | HardwareAd))) return false;
        const uint64_t desired = (old & ~Permissions) | permissions;
        const uint64_t observed = c.ops.compare_exchange(c.ops.context, p.leaf, old, desired);
        if (observed == old) return true;
        old = observed;
    }
    return false;
}

/** Restore all entries even if one fails; retain pending state on any failure.
 * Clear only the MTF bit owned by this component, preserving other controls.
 */
bool Restore(Vcpu& c) noexcept {
    bool ok = true;
    for (size_t i = 0; i < c.pending_count; ++i)
        if (!SetPermissions(c, *c.pending[i].page, ReadOnly)) ok = false;
    if (c.pending_count && !InvalidateLocal(c)) ok = false;
    if (c.owns_mtf) {
        uint64_t controls = 0;
        if (!c.ops.read_vmcs(c.ops.context, PrimaryControls, &controls) ||
            !c.ops.write_vmcs(c.ops.context, PrimaryControls, controls & ~uint64_t(Mtf)))
            ok = false;
        else c.owns_mtf = false;
    }
    if (ok) c.pending_count = 0;
    return ok;
}

/** Best-effort rearm followed by a sticky failure. Caller MUST NOT VMRESUME. */
Result Stop(Vcpu& c) noexcept {
    (void)Restore(c);
    c.state = State::Failed;
    (void)Record(c, Failure, 0, 0, 0);
    return Result::StopSession;
}

/** Close a live window; end timestamp follows permission restoration/INVEPT. */
Result Close(Vcpu& c, uint32_t extra_flags) noexcept {
    if (c.state == State::Uninitialized || !c.ops.is_owner(c.ops.context))
        return Result::StopSession;
    if (c.state == State::Failed) return Result::StopSession;
    if (c.state == State::Armed) return Result::NotHandled;
    if (!Restore(c)) return Stop(c);
    c.state = State::Armed;
    if (!Record(c, WindowEnd | extra_flags, 0, 0, 0)) return Stop(c);
    return Result::Rearmed;
}
}

bool InvalidateLocal(Vcpu& c) noexcept {
    return c.ops.is_owner && c.ops.invept && c.ops.is_owner(c.ops.context) &&
           c.ops.invept(c.ops.context, 1, c.eptp);
}

bool Initialize(Vcpu& c, const Operations& ops, const Capabilities& caps,
                uint64_t eptp, uint64_t generation, uint16_t group,
                uint16_t number, const Page* pages, size_t count) noexcept {
    if (c.state != State::Uninitialized || !pages || !count || count > MaxPages ||
        !ops.is_owner || !ops.read_vmcs || !ops.write_vmcs ||
        !ops.compare_exchange || !ops.invept || !ops.read_tsc ||
        !caps.ept_verified || !caps.invept_single_context ||
        !(caps.primary_may_be_one & Mtf) || (caps.primary_must_be_one & Mtf) ||
        !ops.is_owner(ops.context)) return false;
    uint64_t controls = 0, current_eptp = 0;
    if (!ops.read_vmcs(ops.context, PrimaryControls, &controls) || (controls & Mtf) ||
        !ops.read_vmcs(ops.context, EptPointer, &current_eptp) || current_eptp != eptp)
        return false;
    for (size_t i = 0; i < count; ++i) {
        if ((pages[i].gpa & ~PageMask) || !pages[i].leaf ||
            (reinterpret_cast<uintptr_t>(pages[i].leaf) & 7) ||
            (pages[i].armed_entry & Permissions) != ReadOnly) return false;
        const uint64_t entry = ops.compare_exchange(ops.context, pages[i].leaf, 0, 0);
        if ((entry & ~HardwareAd) != (pages[i].armed_entry & ~HardwareAd)) return false;
        for (size_t j = 0; j < i; ++j)
            if (pages[i].gpa == pages[j].gpa || pages[i].leaf == pages[j].leaf) return false;
    }
    c.ops = ops; c.eptp = eptp; c.generation = generation;
    c.group = group; c.number = number;
    for (size_t i = 0; i < count; ++i) c.pages[i] = pages[i];
    c.page_count = count;
    if (!InvalidateLocal(c)) { c.state = State::Failed; return false; }
    c.state = State::Armed;
    return true;
}

Result HandleEptViolation(Vcpu& c) noexcept {
    if (c.state == State::Uninitialized || !c.ops.is_owner(c.ops.context))
        return Result::StopSession;
    if (c.state == State::Failed) return Result::StopSession;
    uint64_t gpa = 0, rip = 0, qualification = 0, current_eptp = 0;
    if (!c.ops.read_vmcs(c.ops.context, GuestPhysicalAddress, &gpa) ||
        !c.ops.read_vmcs(c.ops.context, GuestRip, &rip) ||
        !c.ops.read_vmcs(c.ops.context, ExitQualification, &qualification) ||
        !c.ops.read_vmcs(c.ops.context, EptPointer, &current_eptp) || current_eptp != c.eptp)
        return Stop(c);
    Page* page = nullptr;
    for (size_t i = 0; i < c.page_count; ++i)
        if (c.pages[i].gpa == (gpa & PageMask)) { page = &c.pages[i]; break; }
    if (!page) {
        const auto closed = Close(c, Interrupted);
        return closed == Result::StopSession ? closed : Result::NotHandled;
    }
    const uint64_t access = qualification & Permissions;
    // A monitored leaf only denies write/fetch, never an ordinary read.
    // No instruction decoder is used; GPA/qualification remain raw evidence.
    if (!(access & 6)) return Stop(c);
    uint64_t controls = 0;
    if (!c.ops.read_vmcs(c.ops.context, PrimaryControls, &controls) ||
        bool(controls & Mtf) != c.owns_mtf) return Stop(c);
    size_t slot = 0;
    while (slot < c.pending_count && c.pending[slot].page != page) ++slot;
    if (slot == c.pending_count) {
        if (c.pending_count == MaxPending) return Stop(c);
        c.pending[c.pending_count++].page = page;
    }
    const uint64_t current = c.ops.compare_exchange(c.ops.context, page->leaf, 0, 0);
    // If the denied access was already granted, retry would loop indefinitely.
    if ((current & access) == access) return Stop(c);
    if (!Record(c, Violation, gpa, rip, qualification)) return Stop(c);
    if (c.state != State::Window) {
        // Conservative window start precedes the first permissive EPTE write.
        if (!Record(c, WindowBegin, gpa, rip, qualification)) return Stop(c);
        c.owns_mtf = true; // cleanup must run even after a failed VM write.
        if (!c.ops.write_vmcs(c.ops.context, PrimaryControls, controls | Mtf)) return Stop(c);
        c.state = State::Window;
    }
    const uint64_t granted = (current & Permissions) | access | ReadOnly;
    if (!SetPermissions(c, *page, granted) || !InvalidateLocal(c)) return Stop(c);
    return Result::RetryInstruction;
}

Result HandleMonitorTrapFlag(Vcpu& c) noexcept {
    if (c.state == State::Uninitialized || !c.ops.is_owner(c.ops.context))
        return Result::StopSession;
    if (c.state == State::Failed) return Result::StopSession;
    if (!c.owns_mtf || c.state != State::Window) return Result::NotHandled;
    return Close(c, 0);
}

Result CloseWindowBeforeOtherExit(Vcpu& c) noexcept { return Close(c, Interrupted); }

bool CanResume(Vcpu& c, bool injecting_event) noexcept {
    if (c.state == State::Uninitialized || c.state == State::Failed ||
        !c.ops.is_owner(c.ops.context)) return false;
    if (injecting_event && c.state == State::Window) {
        if (CloseWindowBeforeOtherExit(c) == Result::StopSession) return false;
    }
    uint64_t eptp = 0, controls = 0;
    if (!c.ops.read_vmcs(c.ops.context, EptPointer, &eptp) || eptp != c.eptp ||
        !c.ops.read_vmcs(c.ops.context, PrimaryControls, &controls) ||
        bool(controls & Mtf) != c.owns_mtf) {
        (void)Stop(c); return false;
    }
    return true;
}
} }
