// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_memory_layout.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc_results.h"

namespace Kernel::Svc {

namespace {
struct CvEvent {
    u64 thread;
    u64 key;
    u64 address;
    u32 tag_or_count;
    s64 timeout;
    bool signal;
};
constexpr size_t CvRingSize = 64;
std::array<CvEvent, CvRingSize> cv_ring{};
std::atomic<u64> cv_ring_next{0};

void RecordCv(Core::System& system, const CvEvent& event) {
    if (GetCurrentProcess(system.Kernel()).IsSwitch2()) {
        cv_ring[cv_ring_next.fetch_add(1, std::memory_order_relaxed) % CvRingSize] = event;
    }
}
} // namespace

/// The last condition variable waits and signals, oldest first (Switch 2 hang diagnosis).
void DumpCvRing() {
    const u64 next = cv_ring_next.load();
    for (u64 i = next > CvRingSize ? next - CvRingSize : 0; i < next; ++i) {
        const auto& e = cv_ring[i % CvRingSize];
        LOG_INFO(Kernel_SVC, "CVRING #{} t{} {} key={:#x} addr={:#x} {}={:#x} timeout={}", i,
                 e.thread, e.signal ? "signal" : "wait", e.key, e.address,
                 e.signal ? "count" : "tag", e.tag_or_count, e.timeout);
    }
}

/// Wait process wide key atomic
Result WaitProcessWideKeyAtomic(Core::System& system, u64 address, u64 cv_key, u32 tag,
                                s64 timeout_ns) {
    LOG_TRACE(Kernel_SVC, "called address={:X}, cv_key={:X}, tag={:#08x}, timeout_ns={}", address,
              cv_key, tag, timeout_ns);

    // Validate input.
    R_UNLESS(!IsKernelAddress(address), ResultInvalidCurrentMemory);
    R_UNLESS(Common::IsAligned(address, sizeof(s32)), ResultInvalidAddress);

    // Convert timeout from nanoseconds to ticks.
    s64 timeout{};
    if (timeout_ns > 0) {
        const s64 offset_tick(timeout_ns);
        if (offset_tick > 0) {
            timeout = system.Kernel().HardwareTimer().GetTick() + offset_tick + 2;
            if (timeout <= 0) {
                timeout = (std::numeric_limits<s64>::max)();
            }
        } else {
            timeout = (std::numeric_limits<s64>::max)();
        }
    } else {
        timeout = timeout_ns;
    }

    RecordCv(system, {GetCurrentThread(system.Kernel()).GetThreadId(), cv_key, address, tag,
                      timeout_ns, false});

    // Wait on the condition variable.
    R_RETURN(
        GetCurrentProcess(system.Kernel())
            .WaitConditionVariable(address, Common::AlignDown(cv_key, sizeof(u32)), tag, timeout));
}

/// Signal process wide key
void SignalProcessWideKey(Core::System& system, u64 cv_key, s32 count) {
    LOG_TRACE(Kernel_SVC, "called, cv_key={:#x}, count={:#08x}", cv_key, count);

    RecordCv(system, {GetCurrentThread(system.Kernel()).GetThreadId(), cv_key, 0,
                      static_cast<u32>(count), 0, true});

    // Signal the condition variable.
    return GetCurrentProcess(system.Kernel())
        .SignalConditionVariable(Common::AlignDown(cv_key, sizeof(u32)), count);
}

Result WaitProcessWideKeyAtomic64(Core::System& system, uint64_t address, uint64_t cv_key,
                                  uint32_t tag, int64_t timeout_ns) {
    R_RETURN(WaitProcessWideKeyAtomic(system, address, cv_key, tag, timeout_ns));
}

void SignalProcessWideKey64(Core::System& system, uint64_t cv_key, int32_t count) {
    SignalProcessWideKey(system, cv_key, count);
}

Result WaitProcessWideKeyAtomic64From32(Core::System& system, uint32_t address, uint32_t cv_key,
                                        uint32_t tag, int64_t timeout_ns) {
    R_RETURN(WaitProcessWideKeyAtomic(system, address, cv_key, tag, timeout_ns));
}

void SignalProcessWideKey64From32(Core::System& system, uint32_t cv_key, int32_t count) {
    SignalProcessWideKey(system, cv_key, count);
}

} // namespace Kernel::Svc
