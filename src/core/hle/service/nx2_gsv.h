// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <algorithm>
#include <map>
#include <mutex>
#include <span>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include <fmt/format.h>

#include "core/core.h"
#include "core/core_timing.h"
#include "video_core/host1x/host1x.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nx2_image_layout.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/memory.h"

// Placeholders for the Switch 2 graphics system servers (nn::gsv and the sessions it hands out).
// Nothing behind them is emulated: they only let the SDK's boot sequence complete, and every
// other command is auto-stubbed and logged through the nx2: prefix.
namespace Service {

// TIPC reply: one-word success result followed directly by raw data words.
inline void TipcReplyWords(HLERequestContext& ctx, std::initializer_list<u32> words) {
    IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + words.size())};
    rb.PushRaw<u32>(ResultSuccess.raw);
    for (const u32 w : words) {
        rb.Push<u32>(w);
    }
}

// TIPC 0(u64 token): handshake newer SDKs send first; echo the token.
inline void TipcHandshake(HLERequestContext& ctx) {
    const auto* w = ctx.CommandBuffer();
    TipcReplyWords(ctx, {w[2], w[3]});
}

// nn::nvsrv: no handlers, so every command (CMIF or TIPC) gets an empty success.
class INx2NvsrvSession final : public ServiceFramework<INx2NvsrvSession> {
public:
    explicit INx2NvsrvSession(Core::System& system_) : ServiceFramework{system_, "nx2:nvsrv"} {}
};

// nn::syncpt, handed out by gsv 202. TIPC 1 returns a u32 the SDK requires to be < 0x400 and then
// passes min(value, 63) to TIPC 21.
class INx2SyncptSession final : public ServiceFramework<INx2SyncptSession> {
public:
    explicit INx2SyncptSession(Core::System& system_) : ServiceFramework{system_, "nx2:syncpt"} {
        RegisterHandlersTipc({
            {1, &INx2SyncptSession::GetCount, "GetCount (guessed)"},
        });
    }

private:
    void GetCount(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:syncpt TIPC 1, returning 32");
        TipcReplyWords(ctx, {32});
    }
};

// nn::gpu::sdk, handed out by gsv 497.
// - TIPC 11: [first, last] id range (first <= min(last, 64)).
// - TIPC 12(id): event handle for that id (the SDK attaches it as a SystemEvent).
// - TIPC 21(id): starts an operation the SDK then waits on; signalled at once here.
// - TIPC 22: another handle the SDK keeps like a session.
class INx2GpuSession final : public ServiceFramework<INx2GpuSession> {
public:
    explicit INx2GpuSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gpu"}, m_context{system_, "nx2:gpu"} {
        RegisterHandlersTipc({
            {11, &INx2GpuSession::GetIdRange, "GetIdRange (guessed)"},
            {12, &INx2GpuSession::GetIdEvent, "GetIdEvent (guessed)"},
            {21, &INx2GpuSession::Start, "Start (guessed)"},
            {22, &INx2GpuSession::OpenSubSession, "OpenSubSession (guessed)"},
            {33, &INx2GpuSession::GetCount33, "Unknown33 (u32 count)"},
            {41, &INx2GpuSession::CreateGraphicsDevice, "CreateGraphicsDevice"},
            {42, &INx2GpuSession::DestroyGraphicsDevice, "DestroyGraphicsDevice"},
            {201, &INx2GpuSession::GetUserPartition, "GetUserPartition (newer SDKs)"},
            {202, &INx2GpuSession::GetGsvPartition, "GetGsvPartition (newer SDKs)"},
            {203, &INx2GpuSession::GetVaRange, "GetVaRange (guessed)"},
            // Sub-session commands (gpu 22 hands out a second session to this same object).
            {101, &INx2GpuSession::SubRegisterMemory, "CreateGpuAccessibleMemory"},
            {102, &INx2GpuSession::SubDestroyMemory, "DestroyGpuAccessibleMemory (guessed)"},
            {211, &INx2GpuSession::SubSubmitJob, "NormalMapGpuMemory4K"},
            {212, &INx2GpuSession::SubModifyKind, "NormalModifyMemoryKind4K"},
            {213, &INx2GpuSession::SubUnmapJob, "NormalUnmapGpuMemory4K"},
            {214, &INx2GpuSession::SubCreateGpuOnlyRegion, "CreateSparseRegion"},
            {215, &INx2GpuSession::SubDestroySparseRegion, "DestroySparseRegion"},
            {216, &INx2GpuSession::SubSparseMap, "ExecuteSparseMapRequests"},
        });
    }

    ~INx2GpuSession() override {
        for (auto* event : m_events) {
            if (event != nullptr) {
                m_context.CloseEvent(event);
            }
        }
    }

