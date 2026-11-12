// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "playback/PlaybackTypes.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mcast::dlna::planning {

struct PlanningHttpResponse {
    int status = 0;
    std::string finalUrl;
    std::vector<std::uint8_t> body;
};

using PlanningHttpGet = std::function<bool(
    const std::string &url,
    const std::optional<std::string> &range,
    std::size_t maxBytes,
    PlanningHttpResponse &response,
    std::string &error)>;

/**
 * DLNA/HLS playback planner.
 *
 * It resolves the quirks of non-standard control points before any Ui process
 * exists and returns only a protocol-neutral MediaRequest plan.
 */
class DlnaPlaybackPlanner
{
public:
    using Log = std::function<void(const std::string &)>;

    explicit DlnaPlaybackPlanner(
        Log log = {}, PlanningHttpGet httpGet = {});

    ::mcast::playback::MediaRequest plan(
        const ::mcast::playback::MediaRequest &request) const;

private:
    bool get(
        const std::string &url,
        const std::optional<std::string> &range,
        std::size_t maxBytes,
        PlanningHttpResponse &response,
        std::string &error) const;
    void log(const std::string &message) const;

    Log m_log;
    PlanningHttpGet m_httpGet;
};

} // namespace mcast::dlna::planning
