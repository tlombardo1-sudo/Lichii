// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <unordered_set>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "common/pulse_diagnostics.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/trap_guards.h"
#include "video_core/nx2.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/object_pool.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/texture_cache/util.h"

namespace VideoCommon {

void ShaderCache::InvalidateRegion(VAddr addr, size_t size) {
    std::scoped_lock lock{invalidation_mutex};
    InvalidatePagesInRegion(addr, size);
    RemovePendingShaders();
}

void ShaderCache::OnCacheInvalidation(VAddr addr, size_t size) {
    std::scoped_lock lock{invalidation_mutex};
    InvalidatePagesInRegion(addr, size);
}

void ShaderCache::SyncGuestHost() {
    std::scoped_lock lock{invalidation_mutex};
    RemovePendingShaders();
}

ShaderCache::ShaderCache(Tegra::MaxwellDeviceMemoryManager& device_memory_)
    : device_memory{device_memory_} {}

bool ShaderCache::RefreshStages(std::array<u64, 6>& unique_hashes) try {
    auto& dirty{maxwell3d->dirty.flags};
    // The pixel stage is left out while rasterization is off. Turning it back on writes no shader
    // register, so without this check the pixel stage stays out until the shaders change.
    const bool rasterize_enable = maxwell3d->regs.rasterize_enable != 0;
    if (!dirty[VideoCommon::Dirty::Shaders] && rasterize_enable == last_rasterize_enable) {
        return last_shaders_valid;
    }
    dirty[VideoCommon::Dirty::Shaders] = false;
    last_rasterize_enable = rasterize_enable;

    const GPUVAddr base_addr{maxwell3d->regs.program_region.Address()};
    if (Common::Pulse::Diagnostics()) {
        static std::atomic<u64> logged_region{~u64{}};
        const u64 device = gpu_memory->GpuToCpuAddress(base_addr).value_or(0);
        const u64 key = (base_addr ^ (device << 1)) ^ (u64{gpu_memory->GetID()} << 56);
        if (logged_region.exchange(key) != key) {
            LOG_INFO(Render, "Shader program region now at GPU {:#x}, device {:#x}, gmmu {}",
                     base_addr, device, gpu_memory->GetID());
        }
    }
    const bool native = maxwell3d->GetNativeMacroEngine().native_turing;
    for (size_t index = 0; index < Tegra::Engines::Maxwell3D::Regs::MaxShaderProgram; ++index) {
        if (!maxwell3d->regs.IsShaderConfigEnabled(index)) {
            unique_hashes[index] = 0;
            continue;
        }
        const auto& shader_config{maxwell3d->regs.pipelines[index]};
        const auto program{static_cast<Tegra::Engines::Maxwell3D::Regs::ShaderType>(index)};
        if (program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Pixel &&
            !maxwell3d->regs.rasterize_enable) {
            unique_hashes[index] = 0;
            continue;
        }
        // Switch 2 programs are addressed by a full GPU address held in the stage's slot.
        const GPUVAddr shader_addr{native ? (static_cast<u64>(shader_config.reserved[0] & 0xff) << 32) |
                                                shader_config.reserved[1]
                                          : base_addr + shader_config.offset};
        const std::optional<VAddr> cpu_shader_addr{gpu_memory->GpuToCpuAddress(shader_addr)};
        if (!cpu_shader_addr) {
            LOG_ERROR(HW_GPU, "Invalid GPU address for shader {:#016x}", shader_addr);
            last_shaders_valid = false;
            return false;
        }
        const ShaderInfo* shader_info{TryGet(*cpu_shader_addr)};
        if (!shader_info && native && IsRejected(*cpu_shader_addr)) {
            // A rejected program fails the same way every time it is bound: skip its draws
            // without rethrowing.
            last_shaders_valid = false;
            return false;
        }
        if (!shader_info) {
            const u32 start_address{native ? 0 : shader_config.offset};
            if (native) {
                try {
                    // Native programs are lowered while the environment is built (once: the
                    // ShaderInfo keeps the result for the pipeline's environments).
                    RethrowIfRejected(*cpu_shader_addr);
                    GraphicsEnvironment env{*maxwell3d, *gpu_memory, program, shader_addr,
                                            start_address};
                    shader_info = MakeShaderInfo(env, *cpu_shader_addr);
                } catch (const Shader::Exception&) {
                    RememberRejection(*cpu_shader_addr);
                    throw;
                }
            } else {
                GraphicsEnvironment env{*maxwell3d, *gpu_memory, program, base_addr, start_address};
                shader_info = MakeShaderInfo(env, *cpu_shader_addr);
            }
        }
        shader_infos[index] = shader_info;
        unique_hashes[index] = shader_info->unique_hash;
    }
    last_shaders_valid = true;
    return true;
} catch (const Shader::Exception& error) {
    if (!maxwell3d->GetNativeMacroEngine().native_turing) {
        throw;
    }
    last_shaders_valid = false;
    // Each rejected program is logged once (the draws that use it keep failing quietly).
    static std::unordered_set<std::string> logged;
    if (logged.size() < 256 && logged.insert(error.what()).second) {
        LOG_ERROR(HW_GPU, "Native graphics shader rejected: {}", error.what());
    }
    return false;
}

const ShaderInfo* ShaderCache::ComputeShader() {
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    const GPUVAddr shader_addr{program_base + qmd.program_start};
    const std::optional<VAddr> cpu_shader_addr{gpu_memory->GpuToCpuAddress(shader_addr)};
    if (!cpu_shader_addr) {
        LOG_ERROR(HW_GPU, "Invalid GPU address for shader {:#016x}", shader_addr);
        return nullptr;
    }
    const ShaderInfo* shader = TryGet(*cpu_shader_addr);
    if (shader) {
        if (shader->lowered_native &&
            !Shader::Ampere::VerifyTrapGuards(
                shader->lowered_native->trap_prefix, [&](u32 bank, u32 offset) {
                    return ReadNativeGuardConstant(*kepler_compute, *gpu_memory, bank, offset);
                })) {
            throw Shader::Ampere::TrapGuardError{};
        }
        return shader;
    }
    if (!kepler_compute->IsNativeMode()) {
        ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start};
        return MakeShaderInfo(env, *cpu_shader_addr);
    }
    try {
        RethrowIfRejected(*cpu_shader_addr);
        ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start};
        return MakeShaderInfo(env, *cpu_shader_addr);
    } catch (const Shader::Ampere::TrapGuardError&) {
        // Constants can become valid on the next dispatch; do not cache this rejection.
        throw;
    } catch (const Shader::Exception&) {
        RememberRejection(*cpu_shader_addr);
        throw;
    }
}