private:
    static constexpr u32 NumIds = 32;

    std::array<bool, 64> m_devices{};

    void GetCount33(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 33 [2]={:#x}, returning 1", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {1});
    }

    // Used by nn::gpu::sdk::CreateGpuAddressSpace: three u64s after the result, a kind (< 5, 0 =
    // none) and the GPU VA range of the in-process VirtualMachine that serves AllocSpace
    // (nn::gpu::sdk::util::ExecuteVirtualMachineAllocSpace). NVN reserves fixed VA from 0x8000000
    // up to 512 GiB, so report the same range NvRm gives AllocAsEx: [0x8000000, 0xC000000000).
    void GetVaRange(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 203 [2]={:#x}", ctx.CommandBuffer()[2]);
        constexpr u64 kind = 1;
        constexpr u64 start = 0x8000000ULL;
        constexpr u64 size = 0xC000000000ULL - start;
        TipcReplyWords(ctx, {static_cast<u32>(kind), static_cast<u32>(kind >> 32),
                             static_cast<u32>(start), static_cast<u32>(start >> 32),
                             static_cast<u32>(size), static_cast<u32>(size >> 32)});
    }

    // Newer SDKs (title 1) query the address space's user and gsv partitions with 201/202(id):
    // two u64s each, stored as the partition's min/max. The user value works whether it is read
    // as (start, size) or (min, max). Title 4 never needed the gsv partition, so it stays empty.
    void GetUserPartition(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 201 [2]={:#x}", ctx.CommandBuffer()[2]);
        constexpr u64 first = 0x8000000ULL;
        constexpr u64 second = 0xC000000000ULL - first;
        TipcReplyWords(ctx, {static_cast<u32>(first), static_cast<u32>(first >> 32),
                             static_cast<u32>(second), static_cast<u32>(second >> 32)});
    }

    // An empty partition is min = ~0, max = 0 (min > max). Fire Emblem: Fortune's Weave's SDK
    // reads (0, 0) as a one-byte partition at address 0 and aborts in nn::gpu::sdk.
    void GetGsvPartition(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 202 [2]={:#x}, empty", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {~0U, ~0U, 0, 0});
    }

    // Fire Emblem: Fortune's Weave: 41(name u64, flags u64, debug label u64) returns a device id
    // that later NvRm calls name; 0 is not a valid id. Only flags 0 has been seen.
    void CreateGraphicsDevice(HLERequestContext& ctx) {
        const auto* b = ctx.CommandBuffer();
        const u64 name = u64{b[2]} | (u64{b[3]} << 32);
        const u64 flags = u64{b[4]} | (u64{b[5]} << 32);
        u32 id = 0;
        for (u32 candidate = 1; candidate < m_devices.size(); ++candidate) {
            if (!m_devices[candidate]) {
                m_devices[candidate] = true;
                id = candidate;
                break;
            }
        }
        LOG_INFO(Service, "nx2:gpu 41 create graphics device name={:#x} flags={:#x} -> id {}", name,
                 flags, id);
        if (id != 0) {
            TipcReplyWords(ctx, {id});
        } else {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.PushRaw<u32>(Kernel::ResultOutOfResource.raw);
        }
    }

    void DestroyGraphicsDevice(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2] & 0xff;
        const bool existed = id < m_devices.size() && std::exchange(m_devices[id], false);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(existed ? ResultSuccess : Kernel::ResultNotFound);
    }

    Kernel::KEvent* EventFor(u32 id) {
        id %= NumIds;
        if (m_events[id] == nullptr) {
            m_events[id] = m_context.CreateEvent(fmt::format("nx2:gpu:Event{}", id));
        }
        return m_events[id];
    }

    void GetIdRange(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 11, returning [0, {}]", NumIds - 1);
        TipcReplyWords(ctx, {0, NumIds - 1});
    }

    void GetIdEvent(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 12 id={}", id);
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, EventFor(id)->GetReadableEvent());
    }

    void Start(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 21 id={}, signalling completion", id);
        EventFor(id)->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void OpenSubSession(HLERequestContext& ctx) {
        // Served by this object so sub-session jobs can signal the per-id events.
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 22, returning a sub-session on nx2:gpu");
        ReturnRawSession(system, ctx, std::static_pointer_cast<INx2GpuSession>(shared_from_this()));
    }

    // Sub 101: (start u64, last u64, flags...) after CreateGpuAccessibleMemory's svc 0x80. The
    // reply's word 3 is the GpuMemoryHandle that later map jobs (211) name.
    void SubRegisterMemory(HLERequestContext& ctx) {
        const auto* b = ctx.CommandBuffer();
        const u64 start = u64{b[2]} | (u64{b[3]} << 32);
        const u64 last = u64{b[4]} | (u64{b[5]} << 32);
        LOG_INFO(Service, "nx2:gpu sub 101 start={:#x} last={:#x}", start, last);
        if (last > start) {
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Register(start, last - start + 1);
        }
        const u32 handle = m_next_memory_handle++;
        m_memory[handle] = {start, last > start ? last - start + 1 : 0};
        TipcReplyWords(ctx, {handle});
    }

    // Sub 102: word 2 is the memory handle 101 returned, word 3 the job slot.
    void SubDestroyMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        if (const auto it = m_memory.find(w[2]); it != m_memory.end()) {
            if (it->second.second != 0) {
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(it->second.first,
                                                                   it->second.second);
            }
            m_memory.erase(it);
        } else {
            LOG_WARNING(Service, "nx2:gpu 102 destroy of unknown handle {}", w[2]);
        }
        EventFor(w[3])->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 211: map job. Words 3-4 first GPU address, 5-6 last, 7 memory handle, 8-9 offset into
    // it; word 17 is the job slot whose event the SDK then waits on. Nothing runs asynchronously,
    // so complete it at once.
    void SubSubmitJob(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 a = u64{w[3]} | (u64{w[4]} << 32);
        const u64 b = u64{w[5]} | (u64{w[6]} << 32);
        const u64 offset = u64{w[8]} | (u64{w[9]} << 32);
        LOG_INFO(Service, "nx2:gpu 211 map [{:#x}, {:#x}] handle={} offset={:#x} slot={}", a, b,
                  w[7], offset, w[17]);
        if (const auto it = m_memory.find(w[7]); it != m_memory.end() && b > a) {
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().MapAt(a, it->second.first + offset,
                                                               b - a + 1);
        } else {
            LOG_WARNING(Service, "nx2:gpu 211 map of unknown handle {}", w[7]);
        }
        EventFor(w[17])->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 212: kinds do not matter to the emulated GPU; keep the mapping, report the wanted kind.
    void SubModifyKind(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        EventFor(w[10])->Signal(system.Kernel());
        TipcReplyWords(ctx, {w[7] & 0xff});
    }

    // Sub 213: words 3-4 first GPU address, 5-6 last, job slot in word 8.
    void SubUnmapJob(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 first = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        if (last > first) {
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(first, last - first + 1);
        }
        EventFor(w[8])->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 214/215/216: GPU-only and sparse regions are not backed yet; complete the job so the
    // game carries on (job slot in word 11 for 214, word 8 for 215, word 14 for 216).
    void SubCreateGpuOnlyRegion(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_WARNING(Service, "(STUBBED) nx2:gpu 214 region [{:#x}, {:#x}] not backed",
                    u64{w[3]} | (u64{w[4]} << 32), u64{w[5]} | (u64{w[6]} << 32));
        EventFor(w[11])->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SubDestroySparseRegion(HLERequestContext& ctx) {
        EventFor(ctx.CommandBuffer()[8])->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SubSparseMap(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const size_t count =
            ctx.BufferDescriptorA().empty() ? 0 : ctx.GetReadBufferSize(0) / 40;
        LOG_WARNING(Service, "(STUBBED) nx2:gpu 216 {} sparse map requests ignored", count);
        EventFor(w[14])->Signal(system.Kernel());
        TipcReplyWords(ctx, {static_cast<u32>(count)});
    }

    KernelHelpers::ServiceContext m_context;
    std::array<Kernel::KEvent*, NumIds> m_events{};
    u32 m_next_memory_handle = 1;
    std::map<u32, std::pair<u64, u64>> m_memory; // handle -> (address, size)
};

// nn::gpu time session handed out by gsv 1121; a missing handle ends in InvalidHandle (0xe401).
class INx2GpuTimeSession final : public ServiceFramework<INx2GpuTimeSession> {
public:
    explicit INx2GpuTimeSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gpu_time"} {
        RegisterHandlersTipc({
            {0, &INx2GpuTimeSession::Handshake, "Handshake"},
            {11, &INx2GpuTimeSession::NotSupported, "QueryApplicationGpuTimeSpan"},
        });
    }

private:
    void Handshake(HLERequestContext& ctx) {
        TipcHandshake(ctx);
    }
    void NotSupported(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.PushRaw<u32>(IPC::ResultNotSupported.raw);
    }
};

// nn::vi2 session handed out by gsv 305 (minimal: no display or events behind it).
class INx2Vi2Session final : public ServiceFramework<INx2Vi2Session> {
public:
    explicit INx2Vi2Session(Core::System& system_)
        : ServiceFramework{system_, "nx2:vi2"}, m_context{system_, "nx2:vi2"} {
        RegisterHandlersTipc({
            {0, &INx2Vi2Session::Handshake, "Handshake"},
            {1, &INx2Vi2Session::ReturnOne, "Unknown1"},
            {3, &INx2Vi2Session::ReturnOne, "GetDisplayCount (guessed)"},
            {5, &INx2Vi2Session::GetTimestampRecorderCount, "GetTimestampRecorderCount (guessed)"},
            {11, &INx2Vi2Session::ReturnEvent, "Unknown11"},
            {31, &INx2Vi2Session::AcquireNativeEvent, "AcquireNativeEvent"},
            {32, &INx2Vi2Session::DuplicateNativeEvent, "DuplicateNativeEvent"},
            {261, &INx2Vi2Session::BindVsync, "BindDisplayVsyncEvent"},
            {262, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayVsyncEvent"},
            {263, &INx2Vi2Session::BindChange, "BindDisplayRefreshRateEvent"},
            {264, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayRefreshRateEvent"},
            {265, &INx2Vi2Session::BindChange, "BindDisplayHdrStatusEvent"},
            {266, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayHdrStatusEvent"},
            {267, &INx2Vi2Session::BindChange, "BindDisplayHdrPaperWhiteEvent"},
            {268, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayHdrPaperWhiteEvent"},
            {269, &INx2Vi2Session::BindChange, "BindDisplayBufferMaxResolutionEvent"},
            {270, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayBufferMaxResolutionEvent"},
            {271, &INx2Vi2Session::BindChange, "BindDisplayModeCapabilityEvent"},
            {272, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayModeCapabilityEvent"},
            {103, &INx2Vi2Session::QueryDisplayRefreshRate, "QueryDisplayRefreshRate"},
            {104, &INx2Vi2Session::QueryDisplayHdrStatus, "QueryDisplayHdrStatus"},
            {105, &INx2Vi2Session::QueryDisplayHdrPaperWhite, "QueryDisplayHdrPaperWhite"},
            {212, &INx2Vi2Session::GetLayerBufferBits, "GetLayerBufferBits"},
            {228, &INx2Vi2Session::CancelClientBuffer, "CancelClientBuffer"},
        });
    }

    ~INx2Vi2Session() override {
        if (m_vsync_timer) {
            system.CoreTiming().UnscheduleEvent(m_vsync_timer);
        }
        {
            // The vsync callback may be running on the host timing thread.
            std::scoped_lock lk{m_vsync->mutex};
            m_vsync->events.clear();
        }
        for (auto* event : m_events) {
            m_context.CloseEvent(event);
        }
    }

private:
    void Handshake(HLERequestContext& ctx) {
        TipcHandshake(ctx);
    }
    void ReturnOne(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {1});
    }
    void GetTimestampRecorderCount(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {7});
    }
    void ReturnEvent(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC {}, returning an event", ctx.GetCommand());
        auto* event = m_context.CreateEvent("nx2:vi2:Event");
        m_events.push_back(event);
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    // 31(u32 index) / 32(handle, u32 index): one of eight display events (vsync, refresh rate,
    // HDR...). Title 1's main thread waits on its vsync slot forever if it is never signalled.
    void AcquireNativeEvent(HLERequestContext& ctx) {
        const u32 index = ctx.CommandBuffer()[2];
        if (index >= m_native_events.size()) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultUnknown);
            return;
        }
        LOG_INFO(Service, "nx2:vi2 31 native event {}", index);
        ReplyNativeEvent(ctx, index);
    }

    void DuplicateNativeEvent(HLERequestContext& ctx) {
        const u32* words = ctx.CommandBuffer();
        u32 data = 2;
        if (words[1] >> 31) {
            const u32 descriptor = words[2];
            data = 3 + ((descriptor >> 1) & 0xf) + ((descriptor >> 5) & 0xf) +
                   (descriptor & 1 ? 2 : 0);
        }
        const u32 index = words[data];
        if (index >= m_native_events.size()) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultUnknown);
            return;
        }
        LOG_INFO(Service, "nx2:vi2 32 native event {}", index);
        ReplyNativeEvent(ctx, index);
    }

    // 261 (u64 display name, u32 index): fire the slot on every vsync.
    void BindVsync(HLERequestContext& ctx) {
        const u32 index = ctx.CommandBuffer()[4];
        LOG_INFO(Service, "nx2:vi2 261 native event {} -> vsync", index);
        if (index < m_native_events.size() && m_native_events[index]) {
            SetVsyncSlot(index, true);
        }
        TipcReplyWords(ctx, {});
    }

    // 263..271 odd: the slot reports a change; signal it once.
    void BindChange(HLERequestContext& ctx) {
        const u32 index = ctx.CommandBuffer()[4];
        LOG_INFO(Service, "nx2:vi2 {} native event {} -> change notification", ctx.GetCommand(),
                 index);
        if (index < m_native_events.size() && m_native_events[index]) {
            SetVsyncSlot(index, false);
            m_native_events[index]->Signal(system.Kernel());
        }
        TipcReplyWords(ctx, {});
    }

    void UnbindNativeEvent(HLERequestContext& ctx) {
        const u32 index = ctx.CommandBuffer()[4];
        LOG_INFO(Service, "nx2:vi2 {} native event {} unbound", ctx.GetCommand(), index);
        if (index < m_native_events.size() && m_native_events[index]) {
            SetVsyncSlot(index, false);
        }
        TipcReplyWords(ctx, {});
    }

    void QueryDisplayRefreshRate(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {60, 0});
    }
    void QueryDisplayHdrStatus(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {1, 0, 400});
    }
    void QueryDisplayHdrPaperWhite(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {203});
    }
    void GetLayerBufferBits(HLERequestContext& ctx) {
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots), static_cast<u32>(AvailableSlots >> 32)});
    }
    void CancelClientBuffer(HLERequestContext& ctx) {
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots),
                             static_cast<u32>(AvailableSlots >> 32), 0, 0, 0, 0});
    }

    void ReplyNativeEvent(HLERequestContext& ctx, u32 index) {
        auto*& event = m_native_events[index];
        if (!event) {
            event = m_context.CreateEvent(fmt::format("nx2:vi2:Native{}", index));
            m_events.push_back(event);
            SetVsyncSlot(index, true);
            StartVsyncTimer();
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    void SetVsyncSlot(u32 index, bool on) {
        auto* event = m_native_events[index];
        std::scoped_lock lk{m_vsync->mutex};
        auto& list = m_vsync->events;
        const auto it = std::find(list.begin(), list.end(), event);
        if (on && it == list.end()) {
            list.push_back(event);
        } else if (!on && it != list.end()) {
            list.erase(it);
        }
    }

    void StartVsyncTimer() {
        if (m_vsync_timer) {
            return;
        }
        constexpr std::chrono::nanoseconds VblankInterval{1'000'000'000LL / 60};
        m_vsync_timer = Core::Timing::CreateEvent(
            "nx2:vi2 vsync",
            [vsync_state = m_vsync, &kernel = system.Kernel()](
                s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                std::scoped_lock lk{vsync_state->mutex};
                for (auto* vsync : vsync_state->events) {
                    vsync->Signal(kernel);
                }
                return std::nullopt;
            });
        system.CoreTiming().ScheduleLoopingEvent(VblankInterval, VblankInterval, m_vsync_timer);
    }

    struct VsyncState {
        std::mutex mutex;
        std::vector<Kernel::KEvent*> events;
    };
    std::array<Kernel::KEvent*, 8> m_native_events{};
    std::shared_ptr<VsyncState> m_vsync = std::make_shared<VsyncState>();
    std::shared_ptr<Core::Timing::EventType> m_vsync_timer;

    KernelHelpers::ServiceContext m_context;
    std::vector<Kernel::KEvent*> m_events;
};

// The graphics-system server nn::gsv::BootWithRemoteMemory talks to over TIPC.
class INx2GraphicsSystemSession final : public ServiceFramework<INx2GraphicsSystemSession> {
public:
    explicit INx2GraphicsSystemSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gsv"}, m_context{system_, "nx2:gsv"} {
        RegisterHandlersTipc({
            {4, &INx2GraphicsSystemSession::Stub, "Boot (Switch 2, guessed)"},
            {5, &INx2GraphicsSystemSession::Stub, "Unknown5"},
            {101, &INx2GraphicsSystemSession::Stub, "Unknown101"},
            {202, &INx2GraphicsSystemSession::OpenSyncptSession, "OpenSyncptSession (guessed)"},
            {204, &INx2GraphicsSystemSession::AttachSyncptMemory, "AttachSyncptMemory (guessed)"},
            {115, &INx2GraphicsSystemSession::QueryGrantedFeatures, "QueryGrantedFeatures"},
            {301, &INx2GraphicsSystemSession::Stub, "CheckPermission301"},
            {303, &INx2GraphicsSystemSession::Stub, "Unknown303"},
            {434, &INx2GraphicsSystemSession::AttachGpuAddressSpace, "AttachGpuAddressSpace"},
            {305, &INx2GraphicsSystemSession::OpenViSession, "OpenViSession (guessed)"},
            {1121, &INx2GraphicsSystemSession::OpenGpuTimeSession, "OpenGpuTimeSession"},
            {497, &INx2GraphicsSystemSession::OpenGpuSession, "OpenGpuSession (guessed)"},
            {1402, &INx2GraphicsSystemSession::OpenNvsrvSession, "OpenNvsrvSession (guessed)"},
            {111, &INx2GraphicsSystemSession::CreateNativeMemory, "CreateNativeMemory"},
            {112, &INx2GraphicsSystemSession::DestroyNativeMemory, "DestroyNativeMemory"},
            {114, &INx2GraphicsSystemSession::BindNativeMemory, "BindNativeMemory"},
            {314, &INx2GraphicsSystemSession::ConfigureWindowImagePool, "DestroyNativeWindowAliasImagePool"},
            {315, &INx2GraphicsSystemSession::ConfigureWindowImagePool, "CreateNativeWindowAliasImagePool"},
            {423, &INx2GraphicsSystemSession::BindNativeGpuMemory, "BindNativeGpuMemory"},
            {426, &INx2GraphicsSystemSession::QueryNativeImage, "MapNativeImage"},
            {428, &INx2GraphicsSystemSession::QueryNativeImage, "QueryNativeImage"},
            {429, &INx2GraphicsSystemSession::CreateNativeImage, "CreateNativeImage"},
            {430, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyNativeImage"},
            {435, &INx2GraphicsSystemSession::CalculateNativeImageSize, "CalculateNativeImageSize"},
            {1510, &INx2GraphicsSystemSession::CalculateRequirements, "CalculateRequirements"},
            {126, &INx2GraphicsSystemSession::GetSessionFlag, "GetSessionFlag"},
            {1111, &INx2GraphicsSystemSession::SetFeatureVersion, "SetFeatureVersion"},
            {311, &INx2GraphicsSystemSession::CreateNativeImage, "CreateFeatureImagePool311"},
            {312, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyFeatureImagePool312"},
            {321, &INx2GraphicsSystemSession::CreateNativeImage, "CreateFeatureImagePool321"},
            {615, &INx2GraphicsSystemSession::CreateNativeImage, "CreateFeatureImagePool615"},
            {612, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyFeatureImagePool612"},
            {811, &INx2GraphicsSystemSession::CreateNativeImage, "CreateFeatureImagePool811"},
            {812, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyFeatureImagePool812"},
            {1211, &INx2GraphicsSystemSession::CreateNativeImage, "CreateChatImagePool"},
            {1212, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyFeatureImagePool1212"},
            {1611, &INx2GraphicsSystemSession::CreateNativeImage, "CreateFeatureImagePool1611"},
            {1612, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyFeatureImagePool1612"},
            {391, &INx2GraphicsSystemSession::CalculateChatImagePoolSize, "CalculateFeatureImagePoolSize391"},
            {691, &INx2GraphicsSystemSession::CalculateChatImagePoolSize, "CalculateFeatureImagePoolSize691"},
            {891, &INx2GraphicsSystemSession::CalculateChatImagePoolSize, "CalculateFeatureImagePoolSize891"},
            {1291, &INx2GraphicsSystemSession::CalculateChatImagePoolSize, "CalculateFeatureImagePoolSize1291"},
            {1691, &INx2GraphicsSystemSession::CalculateChatImagePoolSize, "CalculateFeatureImagePoolSize1691"},
        });
    }

    ~INx2GraphicsSystemSession() override {
        if (m_syncpt_poll) {
            system.CoreTiming().UnscheduleEvent(m_syncpt_poll);
        }
        for (auto* event : m_syncpt_events) {
            if (event != nullptr) {
                m_context.CloseEvent(event);
            }
        }
    }

private:
    // Index of the first raw data word of a TIPC request: header (2 words), then an optional
    // special header with pid and copy/move handle words.
    static size_t TipcDataOffset(const u32* cmd_buf) {
        if ((cmd_buf[1] & 0x80000000) == 0) {
            return 2;
        }
        const u32 desc = cmd_buf[2];
        return 3 + ((desc & 1) ? 2 : 0) + ((desc >> 1) & 0xF) + ((desc >> 5) & 0xF);
    }

    void Stub(HLERequestContext& ctx) {
        const auto* cmd_buf = ctx.CommandBuffer();
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC {} [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x}",
                    ctx.GetCommand(), cmd_buf[2], cmd_buf[3], cmd_buf[4], cmd_buf[5]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void OpenSyncptSession(HLERequestContext& ctx) {
        // Used by nn::syncpt::sdk::InitializeEx; handle at word 3, result at word 4.
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 202, returning a nx2:syncpt session");
        ReturnRawSession(system, ctx, std::make_shared<INx2SyncptSession>(system));
    }

    // nn::syncpt::sdk::InitializeEx sends two transfer memories (0x1000 bytes RW, then 0x2000
    // bytes the client maps read-only) and expects the server to have written a header into the
    // second one: u32 version (must be 1) at +0, then u32s at +4 and +8 (a count; the client
    // requires ((count + 0x40) >> 6) - 1 < 0x40) and five offsets at +0xc..+0x1c (into the RO,
    // RW, RO, RW and RO regions). Reply: two handles (words 3 and 4), then the result.
    void AttachSyncptMemory(HLERequestContext& ctx) {
        // RequestParser doesn't skip TIPC handle words, so read the raw data words.
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        const u32 version_requested = cmd_buf[data];
        const u32 max_syncpoints = cmd_buf[data + 1];
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 204, version={} max_syncpoints={:#x}",
                    version_requested, max_syncpoints);

        const u32 count = std::min<u32>(max_syncpoints, (0x1000 - 0x40) / 8);
        auto rw = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(0));
        auto ro = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(1));
        if (rw.IsNotNull() && ro.IsNotNull() && ro->GetSize() >= 0x2000) {
            m_syncpt_memory = &ro->GetOwner()->GetMemory();
            m_syncpt_rw = GetInteger(rw->GetSourceAddress());
            m_syncpt_ro = GetInteger(ro->GetSourceAddress());
            m_syncpt_count = count;
            auto& memory = *m_syncpt_memory;
            memory.Write32(m_syncpt_ro + 0x00, 1);                     // version
            memory.Write32(m_syncpt_ro + 0x04, 0);                     // base id (guess)
            memory.Write32(m_syncpt_ro + 0x08, max_syncpoints);        // count
            memory.Write32(m_syncpt_ro + 0x0c, SyncptValuesOffset);    // -> RO region
            memory.Write32(m_syncpt_ro + 0x10, SyncptRwSummaryOffset); // -> RW region
            memory.Write32(m_syncpt_ro + 0x14, SyncptRoSummaryOffset); // -> RO region
            memory.Write32(m_syncpt_ro + 0x18, SyncptRwBitsOffset);    // -> RW region
            memory.Write32(m_syncpt_ro + 0x1c, SyncptRoBitsOffset);    // -> RO region
            UpdateSyncptValues();
            if (!m_syncpt_poll) {
                m_syncpt_poll = Core::Timing::CreateEvent(
                    "nx2:gsv:SyncptPoll",
                    [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                        PollSyncpts();
                        return std::nullopt;
                    });
                system.CoreTiming().ScheduleLoopingEvent(SyncptPollInterval, SyncptPollInterval,
                                                         m_syncpt_poll);
            }
        } else {
            LOG_ERROR(Service, "nx2:gsv TIPC 204: missing syncpt transfer memory");
        }

        if (m_syncpt_events[0] == nullptr) {
            m_syncpt_events[0] = m_context.CreateEvent("nx2:gsv:SyncptRequest");
            m_syncpt_events[1] = m_context.CreateEvent("nx2:gsv:SyncptNotify");
        }
        IPC::ResponseBuilder rb{ctx, 2, 2, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, *m_syncpt_events[0]); // signalled by the client
        rb.PushCopyObjects(ctx, m_syncpt_events[1]->GetReadableEvent());
    }

    // RO: u64 values at +0x40; RW armed bits at +0x100; RO acknowledge bits at +0x1100;
    // summaries at RW+0x0 and RO+0x1040. The client arms a wait by making its RW bit differ from
    // the RO bit, signals the first event and waits on the second until the bits match again.
    static constexpr u32 SyncptValuesOffset = 0x40;
    static constexpr u32 SyncptRoSummaryOffset = 0x1040;
    static constexpr u32 SyncptRoBitsOffset = 0x1100;
    static constexpr u32 SyncptRwSummaryOffset = 0x0;
    static constexpr u32 SyncptRwBitsOffset = 0x100;
    static constexpr u32 HostSyncpoints = 192;
    static constexpr std::chrono::nanoseconds SyncptPollInterval{std::chrono::microseconds{500}};

    // ---- Native memory and image objects (guest-side bookkeeping, no rendering). ----
    struct NativeMemory {
        u64 size{};
        u64 address{};
        u32 parent{};
        u64 parent_offset{};
    };
    struct NativeImage {
        u32 memory{};
        std::array<u32, 16> preference{};
        u64 size{};
        u32 count{1};
        bool window_pool{};
    };

    static u64 Pair(const u32* w, size_t i) {
        return u64{w[i]} | (u64{w[i + 1]} << 32);
    }

    // 111(u8 mode at word 2, u64 size): u32 native memory id.
    void CreateNativeMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 size = Pair(w, 3);
        std::scoped_lock lk{m_objects_mutex};
        const u32 id = m_next_native_memory++;
        m_native_memories[id] = {size, 0, 0, 0};
        LOG_INFO(Service, "nx2:gsv 111 CreateNativeMemory mode={} size={:#x} -> id {}", w[2] & 0xFF,
                 size, id);
        TipcReplyWords(ctx, {id});
    }

    void DestroyNativeMemory(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        {
            std::scoped_lock lk{m_objects_mutex};
            m_native_memories.erase(id);
        }
        LOG_DEBUG(Service, "nx2:gsv 112 DestroyNativeMemory id {}", id);
        TipcReplyWords(ctx, {});
    }

    void BindMemory(u32 id, u64 address, u64 size, const char* command, u32 parent = 0,
                    u64 parent_offset = 0) {
        std::scoped_lock lk{m_objects_mutex};
        auto& memory = m_native_memories[id];
        memory.address = address;
        memory.parent = parent;
        memory.parent_offset = parent_offset;
        if (memory.size == 0) {
            memory.size = size;
        }
        LOG_INFO(Service, "nx2:gsv {} bind native memory {} at {:#x}+{:#x}", command, id, address,
                 size);
    }

    void BindNativeMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        BindMemory(w[2], Pair(w, 3), Pair(w, 5), "114");
        TipcReplyWords(ctx, {});
    }

    void BindNativeGpuMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_DEBUG(Service, "nx2:gsv 423 parent={} offset={:#x}", w[7], Pair(w, 8));
        BindMemory(w[2], Pair(w, 3), Pair(w, 5), "423", w[7], Pair(w, 8));
        TipcReplyWords(ctx, {});
    }

    // 429(preference[16], u32 count, u32 memory id): u32 native image id.
    void CreateNativeImage(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        std::scoped_lock lk{m_objects_mutex};
        const u32 id = m_next_native_image;
        auto& image = m_native_images[id];
        image.count = std::max<u32>(w[18], 1);
        // nn::gs::sdk::CalculateNativeImageHandle(pool, i) is pool + i, so keep those free.
        m_next_native_image += image.count;
        image.memory = w[19];
        std::copy_n(w + 2, image.preference.size(), image.preference.begin());
        image.size = Nx2CalculateImageLayout(w + 2).size;
        LOG_INFO(Service, "nx2:gsv {} CreateNativeImage memory={} count={} size={:#x} -> id {}",
                 ctx.GetCommand(), w[19], w[18], image.size, id);
        TipcReplyWords(ctx, {id});
    }

    void DestroyNativeImage(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        {
            std::scoped_lock lk{m_objects_mutex};
            m_native_images.erase(id);
        }
        TipcReplyWords(ctx, {});
    }

    // 428(u32 handle) QueryNativeImage and 426 MapNativeImage: an address, an offset and a
    // 0x90-byte descriptor of the image.
    void QueryNativeImage(HLERequestContext& ctx) {
        const u32 handle = ctx.CommandBuffer()[2];
        std::array<u32, 16> preference{};
        u64 image_offset = 0;
        u64 memory_address = 0;
        {
            std::scoped_lock lk{m_objects_mutex};
            auto it = m_native_images.upper_bound(handle);
            if (it != m_native_images.begin()) {
                --it;
                if (handle - it->first < it->second.count) {
                    preference = it->second.preference;
                    image_offset = (handle - it->first) * it->second.size;
                    if (const auto memory = m_native_memories.find(it->second.memory);
                        memory != m_native_memories.end()) {
                        memory_address = memory->second.address;
                    }
                }
            }
        }
        const auto layout = Nx2CalculateImageLayout(preference.data());
        const u64 gpu_address = ctx.GetCommand() == 426 ? memory_address + image_offset : 0;
        std::array<u32, 37> words{};
        words[0] = static_cast<u32>(gpu_address);
        words[1] = static_cast<u32>(gpu_address >> 32);
        words[2] = static_cast<u32>(image_offset);
        words[3] = static_cast<u32>(image_offset >> 32);
        u32* const desc = words.data() + 4;
        desc[0] = 1;
        desc[1] = preference[1];
        desc[2] = preference[2];
        desc[4] = layout.block_height_log2;
        desc[5] = static_cast<u32>(layout.pitch / 64);
        desc[8] = 1;
        desc[9] = 1;
        desc[11] = static_cast<u32>(layout.size);
        LOG_INFO(Service, "nx2:gsv {} handle={} {}x{} size={:#x} gpu={:#x}", ctx.GetCommand(),
                 handle, layout.width, layout.height, layout.size, gpu_address);
        TipcReplyWords(ctx, std::span<const u32>{words.data(), words.size()});
    }

    void CalculateNativeImageSize(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 size = Nx2CalculateImageLayout(w + 2).size * std::max<u32>(w[18], 1);
        LOG_DEBUG(Service, "nx2:gsv 435 count={} -> size={:#x}", w[18], size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32)});
    }

    // 1510 gsv::shim::CalculateRequirements: size and alignment of a window image.
    void CalculateRequirements(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout =
            Nx2CalculateNativeImageRequirements(std::span<const u32, 16>{w + 2, 16}, w[18]);
        LOG_INFO(Service, "nx2:gsv 1510 kind={} {}x{} block={} pitch={:#x} format={} -> size={:#x} align={:#x}",
                 w[3], layout.width, layout.height, layout.block_height_log2, layout.pitch, w[8],
                 layout.size, layout.alignment);
        TipcReplyWords(ctx, {static_cast<u32>(layout.size), static_cast<u32>(layout.size >> 32),
                             static_cast<u32>(layout.alignment),
                             static_cast<u32>(layout.alignment >> 32)});
    }

    // Pool size of a feature's images (391, 691, 891, 1291, 1691 share the server template):
    // preference[16], u32 count -> u64 size, u64 alignment.
    void CalculateChatImagePoolSize(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateImageLayout(w + 2);
        const u64 size = layout.size * std::max<u32>(w[18], 1);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32),
                             static_cast<u32>(layout.alignment),
                             static_cast<u32>(layout.alignment >> 32)});
    }

    void ConfigureWindowImagePool(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        const bool create = ctx.GetCommand() == 315;
        std::scoped_lock lk{m_objects_mutex};
        if (auto it = m_native_images.find(id); it != m_native_images.end()) {
            it->second.window_pool = create;
        }
        LOG_INFO(Service, "nx2:gsv {} window image pool image {}", ctx.GetCommand(), id);
        TipcReplyWords(ctx, {});
    }

    void GetSessionFlag(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {0});
    }

    void SetFeatureVersion(HLERequestContext& ctx) {
        LOG_INFO(Service, "nx2:gsv TIPC 1111 value {:#x}", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {});
    }

    std::mutex m_objects_mutex;
    std::map<u32, NativeMemory> m_native_memories;
    std::map<u32, NativeImage> m_native_images;
    u32 m_next_native_memory{1};
    u32 m_next_native_image{1};

    bool UpdateSyncptValues() {
        auto& manager = system.Host1x().GetSyncpointManager();
        bool changed = false;
        for (u32 id = 0; id < std::min(m_syncpt_count, HostSyncpoints); ++id) {
            const u64 value = manager.GetGuestSyncpointValue(id);
            const u64 address = m_syncpt_ro + SyncptValuesOffset + id * 8;
            if (m_syncpt_memory->Read64(address) != value) {
                m_syncpt_memory->Write64(address, value);
                changed = true;
            }
        }
        return changed;
    }

    // Answers every armed wait: the value page is current, so the client rechecks its threshold
    // and rearms if it still has to wait.
    void PollSyncpts() {
        if (m_syncpt_memory == nullptr) {
            return;
        }
        const u32 words = (m_syncpt_count + 63) / 64;
        std::array<u64, 0x40> armed{};
        for (u32 w = 0; w < words; ++w) {
            armed[w] = m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwBitsOffset + w * 8);
        }
        UpdateSyncptValues();
        bool acked = false;
        for (u32 w = 0; w < words; ++w) {
            const u64 ro_address = m_syncpt_ro + SyncptRoBitsOffset + w * 8;
            if (m_syncpt_memory->Read64(ro_address) != armed[w]) {
                m_syncpt_memory->Write64(ro_address, armed[w]);
                acked = true;
            }
        }
        m_syncpt_memory->Write64(m_syncpt_ro + SyncptRoSummaryOffset,
                                 m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwSummaryOffset));
        if (acked) {
            m_syncpt_events[1]->Signal(system.Kernel());
        }
    }

    void OpenGpuSession(HLERequestContext& ctx) {
        // Used by nn::gpu::sdk::BootWithRemoteMemory; takes one u64, returns a session (handle at
        // word 3, result at word 4) that the GPU SDK keeps.
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 497 arg={:#x}, returning a nx2:gpu session",
                    u64{cmd_buf[data]} | (u64{cmd_buf[data + 1]} << 32));
        ReturnRawSession(system, ctx, std::make_shared<INx2GpuSession>(system));
    }

    // 115(u64 requested feature flags): the granted subset, which is all of them here. A zeroed
    // reply would deny every feature.
    void QueryGrantedFeatures(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_INFO(Service, "nx2:gsv TIPC 115 features {:#x}", u64{w[2]} | (u64{w[3]} << 32));
        TipcReplyWords(ctx, {w[2], w[3]});
    }

    void AttachGpuAddressSpace(HLERequestContext& ctx) {
        LOG_INFO(Service, "nx2:gsv TIPC 434 address space {:#x}", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {});
    }

    void OpenViSession(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 305, returning a nx2:vi2 session");
        ReturnRawSession(system, ctx, std::make_shared<INx2Vi2Session>(system));
    }

    void OpenGpuTimeSession(HLERequestContext& ctx) {
        LOG_INFO(Service, "nx2:gsv TIPC 1121, returning a nx2:gpu_time session");
        ReturnRawSession(system, ctx, std::make_shared<INx2GpuTimeSession>(system));
    }

    void OpenNvsrvSession(HLERequestContext& ctx) {
        // The SDK reads the handle at word 3 and the result at word 4.
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 1402, returning a nx2:nvsrv session");
        auto module = system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv", true)->GetModule();
        auto nvsrv = std::make_shared<Nvidia::NVDRV>(system, module, "nx2:nvsrv");
        nvsrv->InitializeForProcess(ctx.GetThread().GetOwnerProcess());
        ReturnRawSession(system, ctx, std::move(nvsrv));
    }

    KernelHelpers::ServiceContext m_context;
    std::array<Kernel::KEvent*, 2> m_syncpt_events{};
    Core::Memory::Memory* m_syncpt_memory{};
    u64 m_syncpt_rw{};
    u64 m_syncpt_ro{};
    u32 m_syncpt_count{};
    std::shared_ptr<Core::Timing::EventType> m_syncpt_poll;
};

} // namespace Service
