// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <vector>

#include <fmt/format.h>

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
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
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(32);
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
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(id != 0 ? ResultSuccess : Kernel::ResultOutOfResource);
        rb.Push(id);
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
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<u32>(0);
        rb.Push<u32>(NumIds - 1);
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
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 22, returning a nx2:gpu-sub session");
        ReturnRawSession(system, ctx,
                         std::make_shared<INx2StubInterface>(
                             system, INx2StubInterface::InternName("nx2:gpu-sub")));
    }

    KernelHelpers::ServiceContext m_context;
    std::array<Kernel::KEvent*, NumIds> m_events{};
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
        });
    }

    ~INx2Vi2Session() override {
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
        });
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

        auto ro = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(1));
        if (ro.IsNotNull() && ro->GetSize() >= 0x2000) {
            auto& memory = ro->GetOwner()->GetMemory();
            const u64 base = GetInteger(ro->GetSourceAddress());
            memory.Write32(base + 0x00, 1);              // version
            memory.Write32(base + 0x04, 0);              // base id (guess)
            memory.Write32(base + 0x08, max_syncpoints); // count
            memory.Write32(base + 0x0c, 0x40);           // -> RO region
            memory.Write32(base + 0x10, 0x000);          // -> RW region
            memory.Write32(base + 0x14, 0x800);          // -> RO region
            memory.Write32(base + 0x18, 0x800);          // -> RW region
            memory.Write32(base + 0x1c, 0x1000);         // -> RO region
        } else {
            LOG_ERROR(Service, "nx2:gsv TIPC 204: no read-only transfer memory");
        }

        if (m_syncpt_events[0] == nullptr) {
            m_syncpt_events[0] = m_context.CreateEvent("nx2:gsv:SyncptEvent0");
            m_syncpt_events[1] = m_context.CreateEvent("nx2:gsv:SyncptEvent1");
        }
        IPC::ResponseBuilder rb{ctx, 2, 2, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, m_syncpt_events[0]->GetReadableEvent(),
                           m_syncpt_events[1]->GetReadableEvent());
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
};

} // namespace Service
