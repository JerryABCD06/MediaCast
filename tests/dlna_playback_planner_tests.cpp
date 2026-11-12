// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/playback/DlnaPlaybackPlanner.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using mcast::dlna::planning::DlnaPlaybackPlanner;
using mcast::dlna::planning::PlanningHttpGet;
using mcast::dlna::planning::PlanningHttpResponse;
namespace playback = mcast::playback;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::vector<std::uint8_t> bytes(const std::string &value)
{
    return {
        reinterpret_cast<const std::uint8_t *>(value.data()),
        reinterpret_cast<const std::uint8_t *>(
            value.data() + value.size()),
    };
}

std::vector<std::uint8_t> pngWithOptionalTs(bool wrapped)
{
    std::vector<std::uint8_t> result = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
        0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0,
    };
    if (wrapped) {
        result.resize(result.size() + 3 * 188, 0);
        result[result.size() - 3 * 188] = 0x47;
        result[result.size() - 2 * 188] = 0x47;
        result[result.size() - 188] = 0x47;
    }
    return result;
}

playback::MediaRequest input()
{
    playback::MediaRequest request;
    request.uri = "http://media.test/show.m3u8";
    request.kind = playback::MediaKind::Audio;
    request.protocol = "DLNA";
    request.title = "Episode";
    return request;
}

PlanningHttpGet fakeHttp(
    std::map<std::string, std::vector<std::uint8_t>> bodies)
{
    return [bodies = std::move(bodies)](
        const std::string &url, const std::optional<std::string> &,
        std::size_t, PlanningHttpResponse &response,
        std::string &error) {
        const auto found = bodies.find(url);
        if (found == bodies.end()) {
            error = "not found";
            return false;
        }
        response.status = 200;
        response.finalUrl = url;
        response.body = found->second;
        return true;
    };
}

} // namespace

int main()
{
    {
        const std::string playlist =
            "#EXTM3U\n"
            "#EXTINF:1.5,\n"
            "frames/frame-1.png\n"
            "#EXTINF:2.0,\n"
            "frames/frame-2.png\n"
            "#EXT-X-ENDLIST\n";
        DlnaPlaybackPlanner planner({}, fakeHttp({
            {"http://media.test/show.m3u8", bytes(playlist)},
            {"http://media.test/frames/frame-1.png", pngWithOptionalTs(true)},
            {"http://media.test/frames/frame-2.png", pngWithOptionalTs(false)},
        }));
        const auto planned = planner.plan(input());
        check(planned.route == playback::MediaRoute::WrappedTransportStream,
              "PNG-wrapped transport stream selects its route");
        check(planned.kind == playback::MediaKind::Video,
              "compatibility routes force video kind");
        check(planned.imageSequenceFrames.size() == 2,
              "wrapped plan keeps all playlist segments");
        check(planned.imageSequenceFrames[0].uri
                  == "http://media.test/frames/frame-1.png",
              "relative segment URLs resolve against the playlist");
        check(planned.imageSequenceFrames[0].durationSeconds == 1.5,
              "segment duration survives planning");
        check(planned.title == "Episode",
              "presentation metadata survives planning");
    }
    {
        const std::string playlist =
            "#EXTM3U\n"
            "#EXTINF:0.5,\n"
            "frame.png\n"
            "#EXT-X-ENDLIST\n";
        DlnaPlaybackPlanner planner({}, fakeHttp({
            {"http://media.test/show.m3u8", bytes(playlist)},
            {"http://media.test/frame.png", pngWithOptionalTs(false)},
        }));
        const auto planned = planner.plan(input());
        check(planned.route == playback::MediaRoute::ImageSequence,
              "plain PNG segments select the image sequence route");
        check(planned.imageSequenceFrames.size() == 1,
              "image sequence plan carries its frame table");
    }
    {
        const std::string playlist =
            "#EXTM3U\n"
            "#EXTINF:1.0,\n"
            "segment.ts\n"
            "#EXT-X-ENDLIST\n";
        DlnaPlaybackPlanner planner({}, fakeHttp({
            {"http://media.test/show.m3u8", bytes(playlist)},
        }));
        const auto planned = planner.plan(input());
        check(planned.route == playback::MediaRoute::Native,
              "ordinary HLS stays on the native MPV route");
        check(planned.imageSequenceFrames.empty(),
              "native HLS carries no compatibility frame table");
        check(planned.kind == playback::MediaKind::Audio,
              "planning does not overwrite an unrelated protocol hint");
    }
    {
        DlnaPlaybackPlanner planner({}, fakeHttp({}));
        const auto planned = planner.plan(input());
        check(planned.route == playback::MediaRoute::Native,
              "playlist probe failure falls back to native playback");
    }
    {
        const std::string playlist =
            "#EXTM3U\n"
            "#EXTINF:1.0,\n"
            "frame.png\n"
            "#EXT-X-ENDLIST\n";
        DlnaPlaybackPlanner planner({}, fakeHttp({
            {"http://media.test/show.m3u8", bytes(playlist)},
            {"http://media.test/frame.png", bytes("<html>error</html>")},
        }));
        const auto planned = planner.plan(input());
        check(planned.route == playback::MediaRoute::Native,
              "a non-image first segment falls back to native playback");
    }
    {
        playback::MediaRequest direct = input();
        direct.uri = "http://media.test/movie.mp4";
        bool called = false;
        DlnaPlaybackPlanner planner(
            {}, [&](const std::string &, const std::optional<std::string> &,
                    std::size_t, PlanningHttpResponse &,
                    std::string &) {
                called = true;
                return false;
            });
        const auto planned = planner.plan(direct);
        check(planned.route == playback::MediaRoute::Native && !called,
              "non-HLS media bypasses compatibility probing");
    }

    std::cout << "DLNA playback planner tests passed\n";
    return 0;
}
