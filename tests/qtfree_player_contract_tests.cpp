// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/MediaPlayer.h"
#include "QtFree/RenderWakeLatch.h"

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class PlayerContract final
    : public mcast::avalonia::qtfree::MediaPlayer
{
public:
    void load(const mcast::avalonia::qtfree::MediaLoadRequest &request) override
    {
        loaded = request.logicalUri;
    }
    void play() override { playing = true; }
    void pause() override { playing = false; }
    void stop() override { playing = false; stopped = true; }
    void seekTo(double seconds) override { position = seconds; }
    void setVolumePercent(int percent) override { volume = percent; }
    void setMuted(bool value) override { muted = value; }
    double positionSeconds() const override { return position; }
    double durationSeconds() const override { return 120.0; }
    int volumePercent() const override { return volume; }
    bool isMuted() const override { return muted; }
    std::vector<mcast::avalonia::qtfree::PictureControlInfo>
    pictureControls() const override
    {
        return {{"brightness", -100, 100, 0}};
    }
    bool setPictureControl(
        const std::string &name, int value) override
    {
        if (name != "brightness")
            return false;
        picture = value;
        return true;
    }
    int pictureControlValue(const std::string &name) const override
    {
        return name == "brightness" ? picture : 0;
    }

    std::string loaded;
    bool playing = false;
    bool stopped = false;
    double position = 0;
    int volume = 100;
    bool muted = false;
    int picture = 0;
};

} // namespace

int main()
{
    PlayerContract player;
    player.load({"https://example.com/movie.mp4"});
    check(
        player.loaded == "https://example.com/movie.mp4",
        "player receives neutral load request");
    player.play();
    player.seekTo(42);
    player.setVolumePercent(35);
    player.setMuted(true);
    check(player.playing && player.position == 42,
          "playback commands use one interface");
    check(player.volume == 35 && player.muted,
          "rendering facts use one interface");
    player.resetPictureControls();
    check(player.picture == 0, "picture controls reset to neutral");

    mcast::avalonia::qtfree::RenderWakeLatch latch;
    check(latch.mark(), "first render request is emitted");
    check(!latch.mark(), "duplicate render request is coalesced");
    check(latch.consume(), "pending render request is consumed");
    check(!latch.consume(), "render request cannot be consumed twice");
    check(latch.mark(), "render request re-arms after consumption");

    std::cout << "Qt-free player contract tests passed\n";
    return 0;
}
