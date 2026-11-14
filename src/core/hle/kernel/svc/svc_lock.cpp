// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/kernel/k_memory_layout.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include <mutex>
#include <set>
#include "core/hle/kernel/svc.h"

namespace Kernel::Svc {

/// Attempts to locks a mutex
Result ArbitrateLock(Core::System& system, Handle thread_handle, u64 address, u32 tag) {
    LOG_TRACE(Kernel_SVC, "called thread_handle={:#08x}, address={:#x}, tag={:#08x}",
              thread_handle, address, tag);

    // Validate the input address.
    R_UNLESS(!IsKernelAddress(address), ResultInvalidCurrentMemory);
    R_UNLESS(Common::IsAligned(address, sizeof(u32)), ResultInvalidAddress);

    auto& kernel = system.Kernel();
    auto& process = GetCurrentProcess(kernel);
    if (process.IsSwitch2()) {
        // Log each contended lock once: who owns it and what that thread is doing.
        static std::mutex m;
        static std::set<u64> seen;
        std::scoped_lock lk{m};
        if (seen.size() < 64 && seen.insert(address).second) {
            KScopedAutoObject owner =
                process.GetHandleTable().GetObjectWithoutPseudoHandle<KThread>(kernel, thread_handle);
            if (owner.IsNotNull()) {
                LOG_INFO(Kernel_SVC,
                         "Switch 2 lock {:#x} contended by t{}: owner t{} state={} svc={:#x} word={:#x}",
                         address, GetCurrentThread(kernel).GetThreadId(), owner->GetThreadId(),
                         static_cast<u32>(owner->GetState()),
                         owner->GetStackParameters().current_svc_id,
                         process.GetMemory().Read32(address));
            } else {
                LOG_INFO(Kernel_SVC, "Switch 2 lock {:#x} contended, owner handle {:#x} unresolved",
                         address, thread_handle);
            }
        }
    }

    R_RETURN(KConditionVariable::WaitForAddress(kernel, thread_handle, address, tag));
}

/// Unlock a mutex
Result ArbitrateUnlock(Core::System& system, u64 address) {
    LOG_TRACE(Kernel_SVC, "called address={:#x}", address);

    // Validate the input address.
    R_UNLESS(!IsKernelAddress(address), ResultInvalidCurrentMemory);
    R_UNLESS(Common::IsAligned(address, sizeof(u32)), ResultInvalidAddress);

    R_RETURN(KConditionVariable::SignalToAddress(system.Kernel(), address));
}

Result ArbitrateLock64(Core::System& system, Handle thread_handle, uint64_t address, uint32_t tag) {
    R_RETURN(ArbitrateLock(system, thread_handle, address, tag));
}

Result ArbitrateUnlock64(Core::System& system, uint64_t address) {
    R_RETURN(ArbitrateUnlock(system, address));
}

Result ArbitrateLock64From32(Core::System& system, Handle thread_handle, uint32_t address,
                             uint32_t tag) {
    R_RETURN(ArbitrateLock(system, thread_handle, address, tag));
}

Result ArbitrateUnlock64From32(Core::System& system, uint32_t address) {
    R_RETURN(ArbitrateUnlock(system, address));
}

} // namespace Kernel::Svc
