// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>

#include "common/common_types.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

[[nodiscard]] u32 ConvertedBytesPerBlock(VideoCore::Surface::PixelFormat pixel_format);

void DecompressBCn(std::span<const u8> input, std::span<u8> output, BufferImageCopy& copy,
                   VideoCore::Surface::PixelFormat pixel_format);

/// BC1-3 colors use the guest texture unit's interpolation (stored; DecompressBCn does not
/// consult it yet).
void SetBcGuestPalette(bool enabled) noexcept;
[[nodiscard]] bool GetBcGuestPalette() noexcept;

[[nodiscard]] std::array<std::array<u8, 4>, 4> GuestBc1Palette(const u8* block,
                                                               bool four_colors) noexcept;

} // namespace VideoCommon
