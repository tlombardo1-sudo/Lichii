# Xbox (UWP) port of Lichii nx2: merge worklist

Branch `uwp-xbox`, based on Lichii `nx2` at 30d6061.

**Done:** commit 5de792b copies in the 969 files that exist in Jadyx (JulianDr14/jadyx, b88ec06) and not in Lichii:
`src/jadyx/`, `src/video_core/renderer_d3d12/`, the D3D12 helper shaders, memory ledger and
guard, `tools/xbox/`, `dist/uwp/`, `externals/spirv-to-dxil`, `externals/libarchive`. Nothing is wired into the
build yet, and nothing has been compiled (no MSVC or UWP toolchain in the authoring environment).

**Not done:** the files below exist in both trees and Jadyx changed them for UWP. Each has to be merged by hand,
keeping Lichii's NX2 changes. No merge-base was available (shallow clones), so there is no automatic 3-way merge.
Lists come from a marker search (UWP/D3D12/ledger/bug-tracker names), so expect misses.

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
- `src/core/device_memory.{h,cpp}`: Jadyx's (fastmem region).
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

## 3. CPU / JIT (x64 W^X merged, not compiled)

Done (dynarmic x64 backend, paged W^X for the AppContainer, where RWX is never allowed):
- Jadyx's `block_of_code.{h,cpp}` (write window: `EnableWriting`/`DisableWriting`/`MakeWritable`/`WriteAlias`),
  `constant_pool.cpp`, `exception_handler_windows.cpp` (unwind info written through `WriteAlias`),
  `block_range_information.{h,cpp}` and `common/spin_lock_x64.cpp`.
- `emit_x64.cpp` (by hand): the compact-patch `Patch` loop calls `MakeWritable` before `SetCodePtr`, since patch sites
  sit in blocks that are already emitted; PerfMap registration limited to Linux; `ClearCache` resets
  `block_descriptors` and `patch_information`.
- `DYNARMIC_UWP_APPCONTAINER` forces `REQUIRE_WX` (section 1).

Deliberately left as nx2: `a64_emit_x64.cpp`, `emit_x64_memory.cpp.inc`, `a64_emit_x64_memory.cpp`, the A32/A64
interfaces, `exclusive_monitor.h`, `emit_x64_vector.cpp`, the A64 frontend and the arm64/riscv64 backends. Only
`Patch` rewrites emitted code through `SetCodePtr`, so the other emitters need no W^X change. This is a reading of
the code, not a test; a JIT run under the AppContainer is the real check.

Also done, to match what `src/jadyx` calls:
- `core_timing.cpp`: `TimedWaitProfile::ActivateThread()` on the host timing thread (Windows only).
- `arm_dynarmic.h`: `Core::SetJitCodeCacheSize` / `JitCodeCacheSize` (used by `arm_dynarmic_32/64.cpp` for the 512 MiB
  default, so the Xbox build can shrink the per-core code cache) and `Core::SetPageTableExclusives`.
- `physical_core.{h,cpp}`: `PhysicalCore::SetIdleSpinPolicy`.
- **Stored only, no effect yet:** `SetPageTableExclusives` (nx2's dynarmic has no `page_table_exclusive_access`, so
  inline exclusives through the page table are not emitted; that matters on Xbox where fastmem is off) and
  `SetIdleSpinPolicy` (nx2's cores still block on a condition variable; Jadyx's `WakeFlag` based `Idle` is not ported).

Checked: every quoted include in `src/jadyx` and `renderer_d3d12` resolves, except the 45 generated `*_spv.h`
shader headers. Each of those has a source in `host_shaders` and a CMake entry or variant row.

