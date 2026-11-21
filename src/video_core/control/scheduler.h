// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <mutex>
#include "common/container/unordered_map.h"

#include "video_core/dma_pusher.h"

namespace Tegra {

class GPU;

namespace Control {

struct ChannelState;

class Scheduler {
public:
    void Push(GPU& gpu, s32 channel, CommandList&& entries);

    void DeclareChannel(std::shared_ptr<ChannelState> new_channel);

    /// Runs channels suspended on a semaphore acquire whose value has been met.
    void ResumePending(GPU& gpu);
    [[nodiscard]] bool HasPendingChannels() const {
        return any_pending.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool HasPendingChannelsAnyThread() const {
        return HasPendingChannels();
    }

private:
    ::Common::unordered_map<s32, std::shared_ptr<ChannelState>> channels;
    std::mutex scheduling_guard;
    std::vector<std::shared_ptr<ChannelState>> pending_channels; ///< GPU thread only
    std::atomic<bool> any_pending{false};
};

} // namespace Control

} // namespace Tegra
