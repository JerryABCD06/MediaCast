// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <functional>
#include <memory>

class PlaybackController;
class QString;

/**
 * 平台媒体控件接入点。
 *
 * Ui 主流程只负责创建和初始化它；标题、进度、队列、按钮回调和平台日志都在
 * 具体平台实现里接好。这样主流程不依赖 Windows、SMTC 或任何平台窗口。
 */
class MediaControls
{
public:
    using LogSink = std::function<void(const QString &)>;

    virtual ~MediaControls() = default;
    virtual bool initialize(const LogSink &logSink) = 0;
    virtual void shutdown() = 0;
};

std::unique_ptr<MediaControls> createPlatformMediaControls(
    PlaybackController &playback);
