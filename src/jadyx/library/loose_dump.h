// SPDX-License-Identifier: GPL-3.0-or-later
//
// A dump kept as loose folders (exefs/ with main and main.npdm, romfs/ next to it) rather than a
// container file. The core loads it from exefs/main (Loader::AppLoader_DeconstructedRomDirectory),
// so the library lists that file, named after the dump's folder.
//
// Layout: <dump>/exefs, or <dump>/base/exefs, plus optionally <dump>/updates/<version>/exefs. When
// the base has no romfs of its own (the data is in the update), the core only starts the update's
// exefs, so the newest update's is the one listed.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Jadyx {

namespace detail {
inline std::wstring LowerWide(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

inline bool IsVersionFolder(const std::wstring& name) {
    return !name.empty() && name.size() <= 18 &&
           std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

/// -1 for a base exefs, the update's version for <dump>/updates/<version>/exefs/main, and -2 when
/// the path is neither (including DLC).
inline std::int64_t LooseVersion(const std::filesystem::path& relative) {
    const auto exefs = relative.parent_path();
    if (LowerWide(relative.filename().wstring()) != L"main" ||
        LowerWide(exefs.filename().wstring()) != L"exefs") {
        return -2;
    }
    const auto version = exefs.parent_path();
    const auto updates = version.parent_path();
    if (LowerWide(updates.filename().wstring()) == L"updates") {
        const auto name = version.filename().wstring();
        return IsVersionFolder(name) ? static_cast<std::int64_t>(std::stoll(name)) : -2;
    }
    for (const auto& part : exefs) {
        const auto name = LowerWide(part.wstring());
        if (name == L"updates" || name == L"dlc") return -2;
    }
    return -1;
}

/// The dump's own folder: what holds exefs/ (or base/, or updates/).
inline std::filesystem::path LooseRoot(const std::filesystem::path& relative) {
    auto folder = relative.parent_path().parent_path(); // holds exefs/
    if (LowerWide(folder.parent_path().filename().wstring()) == L"updates") {
        return folder.parent_path().parent_path();
    }
    if (LowerWide(folder.filename().wstring()) == L"base") return folder.parent_path();
    return folder;
}
} // namespace detail

/// True for the exefs/main of a base game or of one of its updates. DLC is not a game of its own.
inline bool IsLooseExeFsMain(const std::filesystem::path& relative) {
    return detail::LooseVersion(relative) >= -1;
}

/// A romfs folder holds thousands of files nothing here lists; scanning it would use up the
/// entry limit before the other games are reached.
inline bool IsLooseRomFsFolder(const std::wstring& name) {
    return detail::LowerWide(name) == L"romfs";
}

/// The title shown until the game's own is read: the dump's folder.
inline std::wstring LooseDumpName(const std::filesystem::path& relative) {
    const auto name = detail::LooseRoot(relative).filename().wstring();
    return name.empty() ? std::wstring{L"ExeFS"} : name;
}

/// A dump with updates is listed once, by its newest update's exefs (else by the base one).
template <class Entry>
void CollapseLooseDumps(std::vector<Entry>& entries) {
    struct Best { std::int64_t version; std::size_t index; };
    std::map<std::wstring, Best> best;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto version = detail::LooseVersion(entries[i].relative_path);
        if (version < -1) continue;
        const auto key = entries[i].source_name + L"|" + detail::LooseRoot(entries[i].relative_path).wstring();
        const auto [it, inserted] = best.try_emplace(key, Best{version, i});
        if (!inserted && version > it->second.version) it->second = Best{version, i};
    }
    std::vector<Entry> kept;
    kept.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (detail::LooseVersion(entries[i].relative_path) >= -1) {
            const auto key = entries[i].source_name + L"|" + detail::LooseRoot(entries[i].relative_path).wstring();
            if (best.at(key).index != i) continue;
        }
        kept.push_back(std::move(entries[i]));
    }
    entries = std::move(kept);
}

} // namespace Jadyx
