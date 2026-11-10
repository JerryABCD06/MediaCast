// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "protocols/dlna/playback/PlaybackGateway.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace mcast::dlna {

class FakePlaybackGateway final : public PlaybackGateway
{
public:
    PlaybackSnapshot snapshot() const override { return m_snapshot; }

    PlaybackCommandSubmission submit(
        const PlaybackCommand &command) override
    {
        if (!m_snapshot.playerAvailable) {
            return {false, PlaybackSubmissionStatus::PlayerUnavailable,
                    0, m_snapshot.generation, "player is unavailable",
                    m_snapshot.stateVersion};
        }
        const ValidationResult validation = validate(command);
        if (!validation.valid) {
            return {false, PlaybackSubmissionStatus::InvalidCommand,
                    0, m_snapshot.generation, validation.error,
                    m_snapshot.stateVersion};
        }

        PlaybackCommand queued = command;
        if (queued.expectedGeneration != 0
            && queued.expectedGeneration != m_snapshot.generation) {
            return {false, PlaybackSubmissionStatus::StaleGeneration,
                    0, m_snapshot.generation, "stale playback generation",
                    m_snapshot.stateVersion};
        }
        queued.expectedGeneration = m_snapshot.generation;
        m_pending.push_back(Pending{++m_nextCommandId, std::move(queued)});
        return {true, PlaybackSubmissionStatus::Queued,
                m_nextCommandId, m_snapshot.generation, {},
                m_snapshot.stateVersion};
    }

    std::uint64_t addEventListener(
        std::function<void(const PlaybackEvent &)> listener) override
    {
        const std::uint64_t id = ++m_nextListenerId;
        if (listener)
            m_listeners.emplace(id, std::move(listener));
        return id;
    }

    void removeEventListener(std::uint64_t listenerId) override
    {
        m_listeners.erase(listenerId);
    }

    bool processNext()
    {
        if (m_pending.empty())
            return false;
        Pending pending = std::move(m_pending.front());
        m_pending.erase(m_pending.begin());

        if (pending.command.expectedGeneration != m_snapshot.generation) {
            PlaybackEvent event;
            event.kind = PlaybackEventKind::CommandCompleted;
            event.commandId = pending.id;
            event.outcome = PlaybackOutcomeStatus::StaleGeneration;
            event.error = "stale playback generation";
            event.snapshot = m_snapshot;
            notify(event);
            return true;
        }
        if (!m_snapshot.playerAvailable) {
            PlaybackEvent event;
            event.kind = PlaybackEventKind::CommandCompleted;
            event.commandId = pending.id;
            event.outcome = PlaybackOutcomeStatus::Rejected;
            event.error = "player became unavailable";
            event.snapshot = m_snapshot;
            notify(event);
            return true;
        }
        const ValidationResult validation = validate(pending.command);
        if (!validation.valid) {
            PlaybackEvent event;
            event.kind = PlaybackEventKind::CommandCompleted;
            event.commandId = pending.id;
            event.outcome = PlaybackOutcomeStatus::Rejected;
            event.error = validation.error;
            event.snapshot = m_snapshot;
            notify(event);
            return true;
        }

        const std::string error = apply(pending.command);
        PlaybackEvent completed;
        completed.kind = PlaybackEventKind::CommandCompleted;
        completed.commandId = pending.id;
        completed.outcome = error.empty()
            ? PlaybackOutcomeStatus::Applied
            : PlaybackOutcomeStatus::Rejected;
        completed.error = error;
        completed.snapshot = m_snapshot;
        notify(completed);
        notifySnapshot();
        return true;
    }

    void processAll()
    {
        while (processNext()) {
        }
    }

    void markReady(bool success = true)
    {
        m_snapshot.loadStatus =
            success ? LoadStatus::Ok : LoadStatus::Failed;
        m_snapshot.state =
            success ? PlaybackState::Playing : PlaybackState::Stopped;
        m_snapshot.durationSeconds = 120.0;
        bumpVersion();
        notifySnapshot();
    }

