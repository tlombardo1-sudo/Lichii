// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "common/logging.h"
#include "common/random.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nso.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

#ifndef HAS_NCE
namespace Core::NCE {
class Patcher {};
} // namespace Core::NCE
#endif

namespace Loader {

struct PatchCollection {
    explicit PatchCollection(bool is_application_) : is_application{is_application_} {
        module_patcher_indices.fill(-1);
        patchers.emplace_back();
    }

    std::vector<Core::NCE::Patcher>* GetPatchers() {
        if (is_application && Settings::IsNceEnabled()) {
            return &patchers;
        }
        return nullptr;
    }

    size_t GetTotalPatchSize() const {
        size_t total_size{};
#ifdef HAS_NCE
        for (auto& patcher : patchers) {
            total_size += patcher.GetSectionSize();
        }
#endif
        return total_size;
    }

    void SaveIndex(size_t module) {
        module_patcher_indices[module] = static_cast<s32>(patchers.size() - 1);
    }

    s32 GetIndex(size_t module) const {
        return module_patcher_indices[module];
    }

    s32 GetLastIndex() const {
        return static_cast<s32>(patchers.size()) - 1;
    }

    bool is_application;
    std::vector<Core::NCE::Patcher> patchers;
    std::array<s32, 13> module_patcher_indices{};
};

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(FileSys::VirtualFile file_, bool override_update_)
    : AppLoader(std::move(file_))
    , override_update(override_update_)
{
    const auto file_dir = file->GetContainingDirectory();

    // Title ID
    const auto npdm = file_dir->GetFile("main.npdm");
    if (npdm != nullptr) {
        const auto res = metadata.Load(npdm);
        if (res == ResultStatus::Success)
            title_id = metadata.GetTitleID();
    }

    // Icon
    FileSys::VirtualFile icon_file = nullptr;
    for (const auto& language : FileSys::LANGUAGE_NAMES) {
        icon_file = file_dir->GetFile("icon_" + std::string(language) + ".dat");
        if (icon_file != nullptr) {
            icon_data = icon_file->ReadAllBytes();
            break;
        }
    }

    if (icon_data.empty()) {
        // Any png, jpeg, or bmp file
        const auto& files = file_dir->GetFiles();
        const auto icon_iter =
            std::find_if(files.begin(), files.end(), [](const FileSys::VirtualFile& f) {
                return f->GetExtension() == "png" || f->GetExtension() == "jpg" ||
                       f->GetExtension() == "bmp" || f->GetExtension() == "jpeg";
            });
        if (icon_iter != files.end())
            icon_data = (*icon_iter)->ReadAllBytes();
    }

    // Metadata
    FileSys::VirtualFile nacp_file = file_dir->GetFile("control.nacp");
    if (nacp_file == nullptr) {
        const auto& files = file_dir->GetFiles();
        const auto nacp_iter =
            std::find_if(files.begin(), files.end(),
                         [](const FileSys::VirtualFile& f) { return f->GetExtension() == "nacp"; });
        if (nacp_iter != files.end())
            nacp_file = *nacp_iter;
    }

    if (nacp_file != nullptr) {
        FileSys::NACP nacp(nacp_file);
        name = nacp.GetApplicationName();
    }
}

namespace {

FileSys::VirtualDir FindLooseRomFSDirectory(const FileSys::VirtualDir& parent) {
    if (parent == nullptr) {
        return nullptr;
    }
    if (const auto dir = parent->GetSubdirectory("romfs")) {
        return dir;
    }
    for (const auto& dir : parent->GetSubdirectories()) {
        const auto name = dir->GetName();
        if (name.size() == 5 && std::equal(name.begin(), name.end(), "romfs", [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == b;
            })) {
            return dir;
        }
    }
    return nullptr;
}

// RomFS for a loose ExeFS: the first .romfs file next to main, else a romfs/ directory next to
// the exefs/ directory (the usual layout of an extracted title).
FileSys::VirtualFile FindLooseRomFS(const FileSys::VirtualDir& exefs_dir) {
    if (exefs_dir == nullptr) {
        return nullptr;
    }
    for (const auto& f : exefs_dir->GetFiles()) {
        if (f->GetExtension() == "romfs") {
            return f;
        }
    }

    const auto build_from = [](const FileSys::VirtualDir& romfs_dir) -> FileSys::VirtualFile {
        if (romfs_dir == nullptr) {
            return nullptr;
        }
        // Building it walks every file and keeps an object for each (title 1: 206k files, ~150
        // MiB), so loaders of the same directory share one build while any of them holds it.
        static std::mutex cache_mutex;
        static std::map<std::string, std::weak_ptr<FileSys::VfsFile>> cache;
        std::scoped_lock lk{cache_mutex};
        auto& cached = cache[romfs_dir->GetFullPath()];
        if (auto built = cached.lock()) {
            return built;
        }
        LOG_INFO(Loader, "Using RomFS directory next to the ExeFS");
        const auto started = std::chrono::steady_clock::now();
        auto built = FileSys::CreateRomFS(romfs_dir);
        LOG_INFO(Loader, "Loose RomFS image ready: elapsed_ms={}",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - started)
                     .count());
        cached = built;
        return built;
    };