void ShaderCache::RethrowIfRejected(VAddr addr) {
    std::scoped_lock lock{rejected_mutex};
    if (const auto it = rejected_native.find(addr); it != rejected_native.end()) {
        std::rethrow_exception(it->second);
    }
}

bool ShaderCache::IsRejected(VAddr addr) {
    std::scoped_lock lock{rejected_mutex};
    return rejected_native.contains(addr);
}

void ShaderCache::RememberRejection(VAddr addr) {
    std::scoped_lock lock{rejected_mutex};
    rejected_native.emplace(addr, std::current_exception());
    rejected_lowest.store((std::min)(rejected_lowest.load(std::memory_order_relaxed), addr),
                          std::memory_order_relaxed);
    rejected_highest.store((std::max)(rejected_highest.load(std::memory_order_relaxed), addr),
                           std::memory_order_relaxed);
}

void ShaderCache::GetGraphicsEnvironments(GraphicsEnvironments& result,
                                          const std::array<u64, NUM_PROGRAMS>& unique_hashes) {
    size_t env_index{};
    const GPUVAddr base_addr{maxwell3d->regs.program_region.Address()};
    for (size_t index = 0; index < NUM_PROGRAMS; ++index) {
        if (unique_hashes[index] == 0) {
            continue;
        }
        const auto program{static_cast<Tegra::Engines::Maxwell3D::Regs::ShaderType>(index)};
        auto& env{result.envs[index]};
        const auto& config = maxwell3d->regs.pipelines[index];
        const bool native = maxwell3d->GetNativeMacroEngine().native_turing;
        const u32 start_address{native ? 0 : config.offset};
        const GPUVAddr address = native ? (static_cast<u64>(config.reserved[0] & 0xff) << 32) |
                                              config.reserved[1]
                                        : base_addr;
        env = GraphicsEnvironment{*maxwell3d, *gpu_memory, program, address, start_address,
                                  shader_infos[index]->lowered_native};
        env.SetCachedSize(shader_infos[index]->size_bytes);
        result.env_ptrs[env_index++] = &env;
    }
}

ShaderInfo* ShaderCache::TryGet(VAddr addr) const {
    std::scoped_lock lock{lookup_mutex};

    const auto it = lookup_cache.find(addr);
    if (it == lookup_cache.end()) {
        return nullptr;
    }
    return it->second->data;
}

void ShaderCache::Register(std::unique_ptr<ShaderInfo> data, VAddr addr, size_t size) {
    std::scoped_lock lock{invalidation_mutex, lookup_mutex};

    const VAddr addr_end = addr + size;
    Entry* const entry = NewEntry(addr, addr_end, data.get());

    const u64 page_end = (addr_end + YUZU_PAGESIZE - 1) >> YUZU_PAGEBITS;
    for (u64 page = addr >> YUZU_PAGEBITS; page < page_end; ++page) {
        invalidation_cache[page].push_back(entry);
    }

    storage.push_back(std::move(data));

    device_memory.UpdatePagesCachedCount(addr, size, 1);
}

