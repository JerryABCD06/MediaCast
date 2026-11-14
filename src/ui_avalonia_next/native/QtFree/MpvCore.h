// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "QtFree/EventLoop.h"
#include "QtFree/ImageSequenceStrategy.h"
#include "QtFree/MediaPlayer.h"
#include "QtFree/WrappedTsProxy.h"

#include <mpv/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mcast::avalonia::qtfree {

class MpvCore final : public MediaPlayer
{
public:
    explicit MpvCore(EventLoop &loop);
    ~MpvCore() override;

    bool start();
    void shutdown();
    void setRendererAttached(bool attached);
    std::string mediaInfoJson() const override;
    mpv_handle *handle() const noexcept { return m_mpv; }

    void load(const MediaLoadRequest &request) override;
    void play() override;
    void pause() override;
    void stop() override;
    void unload() override;
    void seekTo(double seconds) override;
    void setVolumePercent(int percent) override;
    void setMuted(bool muted) override;

    std::vector<PictureControlInfo> pictureControls() const override;
    bool setPictureControl(
        const std::string &name, int value) override;
    int pictureControlValue(
        const std::string &name) const override;

    double positionSeconds() const override
    {
        return m_positionSeconds.load();
    }
    double durationSeconds() const override
    {
        return m_durationSeconds.load();
    }
    std::int64_t mediaSizeBytes() const override;
    int volumePercent() const override
    {
        return m_volumePercent.load();
    }
    bool isMuted() const override { return m_muted.load(); }

private:
    void eventLoop(std::stop_token stop);
    void activateLoad(const MediaLoadRequest &request);
    void loadNow(const std::string &uri);
    bool loadImageSequencePlan(
        const MediaLoadRequest &request);
    bool loadMfImageSequence(
        const std::vector<MediaLoadFrame> &segments,
        const std::string &logicalUri);
    void loadMpvFile(
        const std::string &mpvUri,
        const std::string &logicalUri,
        bool emitStarted = true);
    void resetLoadState(const std::string &logicalUri);
    std::string logicalMediaUri(const std::string &mpvUri) const;
    bool mediaEventsCurrent() const;
    void publishMediaSummary(std::uint64_t expectedEpoch);
    void clearMediaSummary();
    void finishPacketCorruptBurst();
    bool applySharpen(int value);

    template <typename Function>
    bool post(Function &&function)
    {
        return m_loop.post(std::forward<Function>(function));
    }

    EventLoop &m_loop;
    mpv_handle *m_mpv = nullptr;
    std::jthread m_eventThread;
    std::atomic_bool m_stopRequested{false};
    std::atomic_bool m_rendererAttached{false};
    std::atomic_bool m_eofReached{false};
    std::atomic_bool m_playOnLoad{true};
    std::atomic_bool m_loadFailed{false};
    std::unique_ptr<ImageSequenceStrategy> m_imageSequence;
    std::vector<Connection> m_imageSequenceConnections;
    std::unique_ptr<WrappedTsProxy> m_wrappedTsProxy;
    std::atomic_bool m_imageSequenceActive{false};
    std::atomic_uint64_t m_imageSequenceLoadGeneration{0};
    std::atomic_uint64_t m_pendingLoadGeneration{0};
    std::atomic_uint64_t m_activeLoadGeneration{0};
    std::atomic_uint64_t m_mediaEpoch{0};
    std::atomic_bool m_mediaSummaryReady{false};
    std::atomic_uint64_t m_currentLoadCommandEpoch{0};
    std::atomic_uint64_t m_eventLoadCommandEpoch{0};
    std::atomic_uint64_t m_commandMediaEpoch{0};
    std::atomic_uint64_t m_eventMediaEpoch{0};
    std::optional<MediaLoadRequest> m_pendingLoad;
    std::optional<MediaLoadRequest> m_lastLoadRequest;
    mutable std::mutex m_uriMutex;
    std::string m_lastRequestedUri;
    std::string m_activeMpvUri;
    std::string m_eventMediaUri;
    mutable std::mutex m_mediaInfoMutex;
    std::string m_mediaInfoJson = "{}";
    std::atomic<double> m_positionSeconds{0};
    std::atomic<double> m_durationSeconds{0};
    std::atomic<int> m_volumePercent{100};
    std::atomic_bool m_muted{false};
    int m_sharpen = 0;
    bool m_packetCorruptBurst = false;
    int m_packetCorruptCount = 0;
    std::chrono::steady_clock::time_point m_packetCorruptLast;
};

} // namespace mcast::avalonia::qtfree
