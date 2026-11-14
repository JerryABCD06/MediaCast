// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace mcast::avalonia::qtfree {

// Transient input coalescing for high-frequency semantic values. The owner of
// the value remains the downstream subsystem; this mailbox only prevents older
// user intents from accumulating ahead of the newest one.
class LatestValueIntents
{
public:
    bool submit(std::string name, int value)
    {
        const std::scoped_lock lock(m_mutex);
        m_pending[std::move(name)] = value;
        if (m_draining)
            return false;
        m_draining = true;
        return true;
    }

    std::map<std::string, int, std::less<>> takeLatest()
    {
        std::map<std::string, int, std::less<>> latest;
        const std::scoped_lock lock(m_mutex);
        latest.swap(m_pending);
        return latest;
    }

    bool finishDrain()
    {
        const std::scoped_lock lock(m_mutex);
        if (!m_pending.empty())
            return true;
        m_draining = false;
        return false;
    }

    void reset()
    {
        const std::scoped_lock lock(m_mutex);
        m_pending.clear();
        m_draining = false;
    }

    bool draining() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_draining;
    }

private:
    mutable std::mutex m_mutex;
    std::map<std::string, int, std::less<>> m_pending;
    bool m_draining = false;
};

} // namespace mcast::avalonia::qtfree