    if (const auto parent = exefs_dir->GetParentDirectory()) {
        if (const auto romfs_dir = FindLooseRomFSDirectory(parent)) {
            return build_from(romfs_dir);
        }
        // Some base dumps carry only the executable and leave every file in an update, so the
        // base has no romfs at all (Pokemon Pokopia: base/romfs does not exist, the 6.9 GB of
        // data is in updates/655360/romfs). Handing a null RomFS to the title makes
        // OpenDataStorageByCurrentProcess fail and the game panics with an opaque 0x3fffff
        // instead of saying where the data is. Prefer the newest update present.
        if (const auto root = parent->GetParentDirectory()) {
            if (const auto updates = root->GetSubdirectory("updates")) {
                FileSys::VirtualDir newest{};
                u64 newest_version{0};
                for (const auto& candidate : updates->GetSubdirectories()) {
                    const std::string name = candidate->GetName();
                    u64 version{0};
                    const auto parsed =
                        std::from_chars(name.data(), name.data() + name.size(), version);
                    // from_chars stops at the first non digit and still reports success, so a
                    // directory called "655360_old" or "999999.bak" would be accepted with its
                    // leading digits. Require the whole name to be the number.
                    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) {
                        continue;
                    }
                    if (version >= newest_version) {
                        newest_version = version;
                        newest = candidate;
                    }
                }
                if (const auto romfs_dir = FindLooseRomFSDirectory(newest)) {
                    // Only safe when the executable is the same build as the update: running a 1.0
                    // program against 2.0.1 data boots and then writes to unmapped memory in a
                    // loop (Pokopia: 2012 invalid byte writes), which is worse than refusing.
                    const auto main_size = [](const FileSys::VirtualDir& dir) -> u64 {
                        if (dir == nullptr) {
                            return 0;
                        }
                        for (const auto& candidate : dir->GetFiles()) {
                            if (candidate->GetName() == "main") {
                                return candidate->GetSize();
                            }
                        }
                        return 0;
                    };
                    const auto base_exefs_size = main_size(exefs_dir);
                    const auto update_exefs_size = main_size(newest->GetSubdirectory("exefs"));
                    if (base_exefs_size != 0 && update_exefs_size != 0 &&
                        base_exefs_size != update_exefs_size) {
                        LOG_ERROR(Loader,
                                  "Base RomFS is absent and the update RomFS belongs to a different "
                                  "build (base main {} bytes, update main {} bytes). Load the "
                                  "update's exefs instead of the base one.",
                                  base_exefs_size, update_exefs_size);
                        return nullptr;
                    }
                    LOG_INFO(Loader, "Base RomFS is absent, using the update RomFS (version {})",
                             newest_version);
                    return build_from(romfs_dir);
                }
            }
        }
    }
    return nullptr;
}

} // namespace

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(
    FileSys::VirtualDir directory, bool override_update_)
    : AppLoader(directory->GetFile("main"))
    , dir(std::move(directory))
    , override_update(override_update_)
{}

FileType AppLoader_DeconstructedRomDirectory::IdentifyType(const FileSys::VirtualFile& dir_file) {
    if (FileSys::IsDirectoryExeFS(dir_file->GetContainingDirectory())) {
        return FileType::DeconstructedRomDirectory;
    }

    return FileType::Error;
}

