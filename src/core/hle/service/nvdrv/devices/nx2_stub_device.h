// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <utility>

#include "common/logging.h"
#include "core/hle/service/nvdrv/devices/nvdevice.h"

namespace Service::Nvidia::Devices {

// Placeholder for the newer nvgpu nodes Switch 2 drivers open (nvhost-tsg-gpu, nvhost-prof-gpu...).
// Every ioctl is logged and succeeds, echoing its input so in/out structures keep their fields.
class nx2_stub_device final : public nvdevice {
public:
    explicit nx2_stub_device(Core::System& system_, std::string name_)
        : nvdevice{system_}, name{std::move(name_)} {}

    NvResult Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<u8> output) override {
        return Stub(fd, command, input, output, {});
    }
    NvResult Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<const u8> inline_input, std::span<u8> output) override {
        return Stub(fd, command, input, output, {});
    }
    NvResult Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                    std::span<u8> inline_output) override {
        return Stub(fd, command, input, output, inline_output);
    }

    void OnOpen(NvCore::SessionId session_id, DeviceFD fd) override {
        LOG_WARNING(Service_NVDRV, "(STUBBED) {} opened as fd {}", name, fd);
    }
    void OnClose(DeviceFD fd) override {}

private:
    NvResult Stub(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                  std::span<u8> inline_output) {
        LOG_WARNING(Service_NVDRV,
                    "(STUBBED) {} fd={} ioctl={:08X} group={:01X} command={:02X} in={:#x}", name,
                    fd, command.raw, command.group, command.cmd, input.size());
        std::fill(output.begin(), output.end(), u8{0});
        std::copy_n(input.begin(), std::min(input.size(), output.size()), output.begin());
        std::fill(inline_output.begin(), inline_output.end(), u8{0});
        return NvResult::Success;
    }

    std::string name;
};

} // namespace Service::Nvidia::Devices

namespace Service::Nvidia::Devices {

// GPU mappings a Switch 2 process made through nx2:gpu: CreateGpuAccessibleMemory (sub 101) maps
// the memory at GPU address == CPU address, and NormalMapGpuMemory4K (211) maps a memory handle at
// a chosen GPU address. Every nvhost-as-gpu address space applies each mapping, including ones
// made before the address space existed.
class Nx2GpuMemoryRegistry {
public:
    using MapListener = std::function<void(u64 gpu_address, u64 cpu_address, u64 size)>;
    using UnmapListener = std::function<void(u64 gpu_address, u64 size)>;

    static Nx2GpuMemoryRegistry& Get() {
        static Nx2GpuMemoryRegistry registry;
        return registry;
    }

    void AddListener(const void* owner, MapListener map, UnmapListener unmap) {
        std::scoped_lock lk{mutex};
        for (const auto& m : mappings) {
            map(m.gpu_address, m.cpu_address, m.size);
        }
        listeners[owner] = {std::move(map), std::move(unmap)};
    }

    void RemoveListener(const void* owner) {
        std::scoped_lock lk{mutex};
        listeners.erase(owner);
    }

    void Register(u64 address, u64 size) {
        MapAt(address, address, size);
    }

    void MapAt(u64 gpu_address, u64 cpu_address, u64 size) {
        std::scoped_lock lk{mutex};
        Trim(gpu_address, size);
        mappings.push_back({gpu_address, cpu_address, size});
        for (auto& [owner, l] : listeners) {
            l.map(gpu_address, cpu_address, size);
        }
    }

    void Unmap(u64 gpu_address, u64 size) {
        std::scoped_lock lk{mutex};
        Trim(gpu_address, size);
        for (auto& [owner, l] : listeners) {
            l.unmap(gpu_address, size);
        }
    }

    void Reset() {
        std::scoped_lock lk{mutex};
        mappings.clear();
    }

private:
    struct Mapping {
        u64 gpu_address;
        u64 cpu_address;
        u64 size;
    };
    struct Listeners {
        MapListener map;
        UnmapListener unmap;
    };

    // Forget what overlaps [gpu_address, +size), keeping the parts outside it.
    void Trim(u64 gpu_address, u64 size) {
        const u64 end = gpu_address + size;
        std::vector<Mapping> kept;
        for (const auto& m : mappings) {
            const u64 m_end = m.gpu_address + m.size;
            if (m_end <= gpu_address || m.gpu_address >= end) {
                kept.push_back(m);
                continue;
            }
            if (m.gpu_address < gpu_address) {
                kept.push_back({m.gpu_address, m.cpu_address, gpu_address - m.gpu_address});
            }
            if (m_end > end) {
                kept.push_back({end, m.cpu_address + (end - m.gpu_address), m_end - end});
            }
        }
        mappings = std::move(kept);
    }

    std::mutex mutex;
    std::vector<Mapping> mappings;
    std::map<const void*, Listeners> listeners;
};

} // namespace Service::Nvidia::Devices
