# Xbox (UWP) port of Lichii nx2: merge worklist

Branch `uwp-xbox`, based on Lichii `nx2` at 30d6061.

**Done:** commit 5de792b copies in the 969 files that exist in Jadyx (JulianDr14/jadyx, b88ec06) and not in Lichii:
`src/jadyx/`, `src/video_core/renderer_d3d12/`, the D3D12 helper shaders, Pulse core files, memory ledger and
guard, `tools/xbox/`, `dist/uwp/`, `externals/spirv-to-dxil`, `externals/libarchive`. Nothing is wired into the
build yet, and nothing has been compiled (no MSVC or UWP toolchain in the authoring environment).

**Not done:** the files below exist in both trees and Jadyx changed them for UWP. Each has to be merged by hand,
keeping Lichii's NX2 changes. No merge-base was available (shallow clones), so there is no automatic 3-way merge.
Lists come from a marker search (UWP/D3D12/ledger/bug-tracker/Pulse names), so expect misses.

## 1. Build wiring (do first)

- `CMakeLists.txt`, `src/CMakeLists.txt`, `src/common/CMakeLists.txt`, `src/core/CMakeLists.txt`
- `src/video_core/CMakeLists.txt`, `src/video_core/host_shaders/CMakeLists.txt`
- `src/dynarmic/CMakeLists.txt`, `src/dynarmic/src/dynarmic/CMakeLists.txt`
- add `CMakePresets.json` entries (`uwp-x64`) and `CMakeModules/JadyxHostLto.cmake` (already copied)

## 2. Host memory and the 5120 MiB wall (blocks Switch 2 titles)

- `src/common/host_memory.{h,cpp}` (Lichii: 12 GB layout, 20 GiB backing; Jadyx: `*FromApp` placeholder memory,
  commit on demand). This is the key conflict.
- `src/core/device_memory.{h,cpp}`, `src/core/device_memory_manager.{h,inc}`, `src/core/memory.cpp`
- `src/common/fiber.cpp`, `sparse_large_vector.cpp`, `thread.cpp`, `settings.cpp`, `fs/fs_paths.h`, `fs/path_util.cpp`

## 3. CPU / JIT (both forks edit the x64 backend)

- `src/dynarmic/src/dynarmic/backend/x64/block_of_code.cpp` (W^X in the AppContainer)
- `src/dynarmic/src/dynarmic/common/spin_lock_x64.cpp`, `interface/exclusive_monitor.h`
- `src/core/arm/dynarmic/arm_dynarmic_64.cpp`, `src/core/arm/exclusive_monitor.h`
- `src/core/core_timing.{h,cpp}`, `cpu_manager.cpp`, `src/tests/core/core_timing.cpp`
- Expect Jadyx's block-range / profile changes (`block_range_information`, `writable_code_ranges.h`) to collide with
  Lichii's LSE atomics, pointer authentication and memory tagging work.

## 4. GPU front end (Lichii split GPU thread vs Jadyx hooks)

- `src/video_core/gpu.cpp`, `video_core.cpp`, `rasterizer_interface.h`, `cdma_pusher.h`, `shader_cache.cpp`,
  `shader_environment.cpp`
- `control/channel_state.{h,cpp}`, `engines/maxwell_3d.h`, `maxwell_dma.h`, `puller.h`
- `host1x/host1x.h`, `syncpoint_manager.{h,cpp}`
- `buffer_cache/buffer_cache{,_base}.h`, `texture_cache/texture_cache{,_base}.h`, `util.h`, `textures/decoders.cpp`
- `renderer_vulkan/fixed_pipeline_state.h` (shared with the D3D12 backend)

## 5. Shader path (SPIR-V to DXIL via Mesa)

- `src/shader_recompiler/profile.h`, `runtime_info.h`, `ir_opt/passes.h`
- `frontend/maxwell/translate_program.{h,cpp}`
- `backend/spirv/emit_spirv_context_get_set.cpp`, `emit_spirv_special.cpp`, `emit_spirv_warp.cpp`, `spirv_emit_context.cpp`
- `src/video_core/host_shaders/astc_decoder.comp`
- Check every SPIR-V capability Lichii's Ampere lowering uses against `spirv_to_dxil` (shader model 6.4, no Agility SDK).

## 6. D3D12 renderer catch-up (no Jadyx counterpart exists)

Lichii's Vulkan work since Jadyx's Eden base has to be mirrored in `renderer_d3d12`: split GPU thread back-end,
texture array / sampler fixes, native QMD and indirect reset, `ULDC.U16` and atomic compute lowering, pipeline cache v50.
Not portable: DLSS (Streamline), Aftermath, Vulkan-only FSR3 paths.

## 7. Optional later: Pulse save/resume in HLE services

About 200 files under `src/core/hle`, plus `hid_core/resources` and `audio_core/renderer`, carry Pulse serializers.
The Pulse core files are already copied. Skip until the game boots, then port service by service.

## Known blockers

- Lichii's Switch 2 layout asks for about 20 GiB of backing commit; the Xbox app limit is 5120 MiB. Switch 2 titles
  will not fit unless guest memory becomes sparse and much smaller than the layout the titles are told.
- Fastmem is unavailable in the Xbox sandbox, so all guest accesses go through the page table.
