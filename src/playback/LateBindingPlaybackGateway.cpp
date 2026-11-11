// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/LateBindingPlaybackGateway.h"

#include <exception>
#include <utility>
#include <vector>

namespace mcast::playback {
namespace {

thread_local const LateBindingPlaybackGateway *t_activeGateway = nullptr;
thread_local std::uint64_t t_activeListenerId = 0;

} // namespace

LateBindingPlaybackGateway::LateBindingPlaybackGateway()
{
    m_lastSnapshot.playerAvailable = false;
}

LateBindingPlaybackGateway::~LateBindingPlaybackGateway()
{
    detach("late-binding playback gateway stopped");
    std::vector<std::uint64_t> listeners;
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        for (auto &[id, listener] : m_listeners) {
            listener.removing = true;
            listeners.push_back(id);
        }
    }
    for (std::uint64_t listener : listeners)
        removeEventListener(listener);
}

bool LateBindingPlaybackGateway::attach(
    std::shared_ptr<PlaybackGateway> gateway,
    std::string &error)
{
    std::unique_lock attachmentLock(m_attachmentMutex);
    error.clear();
    if (!gateway) {
        error = "playback gateway is required";
        return false;
    }

    std::shared_ptr<PlaybackGateway> oldGateway;
    std::shared_ptr<PlaybackGateway> newGateway;
    std::uint64_t oldListener = 0;
    std::uint64_t token = 0;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping) {
            error = "late-binding playback gateway is stopping";
            return false;
        }
        oldGateway = std::move(m_current);
        oldListener = m_currentListener;
        m_currentListener = 0;
        token = ++m_attachmentToken;
        newGateway = gateway;
        m_current = std::move(gateway);
    }
    if (oldGateway && oldListener != 0)
        oldGateway->removeEventListener(oldListener);

    auto *raw = newGateway.get();
    const std::uint64_t listener = raw->addEventListener(
        [this, token](const PlaybackEvent &event) {
            forwardEvent(token, event);
        });
    if (listener == 0) {
        attachmentLock.unlock();
        detach("playback gateway listener registration failed");
        error = "playback gateway listener registration failed";
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping || token != m_attachmentToken || !m_current) {
            error = "playback gateway attachment changed during registration";
        } else {
            m_currentListener = listener;
        }
    }
    if (!error.empty()) {
        raw->removeEventListener(listener);
        return false;
    }

    PlaybackEvent reconnected;
    reconnected.kind = PlaybackEventKind::Reconnected;
    reconnected.snapshot = raw->snapshot();
    {
        std::lock_guard lock(m_mutex);
        m_lastSnapshot = reconnected.snapshot;
    }
    notify(reconnected);
    return true;
}

void LateBindingPlaybackGateway::detach(std::string reason)
{
    std::lock_guard attachmentLock(m_attachmentMutex);
    std::shared_ptr<PlaybackGateway> gateway;
    std::uint64_t listener = 0;
    PlaybackSnapshot finalSnapshot;
    {
        std::lock_guard lock(m_mutex);
        gateway = std::move(m_current);
        listener = m_currentListener;
        m_currentListener = 0;
        ++m_attachmentToken;
        const PlaybackSnapshot previous = m_lastSnapshot;
        m_lastSnapshot = {};
        m_lastSnapshot.generation = previous.generation;
        m_lastSnapshot.stateVersion = previous.stateVersion + 1;
        m_lastSnapshot.playerAvailable = false;
        m_lastSnapshot.peerConnected = previous.peerConnected;
        m_lastSnapshot.volumePercent = previous.volumePercent;
        m_lastSnapshot.muted = previous.muted;
        m_lastSnapshot.pictureControls = previous.pictureControls;
        finalSnapshot = m_lastSnapshot;
    }
    if (gateway && listener != 0)
        gateway->removeEventListener(listener);
    if (!gateway)
        return;

    PlaybackEvent lost;
    lost.kind = PlaybackEventKind::PlayerLost;
    lost.error = std::move(reason);
    lost.snapshot = finalSnapshot;
    notify(lost);
}

bool LateBindingPlaybackGateway::attached() const noexcept
{
    std::lock_guard lock(m_mutex);
    return static_cast<bool>(m_current);
}

PlaybackSnapshot LateBindingPlaybackGateway::snapshot() const
{
    std::shared_ptr<PlaybackGateway> gateway;
    {
        std::lock_guard lock(m_mutex);
        gateway = m_current;
    }
    return gateway ? gateway->snapshot() : [this] {
        std::lock_guard lock(m_mutex);
        return m_lastSnapshot;
    }();
}

PlaybackCommandSubmission LateBindingPlaybackGateway::submit(
    const PlaybackCommand &command)
{
    std::shared_ptr<PlaybackGateway> gateway;
    {
        std::lock_guard lock(m_mutex);
        gateway = m_current;
    }
    if (gateway)
        return gateway->submit(command);

    const PlaybackSnapshot current = snapshot();
    return {
        false,
        PlaybackSubmissionStatus::PlayerUnavailable,
        0,
        current.generation,
        "Ui player is not connected",
        current.stateVersion,
    };
}

std::uint64_t LateBindingPlaybackGateway::addEventListener(
    std::function<void(const PlaybackEvent &)> listener)
{
    if (!listener)
        return 0;
    std::lock_guard lock(m_mutex);
    if (m_stopping)
        return 0;
    const std::uint64_t id = m_nextListenerId++;
    m_listeners.emplace(
        id, ListenerEntry{std::move(listener), 0, false});
    return id;
}

void LateBindingPlaybackGateway::removeEventListener(
    std::uint64_t listenerId)
{
    std::unique_lock lock(m_mutex);
    const auto found = m_listeners.find(listenerId);
    if (found == m_listeners.end())
        return;
    found->second.removing = true;
    if (t_activeGateway == this && t_activeListenerId == listenerId)
        return;
    m_listenerIdle.wait(lock, [&] {
        const auto current = m_listeners.find(listenerId);
        return current == m_listeners.end()
            || current->second.activeCallbacks == 0;
    });
    m_listeners.erase(listenerId);
}

void LateBindingPlaybackGateway::forwardEvent(
    std::uint64_t attachmentToken,
    const PlaybackEvent &event)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping || attachmentToken != m_attachmentToken)
            return;
        m_lastSnapshot = event.snapshot;
    }
    notify(event);
}

void LateBindingPlaybackGateway::notify(const PlaybackEvent &event)
{
    struct Callback {
        std::uint64_t id = 0;
        std::function<void(const PlaybackEvent &)> callback;
    };
    std::vector<Callback> callbacks;
    {
        std::lock_guard lock(m_mutex);
        for (auto &[id, listener] : m_listeners) {
            if (listener.removing)
                continue;
            ++listener.activeCallbacks;
            callbacks.push_back(Callback{id, listener.callback});
        }
    }
    for (const Callback &entry : callbacks) {
        t_activeGateway = this;
        t_activeListenerId = entry.id;
        try {
            entry.callback(event);
        } catch (const std::exception &) {
        } catch (...) {
        }
        t_activeGateway = nullptr;
        t_activeListenerId = 0;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_listeners.find(entry.id);
            if (found == m_listeners.end())
                continue;
            if (found->second.activeCallbacks > 0)
                --found->second.activeCallbacks;
            if (found->second.removing
                && found->second.activeCallbacks == 0) {
                m_listeners.erase(found);
            }
        }
        m_listenerIdle.notify_all();
    }
}

} // namespace mcast::playback