    void setPictureControls(std::vector<PictureControl> controls)
    {
        m_snapshot.pictureControls = std::move(controls);
    }

    void simulateReconnect()
    {
        ++m_snapshot.generation;
        m_snapshot.state = PlaybackState::NoMedia;
        m_snapshot.current = {};
        m_snapshot.next = {};
        m_snapshot.hasPrevious = false;
        m_snapshot.hasNext = false;
        m_snapshot.playerAvailable = true;
        m_snapshot.loadStatus = LoadStatus::Ok;
        m_snapshot.positionSeconds = 0.0;
        m_snapshot.durationSeconds = 0.0;
        m_snapshot.stateVersion = 1;
        m_previous = {};
        PlaybackEvent event;
        event.kind = PlaybackEventKind::Reconnected;
        event.snapshot = m_snapshot;
        notify(event);
    }

    void simulatePlayerLost()
    {
        const std::uint64_t generation = m_snapshot.generation;
        const std::uint64_t stateVersion = m_snapshot.stateVersion + 1;
        const bool peerConnected = m_snapshot.peerConnected;
        const int volume = m_snapshot.volumePercent;
        const bool muted = m_snapshot.muted;
        const auto pictureControls = m_snapshot.pictureControls;
        m_snapshot = PlaybackSnapshot{};
        m_snapshot.generation = generation;
        m_snapshot.stateVersion = stateVersion;
        m_snapshot.playerAvailable = false;
        m_snapshot.peerConnected = peerConnected;
        m_snapshot.volumePercent = volume;
        m_snapshot.muted = muted;
        m_snapshot.pictureControls = pictureControls;
        m_previous = {};
        PlaybackEvent event;
        event.kind = PlaybackEventKind::PlayerLost;
        event.snapshot = m_snapshot;
        notify(event);
        notifySnapshot();
    }

private:
    struct Pending {
        std::uint64_t id = 0;
        PlaybackCommand command;
    };

    struct ValidationResult {
        bool valid = true;
        std::string error;
    };

    ValidationResult validate(const PlaybackCommand &command) const
    {
        switch (command.type) {
        case PlaybackCommandType::Open:
            return command.media.uri.empty()
                ? ValidationResult{false, "open requires a URI"}
                : ValidationResult{};
        case PlaybackCommandType::SetVolume:
            return command.intValue < 0 || command.intValue > 100
                ? ValidationResult{false, "volume is out of range"}
                : ValidationResult{};
        case PlaybackCommandType::Seek:
            return command.doubleValue < 0.0
                ? ValidationResult{false, "seek position is negative"}
                : ValidationResult{};
        case PlaybackCommandType::SetPictureControl:
            return !std::any_of(
                m_snapshot.pictureControls.begin(),
                m_snapshot.pictureControls.end(),
                [&](const PictureControl &control) {
                    return control.name == command.text;
                })
                ? ValidationResult{false, "unknown picture control"}
                : ValidationResult{};
        default:
            return {};
        }
    }

