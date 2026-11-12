// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/PlaybackCommandCoalescer.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>

using namespace mcast::playback;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

PlaybackCommand command(
    PlaybackCommandType type, const std::string &text = {})
{
    PlaybackCommand result;
    result.type = type;
    result.text = text;
    return result;
}

PlaybackCommandCoalescer::QueuedCommand queued(
    std::uint64_t id, PlaybackCommandType type,
    const std::string &text = {}, std::uint64_t generation = 0)
{
    PlaybackCommand value = command(type, text);
    value.expectedGeneration = generation;
    return {id, value, {}};
}

std::vector<PlaybackCommandType> types(PlaybackCommandCoalescer &queue)
{
    std::vector<PlaybackCommandType> result;
    while (auto item = queue.takeNext())
        result.push_back(item->command.type);
    return result;
}

void testSamePropertyKeepsNewest()
{
    PlaybackCommandCoalescer queue;
    check(queue.enqueue(queued(1, PlaybackCommandType::Play)).queued,
          "play queues");
    const auto pause = queue.enqueue(queued(2, PlaybackCommandType::Pause));
    check(pause.queued && pause.supersededCommandIds == std::vector{1ULL},
          "pause supersedes an unsent play");
    const auto list = types(queue);
    check(list == std::vector{PlaybackCommandType::Pause},
          "only the newest running-state command remains");
}

void testBoundaryPreservesBothSides()
{
    PlaybackCommandCoalescer queue;
    queue.enqueue(queued(1, PlaybackCommandType::Play));
    queue.enqueue(queued(2, PlaybackCommandType::Stop));
    const auto pause = queue.enqueue(queued(3, PlaybackCommandType::Pause));
    check(pause.supersededCommandIds.empty(),
          "a stop boundary protects commands on both sides");
    const auto list = types(queue);
    check(list == std::vector{
              PlaybackCommandType::Play,
              PlaybackCommandType::Stop,
              PlaybackCommandType::Pause},
          "stop remains an ordering boundary");
}

void testScanCrossesDifferentProperties()
{
    PlaybackCommandCoalescer queue;
    queue.enqueue(queued(1, PlaybackCommandType::Seek));
    queue.enqueue(queued(2, PlaybackCommandType::SetVolume));
    const auto seek = queue.enqueue(queued(3, PlaybackCommandType::Seek));
    check(seek.supersededCommandIds == std::vector{1ULL},
          "the later absolute seek supersedes the earlier one");
    const auto list = types(queue);
    check(list == std::vector{
              PlaybackCommandType::SetVolume,
              PlaybackCommandType::Seek},
          "unrelated properties retain their order");
}

void testRelativeActionsAndPictureProperties()
{
    PlaybackCommandCoalescer queue;
    queue.enqueue(queued(1, PlaybackCommandType::Next));
    queue.enqueue(queued(2, PlaybackCommandType::Next));
    queue.enqueue(queued(3, PlaybackCommandType::Previous));
    const auto brightness1 = queue.enqueue(queued(
        4, PlaybackCommandType::SetPictureControl, "brightness"));
    const auto contrast = queue.enqueue(queued(
        5, PlaybackCommandType::SetPictureControl, "contrast"));
    const auto brightness2 = queue.enqueue(queued(
        6, PlaybackCommandType::SetPictureControl, "brightness"));
    check(brightness1.supersededCommandIds.empty()
              && contrast.supersededCommandIds.empty()
              && brightness2.supersededCommandIds
                  == std::vector{4ULL},
          "relative actions never merge and picture controls merge by name");
    const auto list = types(queue);
    check(list == std::vector{
              PlaybackCommandType::Next,
              PlaybackCommandType::Next,
              PlaybackCommandType::Previous,
              PlaybackCommandType::SetPictureControl,
              PlaybackCommandType::SetPictureControl},
          "next/previous remain ordered");
}

