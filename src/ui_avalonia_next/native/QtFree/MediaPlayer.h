// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "QtFree/MediaTypes.h"
#include "QtFree/Signal.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mcast::avalonia::qtfree {

class MediaPlayer
{
public:
    virtual ~MediaPlayer() = default;

    virtual void load(const MediaLoadRequest &request) = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void unload() { stop(); }
    virtual void seekTo(double seconds) = 0;
    virtual void setVolumePercent(int percent) = 0;
    virtual void setMuted(bool muted) = 0;

    virtual double positionSeconds() const = 0;
    virtual double durationSeconds() const = 0;
    virtual std::int64_t mediaSizeBytes() const { return 0; }
    virtual int volumePercent() const = 0;
    virtual bool isMuted() const = 0;
    virtual std::string mediaInfoJson() const { return "{}"; }

    virtual std::vector<PictureControlInfo> pictureControls() const = 0;
    virtual bool setPictureControl(
        const std::string &name, int value) = 0;
    virtual int pictureControlValue(
        const std::string &name) const = 0;

    void resetPictureControls()
    {
        for (const auto &control : pictureControls())
            setPictureControl(control.name, control.neutral);
    }

    Signal<const std::string &> statusChanged;
    Signal<const std::string &> logMessage;
    Signal<bool> runningChanged;
    Signal<> mediaInfoChanged;
    Signal<> renderUpdate;
    Signal<> rendererReady;
    Signal<std::uint64_t> ready;
    Signal<std::uint64_t> loadStarted;
    Signal<> ended;
    Signal<std::uint64_t, const std::string &> loadFailed;
    Signal<> lost;
    Signal<double> positionChanged;
    Signal<double> durationChanged;
    Signal<int> volumeChanged;
    Signal<bool> muteChanged;
    Signal<bool> pausedChanged;
    Signal<const std::string &, int> pictureControlChanged;
    Signal<
        const std::map<std::string, std::string, std::less<>> &,
        const std::string &>
        metadataChanged;
    Signal<const std::string &, const std::string &> artworkChanged;
    Signal<MediaKind, const std::string &> mediaKindChanged;
};

} // namespace mcast::avalonia::qtfree

