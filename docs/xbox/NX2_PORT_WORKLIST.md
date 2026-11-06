# Xbox (UWP) port of Lichii nx2: merge worklist

Branch `uwp-xbox`, based on Lichii `nx2` at 30d6061.

**Done:** commit 5de792b copies in the 969 files that exist in Jadyx (JulianDr14/jadyx, b88ec06) and not in Lichii:
`src/jadyx/`, `src/video_core/renderer_d3d12/`, the D3D12 helper shaders, Pulse core files, memory ledger and
guard, `tools/xbox/`, `dist/uwp/`, `externals/spirv-to-dxil`, `externals/libarchive`. Nothing is wired into the
build yet, and nothing has been compiled (no MSVC or UWP toolchain in the authoring environment).

**Not done:** the files below exist in both trees and Jadyx changed them for UWP. Each has to be merged by hand,
keeping Lichii's NX2 changes. No merge-base was available (shallow clones), so there is no automatic 3-way merge.
Lists come from a marker search (UWP/D3D12/ledger/bug-tracker/Pulse names), so expect misses.

## 1. Build wiring (done, not yet configured or compiled)

Merged by hand: root `CMakeLists.txt` (ENABLE_VULKAN/ENABLE_D3D12, sirit and OpenSSL built from source on
WindowsStore, AVX2), `src/CMakeLists.txt` (`src/jadyx`, host LTO), `src/common`, `src/core`, `src/video_core`
(Vulkan sources behind `ENABLE_VULKAN`, D3D12 block, DLSS Vulkan-only), `host_shaders`, both dynarmic lists
(`DYNARMIC_UWP_APPCONTAINER`). `CMakePresets.json` and `CMakeModules/JadyxHostLto.cmake` came with the overlay.
Checked only for balanced blocks and that every listed file exists. Expect configure errors on the first run
with the Store toolchain: the C++ sources that these lists now compile still contain the Lichii-side API that the
D3D12 backend and `src/jadyx` have not been adapted to (sections 2 to 6).

## 2. Host memory and the 5120 MiB wall (merged, not compiled)

Done:
- `src/common/host_memory.{h,cpp}`: Jadyx's file (FromApp placeholder memory, demand commit, decommit on clear,
  guest swap, fastmem hot region) with nx2's granule API on top (`MarkUsed`, `MarkDirty`, `ForgetContents`,
  `HintCold`, `VirtualSize`) and nx2's skip-clean `ClearBackingRegion`. nx2's POSIX-only macOS work is not carried over.
- `src/core/device_memory.{h,cpp}`: Jadyx's (fastmem region, Pulse checkpoint access).
- `src/core/hle/kernel/k_memory_manager.{h,cpp}`: nx2's with Jadyx's `TrimFree` (gives freed guest pages back to the
  host) and run-coalesced `ProcessOptimizedAllocation`.
- `src/common/settings.cpp`, `fs/fs_paths.h`, `fs/path_util.cpp`: Jadyx's UWP guards only (LocalFolder user dir,
  no shell32, fastmem follows `cpuopt_fastmem`). nx2's settings and `STREAMLINE_DIR` kept.
- `src/common/fiber.{h,cpp}`: Jadyx's growable fiber stacks (reserve all, commit 64 KiB). nx2's unused canaries and
  `rewind_context` are gone.
- `src/common/sparse_large_vector.cpp`: nx2's with Jadyx's `*FromApp` wrappers. nx2's region-end and `erase_if` fixes kept.
- `src/common/thread.{h,cpp}`: Jadyx's (precise-timer `Event::WaitFor`). **Dropped:** nx2's x64 MWAITX/WAITPKG
  busy-wait `Event::WaitFor`. Reinstate it behind `#if !defined(_WIN32)` if desktop builds from this branch matter.

Deliberately left as nx2 (not Jadyx): `src/core/device_memory_manager.{h,inc}` and `src/core/memory.cpp`. They keep
the 35-bit physical space the 12 GB layout needs and nx2's translation-cache and continuity fixes. Jadyx's JIT read
table (`Common::PageTable::jit_entries`, `MarkGpuWritten`, `ReadBlockUnsafeToWriteCombined`) is **not** ported; it is a
no-fastmem speed-up. It needs `common/page_table.{h,cpp}` and the dynarmic read path, so do it with section 3.

Open questions that only a run on a console can answer:
- Jadyx reserves the guest DRAM and commits pages on first touch, so reserving the 12 GiB layout costs nothing;
  only the pages a title touches count against the 5120 MiB app limit. Whether a Switch 2 title stays under that is
  unmeasured. (Earlier notes called this impossible; that overstated it.)
- `GetFastmemRegion()` assumes the application pool ends at `dram_size - KTraceBufferSize`. nx2 appends
  `Nx2GraphicsServerMemorySize` to the 12 GB layout, so the hot region may sit in the wrong place. It is a speed hint only.
- The remaining shared files in this section (`core/core_timing`, `cpu_manager`) are not done.

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