void testAliasFlattening()
{
    PlaybackCommandCoalescer queue;
    queue.enqueue(queued(1, PlaybackCommandType::SetVolume));
    queue.enqueue(queued(2, PlaybackCommandType::SetVolume));
    const auto third =
        queue.enqueue(queued(3, PlaybackCommandType::SetVolume));
    const std::set<std::uint64_t> ids(
        third.supersededCommandIds.begin(),
        third.supersededCommandIds.end());
    check(ids == std::set<std::uint64_t>{1, 2},
          "all superseded ids are reported once");
    auto item = queue.takeNext();
    check(item && item->commandId == 3
              && std::set<std::uint64_t>(
                     item->aliases.begin(), item->aliases.end())
                  == std::set<std::uint64_t>{1, 2},
          "the surviving command owns every superseded id");
}

void testGenerationIsABoundary()
{
    PlaybackCommandCoalescer queue;
    queue.enqueue(queued(1, PlaybackCommandType::SetVolume, {}, 1));
    const auto second =
        queue.enqueue(queued(2, PlaybackCommandType::SetVolume, {}, 2));
    check(second.supersededCommandIds.empty(),
          "a command from another playback generation is not merged");
    const auto list = types(queue);
    check(list == std::vector{
              PlaybackCommandType::SetVolume,
              PlaybackCommandType::SetVolume},
          "generation changes retain an ordering boundary");
}

void testDiscardCancelsUnsentCommand()
{
    PlaybackCommandCoalescer queue;
    queue.enqueue(queued(1, PlaybackCommandType::SetVolume));
    const auto discarded = queue.discard(1);
    check(discarded == std::vector{1ULL} && queue.empty(),
          "a timed-out command can be removed before the writer sends it");
}

void testEndSessionClearsUnsentWork()
{
    PlaybackCommandCoalescer queue;
    queue.enqueue(queued(1, PlaybackCommandType::Open));
    queue.enqueue(queued(2, PlaybackCommandType::SetVolume));
    queue.enqueue(queued(3, PlaybackCommandType::Next));
    const auto ending =
        queue.enqueue(queued(4, PlaybackCommandType::EndSession));
    check(ending.queued
              && std::set<std::uint64_t>(
                     ending.supersededCommandIds.begin(),
                     ending.supersededCommandIds.end())
                  == std::set<std::uint64_t>{1, 2, 3},
          "disconnect supersedes every unsent media command");
    const auto list = types(queue);
    check(list == std::vector{PlaybackCommandType::EndSession},
          "disconnect is the only queued command");
}

void testPeerTransitionsRemainOrdered()
{
    PlaybackCommandCoalescer queue;
    queue.enqueue(queued(1, PlaybackCommandType::SetPeerConnected));
    queue.enqueue(queued(2, PlaybackCommandType::SetPeerConnected));
    const auto list = types(queue);
    check(list == std::vector{
              PlaybackCommandType::SetPeerConnected,
              PlaybackCommandType::SetPeerConnected},
          "peer false/true transitions are never coalesced");
}

void testCapacityIsExplicit()
{
    PlaybackCommandCoalescer queue(1);
    check(queue.enqueue(queued(1, PlaybackCommandType::Stop)).queued,
          "first command fits");
    const auto overflow =
        queue.enqueue(queued(2, PlaybackCommandType::Stop));
    check(!overflow.queued && queue.size() == 1,
          "queue overflow is reported without dropping existing work");
}

} // namespace

int main()
{
    testSamePropertyKeepsNewest();
    testBoundaryPreservesBothSides();
    testScanCrossesDifferentProperties();
    testRelativeActionsAndPictureProperties();
    testAliasFlattening();
    testGenerationIsABoundary();
    testDiscardCancelsUnsentCommand();
    testEndSessionClearsUnsentWork();
    testPeerTransitionsRemainOrdered();
    testCapacityIsExplicit();
    std::cout << "Playback command coalescer tests passed\n";
    return 0;
}
