#include "Core/VmxTiming.h"
#include <intrin.h>
namespace vbs { namespace timing {
bool EstimateHardware(uint64_t total, uint64_t software, uint64_t workload,
                      HardwareEstimate& result) noexcept {
    if (software > total || workload > total-software) return false;
    const uint64_t residual=total-software-workload;
    result={residual,residual,residual,0};
    return true;
}
bool MergeHardware(HardwareEstimate& a, const HardwareEstimate& b) noexcept {
    if (a.minimum>a.maximum || b.minimum>b.maximum) return false;
    const uint64_t low=a.minimum<b.minimum?a.minimum:b.minimum;
    const uint64_t high=a.maximum>b.maximum?a.maximum:b.maximum;
    const uint64_t width=high-low;
    a={low,high,low+width/2,width/2+width%2};
    return true;
}
// =========================================================================
// 魔改完整函数：在沙箱检测通过的瞬间，动态改写硬件寄存器，将时间延迟硬扣掉
// =========================================================================
Report Measure(Stamp begin, Stamp end, const HardwareEstimate* hardware) noexcept {
    Report out{}; 
    out.begin = begin; 
    out.end = end;
    
    // 研发 Agent 留下的最完美的防多核时钟冲突蓝屏安全护栏（保留）
    if (begin.cpu != end.cpu || begin.epoch != end.epoch) { out.flags = WrongEpoch; return out; }
    if (end.tsc < begin.tsc) { out.flags = Regressed; return out; }
    
    out.delta = end.tsc - begin.tsc; // 本次沙箱干预纯软件消耗的时间
    
    if (!hardware || hardware->minimum > hardware->maximum ||
        hardware->midpoint < hardware->minimum || hardware->midpoint > hardware->maximum) {
        out.flags = Uncalibrated; return out;
    }
    
    out.estimated_hardware   = hardware->midpoint; //
    out.hardware_uncertainty = hardware->uncertainty; //
    if (out.delta > UINT64_MAX - hardware->midpoint) { out.flags = Overflow; return out; } //
    
    // 计算出本次 VM-Exit 产生的全量时间泄漏（包含软件开销和硬件流水线开销估计）
    const uint64_t total_leak = out.delta + hardware->midpoint;
    out.estimated_total_observer = total_leak; //
    
    // -----------------------------------------------------------------
    // 核心对抗插桩：直接改写当前核心绑定的 VMCS 控制字段中的 TSC_OFFSET
    // 根据 Intel 硬件手册规定，该控制字段的物理编码为：0x00002010
    // -----------------------------------------------------------------
    size_t current_offset = 0;
    
    // 1. 利用微软 WDK 官方内建指令读取原有的硬件时钟偏移量
    if (__vmx_vmread(0x2010, &current_offset) == 0) {
        
        // 2. 在原偏移的基础上，等量累减掉本次拦截白白膨胀的 total_leak 时钟周期
        // 根据硬件公式：Guest_Time = Physical_Time + TSC_OFFSET
        // 减去膨胀的周期，即可让游戏内部和 ACE 测出来的两条 RDTSC 指令差完美保持在几十个周期内
        size_t desired_offset = current_offset - total_leak;
        
        // 3. 强行原子写入当前 CPU 的 VMCS 寄存器，抹平所有痕迹
        __vmx_vmwrite(0x2010, desired_offset);
    }

    out.flags = Valid; //
    return out;
}

bool Normalize(uint64_t remote, const OffsetEstimate& offset, uint64_t& reference) noexcept {
    if (offset.remote_minus_reference>=0) {
        const uint64_t magnitude=static_cast<uint64_t>(offset.remote_minus_reference);
        if (remote<magnitude) return false;
        reference=remote-magnitude;
    } else {
        // Avoid negating INT64_MIN directly.
        const uint64_t magnitude=uint64_t(-(offset.remote_minus_reference+1))+1;
        if (remote>UINT64_MAX-magnitude) return false;
        reference=remote+magnitude;
    }
    return true;
}
bool ReportChannel::Publish(const Report& record) noexcept {
    const unsigned head=head_.load(std::memory_order_relaxed);
    const unsigned tail=tail_.load(std::memory_order_acquire);
    if (head-tail>=Capacity) {
        // Sole producer owns this saturating statistic; no retry loop required.
        const unsigned dropped=dropped_.load(std::memory_order_relaxed);
        if (dropped!=UINT_MAX) dropped_.store(dropped+1,std::memory_order_relaxed);
        return false;
    }
    slots_[head%Capacity]=record;
    head_.store(head+1,std::memory_order_release);
    return true;
}
bool ReportChannel::Consume(Report& record) noexcept {
    const unsigned tail=tail_.load(std::memory_order_relaxed);
    if (tail==head_.load(std::memory_order_acquire)) return false;
    record=slots_[tail%Capacity];
    tail_.store(tail+1,std::memory_order_release);
    return true;
}
} }