    std::string apply(const PlaybackCommand &command)
    {
        switch (command.type) {
        case PlaybackCommandType::Open:
            m_snapshot.current = command.media;
            m_snapshot.state = PlaybackState::Preparing;
            m_snapshot.loadStatus = LoadStatus::Ok;
            m_snapshot.positionSeconds = 0.0;
            m_snapshot.durationSeconds = 0.0;
            break;
        case PlaybackCommandType::Play:
            if (m_snapshot.state == PlaybackState::NoMedia)
                return "no media";
            m_snapshot.state = PlaybackState::Playing;
            break;
        case PlaybackCommandType::Pause:
            if (m_snapshot.state == PlaybackState::NoMedia)
                return "no media";
            m_snapshot.state = PlaybackState::Paused;
            break;
        case PlaybackCommandType::Stop:
            if (m_snapshot.state == PlaybackState::NoMedia)
                return "no media";
            m_snapshot.state = PlaybackState::Stopped;
            break;
        case PlaybackCommandType::Seek:
            if (m_snapshot.state == PlaybackState::NoMedia)
                return "no media";
            m_snapshot.positionSeconds = std::clamp(
                command.doubleValue, 0.0,
                std::max(0.0, m_snapshot.durationSeconds));
            break;
        case PlaybackCommandType::Next:
            if (!m_snapshot.hasNext)
                return "no next item";
            m_previous = m_snapshot.current;
            m_snapshot.current = m_snapshot.next;
            m_snapshot.next = {};
            m_snapshot.hasNext = false;
            m_snapshot.hasPrevious = true;
            m_snapshot.state = PlaybackState::Preparing;
            break;
        case PlaybackCommandType::Previous:
            if (m_previous.uri.empty())
                return "no previous item";
            m_snapshot.next = m_snapshot.current;
            m_snapshot.current = m_previous;
            m_previous = {};
            m_snapshot.hasNext = true;
            m_snapshot.hasPrevious = false;
            m_snapshot.state = PlaybackState::Preparing;
            break;
        case PlaybackCommandType::EndSession: {
            const std::uint64_t generation = m_snapshot.generation;
            const std::uint64_t stateVersion = m_snapshot.stateVersion;
            const bool peerConnected = m_snapshot.peerConnected;
            const bool playerAvailable = m_snapshot.playerAvailable;
            const int volume = m_snapshot.volumePercent;
            const bool muted = m_snapshot.muted;
            const auto pictureControls = m_snapshot.pictureControls;
            m_snapshot = PlaybackSnapshot{};
            m_snapshot.generation = generation;
            m_snapshot.stateVersion = stateVersion;
            m_snapshot.peerConnected = peerConnected;
            m_snapshot.playerAvailable = playerAvailable;
            m_snapshot.volumePercent = volume;
            m_snapshot.muted = muted;
            m_snapshot.pictureControls = pictureControls;
            m_previous = {};
            break;
        }
        case PlaybackCommandType::SetNext:
            m_snapshot.next = command.media;
            m_snapshot.hasNext = !command.media.uri.empty();
            break;
        case PlaybackCommandType::SetPlayMode:
            m_snapshot.playMode = command.playMode;
            break;
        case PlaybackCommandType::SetVolume:
            m_snapshot.volumePercent = command.intValue;
            break;
        case PlaybackCommandType::SetMuted:
            m_snapshot.muted = command.boolValue;
            break;
        case PlaybackCommandType::SetPictureControl: {
            const auto it = std::find_if(
                m_snapshot.pictureControls.begin(),
                m_snapshot.pictureControls.end(),
                [&](const PictureControl &control) {
                    return control.name == command.text;
                });
            if (it == m_snapshot.pictureControls.end())
                return "picture control disappeared";
            it->value = std::clamp(
                command.intValue, it->minimum, it->maximum);
            break;
        }
        case PlaybackCommandType::ResetPictureControls:
            for (PictureControl &control : m_snapshot.pictureControls)
                control.value = control.neutral;
            break;
        case PlaybackCommandType::SetPeerConnected:
            if (m_snapshot.peerConnected != command.boolValue) {
                m_snapshot.peerConnected = command.boolValue;
                m_snapshot.next = {};
                m_previous = {};
                m_snapshot.hasNext = false;
                m_snapshot.hasPrevious = false;
            }
            break;
        }
        bumpVersion();
        return {};
    }

    void bumpVersion()
    {
        ++m_snapshot.stateVersion;
    }

    void notifySnapshot()
    {
        PlaybackEvent event;
        event.kind = PlaybackEventKind::SnapshotChanged;
        event.snapshot = m_snapshot;
        notify(event);
    }

    void notify(const PlaybackEvent &event)
    {
        const auto listeners = m_listeners;
        for (const auto &[id, listener] : listeners) {
            (void)id;
            listener(event);
        }
    }

    PlaybackSnapshot m_snapshot;
    MediaRequest m_previous;
    std::vector<Pending> m_pending;
    std::map<std::uint64_t, std::function<void(const PlaybackEvent &)>>
        m_listeners;
    std::uint64_t m_nextCommandId = 0;
    std::uint64_t m_nextListenerId = 0;
};

} // namespace mcast::dlna
