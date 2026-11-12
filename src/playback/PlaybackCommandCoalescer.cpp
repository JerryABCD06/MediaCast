// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/PlaybackCommandCoalescer.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace mcast::playback {
namespace {

std::string pictureControlKey(const PlaybackCommand &command)
{
    return "picture:" + command.text;
}

} // namespace

PlaybackCommandCoalescer::PlaybackCommandCoalescer(
    std::size_t capacity)
    : m_capacity(capacity)
{
}

PlaybackCommandCoalescer::EnqueueResult
PlaybackCommandCoalescer::enqueue(QueuedCommand command)
{
    EnqueueResult result;
    if (m_capacity == 0)
        return result;

    if (command.command.type == PlaybackCommandType::EndSession) {
        // Disconnect is control-plane traffic. Anything still waiting here has
        // not been sent, so ending the session safely makes it obsolete.
        result.supersededCommandIds = clear();
    } else if (m_commands.size() >= m_capacity) {
        return result;
    }

    const auto key = coalescingKey(command.command);
    if (key) {
        // Walk backwards until a media/action boundary. Every matching
        // command in this segment is older than the new command and would be
        // executed before it, so only the newest value can affect the result.
        for (auto it = m_commands.end(); it != m_commands.begin();) {
            --it;
            if (isBoundary(it->command.type))
                break;
            if (!coalescingKey(it->command)
                || *coalescingKey(it->command) != *key) {
                continue;
            }
            result.supersededCommandIds.push_back(it->commandId);
            result.supersededCommandIds.insert(
                result.supersededCommandIds.end(),
                it->aliases.begin(), it->aliases.end());
            command.aliases.insert(
                command.aliases.end(),
                it->aliases.begin(), it->aliases.end());
            command.aliases.push_back(it->commandId);
            it = m_commands.erase(it);
        }
    }

    m_commands.push_back(std::move(command));
    result.queued = true;
    return result;
}

std::optional<PlaybackCommandCoalescer::QueuedCommand>
PlaybackCommandCoalescer::takeNext()
{
    if (m_commands.empty())
        return std::nullopt;
    QueuedCommand command = std::move(m_commands.front());
    m_commands.pop_front();
    return command;
}

std::vector<std::uint64_t> PlaybackCommandCoalescer::clear()
{
    std::vector<std::uint64_t> ids;
    for (const QueuedCommand &command : m_commands) {
        ids.push_back(command.commandId);
        ids.insert(
            ids.end(), command.aliases.begin(), command.aliases.end());
    }
    m_commands.clear();
    return ids;
}

std::vector<std::uint64_t>
PlaybackCommandCoalescer::discard(std::uint64_t commandId)
{
    std::vector<std::uint64_t> ids;
    for (auto it = m_commands.begin(); it != m_commands.end(); ++it) {
        if (it->commandId != commandId
            && std::find(
                   it->aliases.begin(), it->aliases.end(),
                   commandId) == it->aliases.end()) {
            continue;
        }
        ids.push_back(it->commandId);
        ids.insert(ids.end(), it->aliases.begin(), it->aliases.end());
        m_commands.erase(it);
        break;
    }
    return ids;
}

std::size_t PlaybackCommandCoalescer::size() const noexcept
{
    return m_commands.size();
}

bool PlaybackCommandCoalescer::empty() const noexcept
{
    return m_commands.empty();
}

bool PlaybackCommandCoalescer::isBoundary(
    PlaybackCommandType type) noexcept
{
    switch (type) {
    case PlaybackCommandType::Open:
    case PlaybackCommandType::Stop:
    case PlaybackCommandType::Next:
    case PlaybackCommandType::Previous:
    case PlaybackCommandType::EndSession:
    case PlaybackCommandType::SetNext:
    case PlaybackCommandType::SetPlayMode:
    case PlaybackCommandType::ResetPictureControls:
    case PlaybackCommandType::SetPeerConnected:
        return true;
    case PlaybackCommandType::Play:
    case PlaybackCommandType::Pause:
    case PlaybackCommandType::Seek:
    case PlaybackCommandType::SetVolume:
    case PlaybackCommandType::SetMuted:
    case PlaybackCommandType::SetPictureControl:
    case PlaybackCommandType::SetCastSession:
        return false;
    }
    return true;
}

std::optional<std::string> PlaybackCommandCoalescer::coalescingKey(
    const PlaybackCommand &command)
{
    const auto generationKey = [&](const char *property) {
        return std::string(property) + '@'
            + std::to_string(command.expectedGeneration);
    };
    switch (command.type) {
    case PlaybackCommandType::Play:
    case PlaybackCommandType::Pause:
        return generationKey("transport.running");
    case PlaybackCommandType::Seek:
        return generationKey("transport.position");
    case PlaybackCommandType::SetVolume:
        return generationKey("rendering.volume");
    case PlaybackCommandType::SetMuted:
        return generationKey("rendering.muted");
    case PlaybackCommandType::SetPictureControl:
        return pictureControlKey(command) + '@'
            + std::to_string(command.expectedGeneration);
    case PlaybackCommandType::SetCastSession:
        return generationKey("session.cast");
    case PlaybackCommandType::Open:
    case PlaybackCommandType::Stop:
    case PlaybackCommandType::Next:
    case PlaybackCommandType::Previous:
    case PlaybackCommandType::EndSession:
    case PlaybackCommandType::SetNext:
    case PlaybackCommandType::SetPlayMode:
    case PlaybackCommandType::ResetPictureControls:
    case PlaybackCommandType::SetPeerConnected:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace mcast::playback