AppLoader_DeconstructedRomDirectory::LoadResult AppLoader_DeconstructedRomDirectory::Load(
    Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    // Loaded directly from a loose ExeFS (not wrapped by the NCA/NSP loaders, which register the
    // process with the filesystem controller themselves).
    const bool standalone = dir == nullptr;
    if (dir == nullptr) {
        if (file == nullptr) {
            return {ResultStatus::ErrorNullFile, {}};
        }

        dir = file->GetContainingDirectory();
    }
    romfs_exefs_dir = dir;

    // Read meta to determine title ID
    FileSys::VirtualFile npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr) {
        return {ResultStatus::ErrorMissingNPDM, {}};
    }

    const ResultStatus result = metadata.Load(npdm);
    if (result != ResultStatus::Success) {
        return {result, {}};
    }

    if (override_update) {
        const FileSys::PatchManager patch_manager(
            metadata.GetTitleID(), system.GetFileSystemController(), system.GetContentProvider());
        dir = patch_manager.PatchExeFS(dir);
    }

    // Reread in case PatchExeFS affected the main.npdm
    npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr) {
        return {ResultStatus::ErrorMissingNPDM, {}};
    }

    const ResultStatus result2 = metadata.Reload(npdm);
    if (result2 != ResultStatus::Success) {
        return {result2, {}};
    }
    metadata.Print();

    // Enable NCE only for applications with 39-bit address space.
    const bool is_39bit =
        metadata.GetAddressSpaceType() == FileSys::ProgramAddressSpaceType::Is39Bit;
    const bool is_application = metadata.GetPoolPartition() == FileSys::PoolPartition::Application;
    Settings::SetNceEnabled(is_39bit);

    const std::array static_modules = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2",
                                       "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                       "subsdk8", "subsdk9", "sdk"};

    std::size_t code_size{};

    // Define an nce patch context for each potential module.
    PatchCollection patch_ctx{is_application};

    // Use the NSO module loader to figure out the code layout
    for (size_t i = 0; i < static_modules.size(); i++) {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file) {
            continue;
        }

        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, code_size, should_pass_arguments, false, {},
            patch_ctx.GetPatchers(), patch_ctx.GetLastIndex());
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        patch_ctx.SaveIndex(i);
        code_size = *tentative_next_load_addr;
    }

    // Enable direct memory mapping in case of NCE.
    const u64 fastmem_base = [&]() -> size_t {
        if (is_application && Settings::IsNceEnabled()) {
            auto& buffer = system.DeviceMemory().buffer;
            buffer.EnableDirectMappedAddress();
            return reinterpret_cast<u64>(buffer.VirtualBasePointer());
        }
        return 0;
    }();

    // Add patch size to the total module size
    code_size += patch_ctx.GetTotalPatchSize();

    // TODO: this is bad form of ASLR, it sucks
    std::uintptr_t aslr_offset = ((::Settings::values.rng_seed_enabled.GetValue()
        ? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 12) & 0xfff000;

    // Setup the process code layout
    if (process.LoadFromMetadata(system.Kernel(), metadata, code_size, fastmem_base, aslr_offset).IsError()) {
        return {ResultStatus::ErrorUnableToParseKernelMetadata, {}};
    }

    // Load NSO modules
    modules.clear();
    const VAddr base_address{GetInteger(process.GetEntryPoint())};
    VAddr next_load_addr{base_address};
    const FileSys::PatchManager pm{metadata.GetTitleID(), system.GetFileSystemController(),
                                   system.GetContentProvider()};
    for (size_t i = 0; i < static_modules.size(); i++) {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file) {
            continue;
        }

        const VAddr load_addr{next_load_addr};
        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, load_addr, should_pass_arguments, true, pm,
            patch_ctx.GetPatchers(), patch_ctx.GetIndex(i));
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        next_load_addr = *tentative_next_load_addr;
        modules.insert_or_assign(load_addr, module);
        LOG_DEBUG(Loader, "loaded module {} @ {:#x}", module, load_addr);
    }

    if (standalone) {
        // Without this, fsp-srv SetCurrentProcess fails with ResultTargetNotFound.
        system.GetFileSystemController().RegisterProcess(
            process.GetProcessId(), metadata.GetTitleID(),
            std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                    system.GetFileSystemController()));
    }

    is_loaded = true;
    return {ResultStatus::Success,
            LoadParameters{metadata.GetMainThreadPriority(), metadata.GetMainThreadStackSize()}};
}

// Resolved on first use: loaders made only for metadata never need the RomFS.
const FileSys::VirtualFile& AppLoader_DeconstructedRomDirectory::LooseRomFS() {
    if (!romfs_resolved) {
        romfs_resolved = true;
        if (romfs == nullptr) {
            romfs = FindLooseRomFS(romfs_exefs_dir);
        }
    }
    return romfs;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadRomFS(FileSys::VirtualFile& out_dir) {
    if (LooseRomFS() == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }

    out_dir = romfs;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadIcon(std::vector<u8>& out_buffer) {
    if (icon_data.empty()) {
        return ResultStatus::ErrorNoIcon;
    }

    out_buffer = icon_data;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadProgramId(u64& out_program_id) {
    out_program_id = title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadTitle(std::string& out_title) {
    if (name.empty()) {
        return ResultStatus::ErrorNoControl;
    }

    out_title = name;
    return ResultStatus::Success;
}

bool AppLoader_DeconstructedRomDirectory::IsRomFSUpdatable() const {
    return false;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadNSOModules(Modules& out_modules) {
    if (!is_loaded) {
        return ResultStatus::ErrorNotInitialized;
    }

    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
