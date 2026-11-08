// SPDX-License-Identifier: GPL-3.0-or-later
//
// A dump kept as loose folders (exefs/ with main and main.npdm, romfs/ next to it) rather than a
// container file. The core loads it from exefs/main (Loader::AppLoader_DeconstructedRomDirectory),
// so the library lists that file, named after the dump's folder.

#pragma once

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

namespace Jadyx {

namespace detail {
inline std::wstring LowerWide(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}
} // namespace detail

/// True for <dump>/exefs/main (or <dump>/base/exefs/main) of the base game. An update's or DLC's
/// own exefs (under updates/ or dlc/) is not a game of its own and is left out.
inline bool IsLooseExeFsMain(const std::filesystem::path& relative) {
    if (detail::LowerWide(relative.filename().wstring()) != L"main") return false;
    if (detail::LowerWide(relative.parent_path().filename().wstring()) != L"exefs") return false;
    for (const auto& part : relative.parent_path()) {
        const auto name = detail::LowerWide(part.wstring());
        if (name == L"updates" || name == L"dlc") return false;
    }
    return true;
}

/// A romfs folder holds thousands of files nothing here lists; scanning it would use up the
/// entry limit before the other games are reached.
inline bool IsLooseRomFsFolder(const std::wstring& name) {
    return detail::LowerWide(name) == L"romfs";
}

/// The title shown until the game's own is read: the dump's folder (above base/ when there is one).
inline std::wstring LooseDumpName(const std::filesystem::path& relative) {
    auto folder = relative.parent_path().parent_path(); // the folder holding exefs/
    if (detail::LowerWide(folder.filename().wstring()) == L"base") folder = folder.parent_path();
    const auto name = folder.filename().wstring();
    return name.empty() ? std::wstring{L"ExeFS"} : name;
}

} // namespace Jadyx
