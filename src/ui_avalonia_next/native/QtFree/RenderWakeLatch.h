// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <atomic>

namespace mcast::avalonia::qtfree {

// MPV can request frames much faster than Avalonia needs to schedule another
// render. Keep at most one pending wake and consume it at the actual render.
class RenderWakeLatch
{
public:
    bool mark() noexcept
    {
        return !m_pending.exchange(true);
    }

    bool consume() noexcept
    {
        return m_pending.exchange(false);
    }

    void clear() noexcept
    {
        m_pending.store(false);
    }

private:
    std::atomic_bool m_pending{false};
};

} // namespace mcast::avalonia::qtfree

