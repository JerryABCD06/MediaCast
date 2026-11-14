// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcast::avalonia::qtfree {

class Connection
{
public:
    Connection() = default;
    explicit Connection(std::function<void()> disconnect)
        : m_disconnect(std::move(disconnect))
    {
    }

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    Connection(Connection &&other) noexcept
        : m_disconnect(std::move(other.m_disconnect))
    {
        other.m_disconnect = {};
    }

    Connection &operator=(Connection &&other) noexcept
    {
        if (this == &other)
            return *this;
        disconnect();
        m_disconnect = std::move(other.m_disconnect);
        other.m_disconnect = {};
        return *this;
    }

    ~Connection()
    {
        disconnect();
    }

    void disconnect()
    {
        if (!m_disconnect)
            return;
        auto callback = std::move(m_disconnect);
        m_disconnect = {};
        callback();
    }

private:
    std::function<void()> m_disconnect;
};

template <typename... Args>
class Signal
{
public:
    using Slot = std::function<void(Args...)>;
    using Id = std::uint64_t;

    Connection connect(Slot slot)
    {
        if (!slot)
            return {};
        const std::scoped_lock lock(m_mutex);
        const Id id = m_nextId++;
        m_slots.emplace(id, std::move(slot));
        return Connection([this, id] { disconnect(id); });
    }

    void disconnect(Id id)
    {
        const std::scoped_lock lock(m_mutex);
        m_slots.erase(id);
    }

    void emit(Args... args) const
    {
        std::vector<Slot> snapshot;
        {
            const std::scoped_lock lock(m_mutex);
            snapshot.reserve(m_slots.size());
            for (const auto &[id, slot] : m_slots) {
                (void)id;
                snapshot.push_back(slot);
            }
        }
        for (const auto &slot : snapshot)
            slot(args...);
    }

private:
    mutable std::mutex m_mutex;
    std::unordered_map<Id, Slot> m_slots;
    Id m_nextId = 1;
};

} // namespace mcast::avalonia::qtfree