void ShaderCache::InvalidatePagesInRegion(VAddr addr, size_t size) {
    const VAddr addr_end = addr + size;
    // Rejected programs are few and in a few places: look at them only for writes that can
    // reach them (this runs for every inline upload and pre-draw invalidation).
    if (addr_end > rejected_lowest.load(std::memory_order_relaxed) &&
        addr <= rejected_highest.load(std::memory_order_relaxed)) {
        std::scoped_lock lock{rejected_mutex};
        boost::unordered::erase_if(rejected_native, [&](const auto& rejected) {
            return rejected.first >= addr && rejected.first < addr_end;
        });
    }
    const u64 page_end = (addr_end + YUZU_PAGESIZE - 1) >> YUZU_PAGEBITS;
    for (u64 page = addr >> YUZU_PAGEBITS; page < page_end; ++page) {
        auto it = invalidation_cache.find(page);
        if (it == invalidation_cache.end()) {
            continue;
        }
        InvalidatePageEntries(it->second, addr, addr_end);
    }
}

void ShaderCache::RemovePendingShaders() {
    if (marked_for_removal.empty()) {
        return;
    }
    // Remove duplicates
    std::ranges::sort(marked_for_removal);
    marked_for_removal.erase(std::unique(marked_for_removal.begin(), marked_for_removal.end()),
                             marked_for_removal.end());
    // Linear growth anyways - maybe consider static_vector instead?
    boost::container::small_vector<ShaderInfo*, 16> removed_shaders;
    std::scoped_lock lock{lookup_mutex};
    for (Entry* const entry : marked_for_removal) {
        removed_shaders.push_back(entry->data);
        auto const it = lookup_cache.find(entry->addr_start);
        ASSERT(it != lookup_cache.end());
        lookup_cache.erase(it);
    }
    marked_for_removal.clear();

    if (!removed_shaders.empty()) {
        // Remove the given shaders from the cache
        std::erase_if(storage, [&removed_shaders](const std::unique_ptr<ShaderInfo>& shader) {
            return std::ranges::find(removed_shaders, shader.get()) != removed_shaders.end();
        });
    }
}

void ShaderCache::InvalidatePageEntries(std::vector<Entry*>& entries, VAddr addr, VAddr addr_end) {
    size_t index = 0;
    while (index < entries.size()) {
        Entry* const entry = entries[index];
        if (!entry->Overlaps(addr, addr_end)) {
            ++index;
            continue;
        }

        UnmarkMemory(entry);
        RemoveEntryFromInvalidationCache(entry);
        marked_for_removal.push_back(entry);
    }
}

void ShaderCache::RemoveEntryFromInvalidationCache(const Entry* entry) {
    const u64 page_end = (entry->addr_end + YUZU_PAGESIZE - 1) >> YUZU_PAGEBITS;
    for (u64 page = entry->addr_start >> YUZU_PAGEBITS; page < page_end; ++page) {
        const auto entries_it = invalidation_cache.find(page);
        ASSERT(entries_it != invalidation_cache.end());
        std::vector<Entry*>& entries = entries_it->second;

        const auto entry_it = std::ranges::find(entries, entry);
        ASSERT(entry_it != entries.end());
        entries.erase(entry_it);
    }
}

void ShaderCache::UnmarkMemory(Entry* entry) {
    if (!entry->is_memory_marked) {
        return;
    }
    entry->is_memory_marked = false;

    const VAddr addr = entry->addr_start;
    const size_t size = entry->addr_end - addr;
    device_memory.UpdatePagesCachedCount(addr, size, -1);
}

ShaderCache::Entry* ShaderCache::NewEntry(VAddr addr, VAddr addr_end, ShaderInfo* data) {
    auto entry = std::make_unique<Entry>(Entry{addr, addr_end, data});
    Entry* const entry_pointer = entry.get();

    lookup_cache.emplace(addr, std::move(entry));
    return entry_pointer;
}

const ShaderInfo* ShaderCache::MakeShaderInfo(GenericEnvironment& env, VAddr cpu_addr) {
    auto info = std::make_unique<ShaderInfo>();
    if (const std::optional<u64> cached_hash{env.Analyze()}) {
        info->unique_hash = *cached_hash;
        info->size_bytes = env.CachedSizeBytes();
    } else {
        // Slow path, not really hit on commercial games
        // Build a control flow graph to get the real shader size
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
        Shader::Maxwell::Flow::CFG cfg{env, flow_block, env.StartAddress()};
        info->unique_hash = env.CalculateHash();
        info->size_bytes = env.ReadSizeBytes();
    }
    info->lowered_native = env.LoweredNative();
    // A native program is registered over the bytes it was read from (header included), so a
    // write anywhere in them invalidates it.
    const size_t size_bytes{env.SourceSizeBytes() ? env.SourceSizeBytes() : info->size_bytes};
    const ShaderInfo* const result{info.get()};
    Register(std::move(info), cpu_addr, size_bytes);
    return result;
}

} // namespace VideoCommon