Left as nx2, deliberately: `arm_dynarmic_64.cpp` (Jadyx's differences are CPU profiling, JIT prewarm and the
JIT read table `jit_entries`, none needed to boot; nx2's CNTPCT pause and sign-extension handling stay),
`exclusive_monitor.h`, `cpu_manager.cpp`, the rest of `core_timing` (guest clock scaling; nx2 has its own guest
CNTFRQ and the timer-thread stop fix).

Still open:
- Inline page-table exclusives in dynarmic (`a64_emit_x64_memory.cpp`, `emit_x64_memory.cpp.inc`) and the JIT read table.
- Jadyx's `WakeFlag` based `PhysicalCore::Idle`.
- Jadyx's `writable_code_ranges.h` / profile changes may collide with nx2's LSE atomics, pointer authentication and
  memory tagging work.

## 4. GPU front end and shared API surface

Method: for every shared file Jadyx changed, take the identifiers that Jadyx's version defines, that exist nowhere in
this tree, and that `src/jadyx` or `renderer_d3d12` actually use. That replaces the earlier marker-based list.
Lichii's `rasterizer_interface.h` is a superset of Jadyx's (Jadyx only adds the removed Pulse hooks) and stays as is;
the D3D12 rasterizer's overrides all exist in it. The shared `TextureCache`/`BufferCache` templates call their runtime
through `requires`-gated paths for almost everything Lichii added, and class-template members are only instantiated
when used, so Lichii's templates can stay and the D3D12 runtime only needs the members that nothing gates.

Done: `Direct3D12` in `RendererBackend`, `XAudio2` in `AudioEngine` (appended, values are persisted), the renderer
factory in `video_core.cpp` (Vulkan and D3D12 behind `HAS_VULKAN`/`HAS_D3D12`), the XAudio2 sink in
`sink_details.cpp` and `audio_core/CMakeLists.txt`, SDL3 not linked on WindowsStore.

Also done (core group): `System::GetCheatEngine`/`EnsureCheatEngine`/`SetApplicationProcessMainRegion` (set by the NSO
loader), `SyncpointManager::CancelWaits` (called from shutdown), `PerfStats::TotalGameFrames`, `CoreWindow` window type,
`GpuThread` exception observer (non-split path; a failed command is reported and still counts as run),
`GPU::RequestFlushCommands`, `KScheduler::SetEmptyYieldPause`/`PauseEmptyYield` (called from the three yield SVCs),
`nvhost_ctrl::SetFailingEventStall` (flush instead of stalling), the debug-string observer, bug-tracker keys on guest
break/panic and a memory line at break, `FramebufferConfig` `vsync_index`/`new_frame`/`queue_time_ns`.
Stored only, no effect yet: `nvdisp_disp0::SetReleaseFence` (nx2's composite has no release fence), and the
`GpuThread` observer is not hooked into the split back thread.

Still missing, grouped (each is called by `src/jadyx` or `renderer_d3d12` and does not exist here):
- GPU thread and sync: `FenceManager::SetEarlySyncpoints`; `Macro` `SetIndirectMacroCpuRead`; `QueryCache` occlusion override.
- Texture cache: policy setters `SetMipRenderTargets`, `SetReclaimSupersededImages`, `SetRenderTargetEarlyOut`,
  `SetRenderTargetInheritance`; GC download (`PrepareGcDownload`, `CompleteGcDownload`, `GetTextureGcPolicy`,
  `RelocateImage`, `TransferPoolBytes`); `BlitCanReplaceDestination`; `SetBcGuestPalette`/`GetBcGuestPalette`;
  `SetOpaqueAstcToBc1`, `SetAstcArrayRecompression`, `HostRowLayout`; graphics binding stamps.
- Buffer cache: `BindGuestUniformBuffer`, `KeepVertexBufferReadable`, `SetInlineIndices`, `UniformNeedsPadding`,
  `UseWriteCombinedUniformCopy`, the immediate-release constants.
- Pipeline state: `FixedPipelineState` fields used for D3D12 geometry emulation (`generated_lines`,
  `generated_polygon_points`, `generated_fill_rectangle`, `shader_polygon_stipple`, `RefreshStatic`);
  `TransformFeedbackVaryings`/`CapturesGeneric`.
- File system: `NcaMetadata` `GetDataPatchId`/`GetRequiredApplicationVersion`/`GetRequiredSystemVersion`,
  `PatchManager::ReadCheatFiles` and cheat toggles, `RegisteredCache::InstallContentSet` (used by the library and cheats pages).
- JIT: `PageTable::SetJitReadTable` (section 3).
- Shader recompiler fields: section 5.

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

## 7. Pulse removed

Jadyx's Pulse (save and resume a whole session) is not part of this port, and the ~200 HLE serializer files are not
carried over. Removed from the tree: `common/pulse_*`, `core/pulse_*`, the kernel wait graph and handle state, the
service and npad Pulse state, `src/jadyx/pulse`, the D3D12 checkpoint readbacks and rasterizer hooks, the XAudio2
checkpoint park, the `host_memory` write-watch and decommit diagnostics, `tools/xbox/pulse_*` and its tests, the
Pulse docs page, the "keep the session" row of the in-game menu, the library's Pulse filter, badge and option pane,
the Pulse system setting, the `pulse_*` boot.cfg keys and the Pulse i18n strings. Leaving a game now just ends it.
Not compiled. `docs/xbox/xbox_internal.md` and the other dated dev logs still mention Pulse as history.

## Known blockers

- Lichii's Switch 2 layout asks for about 20 GiB of backing commit; the Xbox app limit is 5120 MiB. Switch 2 titles
  will not fit unless guest memory becomes sparse and much smaller than the layout the titles are told.
- Fastmem is unavailable in the Xbox sandbox, so all guest accesses go through the page table.

### Section 4 addendum: shared toggles added

Stored only (no behavioral effect in Lichii's texture cache yet): `SetRenderTargetEarlyOut`,
`SetMipRenderTargets`, `SetReclaimSupersededImages`, `SetRenderTargetInheritance`,
`SetOpaqueAstcToBc1`, `SetBcGuestPalette` (palette function itself is ported, DecompressBCn does
not use it). `SetAstcArrayRecompression` and `AstcRecompressionFor` are live. `AstcMask` exists
but images always report `None` (needs Jadyx's `ASTC::ScanAlpha`). `HostRowLayout` is ported: `UnswizzleImage` takes it (default packed, so existing callers are
unchanged), `CalculateHostUnswizzledSizeBytes` exists, and `UnswizzleTexture` gained `linear_pitch`.
Still open: texture_cache.h does not yet ask the runtime for a layout (Jadyx's
`HostRowLayout` plumbing in its upload path).

### Section 4 addendum: buffer-cache hooks

Ported into shared `buffer_cache.h` as `if constexpr (requires ...)` calls (no-ops for Vulkan/GL):
`SetInlineIndices`, `KeepVertexBufferReadable`, `UniformNeedsPadding`, `BindGuestUniformBuffer`,
`UseWriteCombinedUniformCopy` (with `DeviceMemoryManager::ReadBlockUnsafeToWriteCombined`, which
skips the translation-cache fast path). Immediate-release constants are still open.

### Section 4 addendum: pipeline state and transform feedback

`FixedPipelineState` gained `VertexAttribute::bgra`, the D3D12-only `generated_lines`,
`generated_polygon_points`, `generated_fill_rectangle`, `shader_polygon_stipple`,
`shader_mirror_border` (dynamic state) and `sample_mask_rest` (static). `RefreshStatic` already
lives in the D3D12 key. Transform feedback now follows Jadyx: `TransformFeedbackState::Refresh`
(disabled buffers contribute nothing), `TransformFeedbackVaryings` with `CapturesGeneric`/`Apply`,
and `RuntimeInfo::xfb_repeats`. The SPIR-V backend does not emit repeats yet (section 5), so a
repeated capture is dropped (the first one wins; Lichii's old code let the last win).

### Section 4 addendum: file system

`nca_metadata`, `patch_manager` and `registered_cache` now come from Jadyx (Lichii's copies were
unmodified Eden code): `CNMT::GetRequiredApplicationVersion/GetRequiredSystemVersion/GetDataPatchId`,
`FileSys::ReadCheatFiles`, `PatchManager::SortModDirectories`, `RegisteredCache::InstallContentSet`
and the launch-scoped `ContentProviderUnion::SetApplicationContentOverride`. This also removes
duplicate `ContentProviderUnion` definitions (Jadyx's `content_provider_union.cpp` was already
overlaid while Lichii's `registered_cache.cpp` still defined them). Lichii's NCZ support is
re-applied on top (`ContainerIsFullyBlockCompressed`, `.ncz` fallback in `GetNCAFromNSPForID`,
`.nsz`/`.xcz` routing). Added `Settings::values.mod_priority`.
