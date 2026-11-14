// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "QtFree/MediaTypes.h"
#include "QtFree/Signal.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mcast::avalonia::qtfree {

class WrappedTsProxy
{
public:
    WrappedTsProxy();
    ~WrappedTsProxy();

    WrappedTsProxy(const WrappedTsProxy &) = delete;
    WrappedTsProxy &operator=(const WrappedTsProxy &) = delete;

    bool start(
        std::vector<MediaLoadFrame> segments,
        std::string &error);
    void reset();

    std::string segmentUrl(std::size_t index) const;
    std::string playlistUrl() const;

    Signal<const std::string &> logMessage;

    struct State;

private:
    std::shared_ptr<State> m_state;
};

} // namespace mcast::avalonia::qtfree

