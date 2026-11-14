// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/WindowsMediaControls.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <roapi.h>
#include <systemmediatransportcontrolsinterop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <algorithm>
#include <string>

namespace WMedia = winrt::Windows::Media;
namespace WFoundation = winrt::Windows::Foundation;
namespace WStorageFile = winrt::Windows::Storage;
namespace WStorage = winrt::Windows::Storage::Streams;

namespace mcast::avalonia::qtfree {
namespace {

using WinrtTimeSpan =
    std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;

WinrtTimeSpan toTimeSpan(double seconds)
{
    if (!(seconds > 0))
        seconds = 0;
    return std::chrono::duration_cast<WinrtTimeSpan>(
        std::chrono::duration<double>(seconds));
}

winrt::hstring toHString(std::string_view text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        wide.data(), length);
    return winrt::hstring(wide);
}

std::string sourceLabel(MediaSource source)
{
    return source == MediaSource::Local ? "Local playback" : "Cast";
}

} // namespace

struct WindowsMediaControls::Impl
{
    explicit Impl(EventLoop &eventLoop)
        : loop(eventLoop)
    {
    }

    void enableForMedia(bool value)
    {
        if (enabled == value)
            return;
        smtc.IsEnabled(value);
        enabled = value;
    }

    EventLoop &loop;
    HWND window = nullptr;
    WMedia::SystemMediaTransportControls smtc{nullptr};
    winrt::event_token buttonToken{};
    winrt::event_token seekToken{};
    bool buttonSubscribed = false;
    bool seekSubscribed = false;
    bool attached = false;
    bool enabled = false;
    std::atomic_bool alive{true};
    NowPlaying info;
    bool hasInfo = false;
    double position = 0;
    double duration = 0;
};

WindowsMediaControls::WindowsMediaControls(EventLoop &loop)
    : m_impl(std::make_unique<Impl>(loop))
{
}

WindowsMediaControls::~WindowsMediaControls()
{
    detach();
}

bool WindowsMediaControls::attach()
{
    if (m_impl->attached)
        return true;
    try {
        try {
            winrt::init_apartment(
                winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error &) {
        }

        m_impl->window = CreateWindowExW(
            0, L"STATIC", L"MCast", WS_OVERLAPPED,
            0, 0, 1, 1, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (!m_impl->window)
            return false;
        ShowWindow(m_impl->window, SW_HIDE);

        winrt::hstring className =
            L"Windows.Media.SystemMediaTransportControls";
        winrt::com_ptr<ISystemMediaTransportControlsInterop> interop;
        winrt::check_hresult(RoGetActivationFactory(
            reinterpret_cast<HSTRING>(winrt::get_abi(className)),
            winrt::guid_of<ISystemMediaTransportControlsInterop>(),
            interop.put_void()));
        WMedia::SystemMediaTransportControls smtc{nullptr};
        winrt::check_hresult(interop->GetForWindow(
            m_impl->window,
            winrt::guid_of<WMedia::SystemMediaTransportControls>(),
            winrt::put_abi(smtc)));
        m_impl->smtc = smtc;
        m_impl->smtc.IsPlayEnabled(true);
        m_impl->smtc.IsPauseEnabled(true);
        m_impl->smtc.IsStopEnabled(true);
        m_impl->smtc.IsNextEnabled(false);
        m_impl->smtc.IsPreviousEnabled(false);

        const std::weak_ptr<WindowsMediaControls> weak =
            weak_from_this();
        if (weak.expired())
            return false;
        m_impl->buttonToken = m_impl->smtc.ButtonPressed(
            [weak](
                const WMedia::SystemMediaTransportControls &,
                const WMedia::SystemMediaTransportControlsButtonPressedEventArgs &args) {
                const auto self = weak.lock();
                if (!self || !self->m_impl->alive)
                    return;
                const auto button = args.Button();
                self->m_impl->loop.post([weak, button] {
                    const auto current = weak.lock();
                    if (!current || !current->m_impl->alive)
                        return;
                    switch (button) {
                    case WMedia::SystemMediaTransportControlsButton::Play:
                        current->playRequested.emit();
                        break;
                    case WMedia::SystemMediaTransportControlsButton::Pause:
                        current->pauseRequested.emit();
                        break;
                    case WMedia::SystemMediaTransportControlsButton::Stop:
                        current->stopRequested.emit();
                        break;
                    case WMedia::SystemMediaTransportControlsButton::Next:
                        current->nextRequested.emit();
                        break;
                    case WMedia::SystemMediaTransportControlsButton::Previous:
                        current->previousRequested.emit();
                        break;
                    default:
                        break;
                    }
                });
            });
        m_impl->buttonSubscribed = true;

        m_impl->seekToken =
            m_impl->smtc.PlaybackPositionChangeRequested(
                [weak](
                    const WMedia::SystemMediaTransportControls &,
                    const WMedia::PlaybackPositionChangeRequestedEventArgs &args) {
                    const auto self = weak.lock();
                    if (!self || !self->m_impl->alive)
                        return;
                    const double seconds =
                        std::chrono::duration_cast<
                            std::chrono::duration<double>>(
                            args.RequestedPlaybackPosition())
                            .count();
                    self->m_impl->loop.post([weak, seconds] {
                        const auto current = weak.lock();
                        if (current && current->m_impl->alive)
                            current->seekRequested.emit(seconds);
                    });
                });
        m_impl->seekSubscribed = true;
        m_impl->smtc.PlaybackStatus(WMedia::MediaPlaybackStatus::Closed);
        m_impl->smtc.IsEnabled(false);
    } catch (const winrt::hresult_error &error) {
        detach();
        logMessage.emit(
            "Windows media controls unavailable: "
            + winrt::to_string(error.message()));
        return false;
    }
    m_impl->attached = true;
    logMessage.emit("Windows media controls attached");
    return true;
}

void WindowsMediaControls::detach()
{
    if (!m_impl)
        return;
    m_impl->alive = false;
    try {
        if (m_impl->enabled) {
            m_impl->smtc.PlaybackStatus(
                WMedia::MediaPlaybackStatus::Closed);
            m_impl->smtc.IsEnabled(false);
        }
    } catch (...) {
    }
    try {
        if (m_impl->buttonSubscribed)
            m_impl->smtc.ButtonPressed(m_impl->buttonToken);
        if (m_impl->seekSubscribed) {
            m_impl->smtc.PlaybackPositionChangeRequested(
                m_impl->seekToken);
        }
    } catch (...) {
    }
    m_impl->buttonSubscribed = false;
    m_impl->seekSubscribed = false;
    m_impl->smtc = nullptr;
    m_impl->enabled = false;
    m_impl->attached = false;
    if (m_impl->window) {
        DestroyWindow(m_impl->window);
        m_impl->window = nullptr;
    }
}

void WindowsMediaControls::setNowPlaying(const NowPlaying &info)
{
    m_impl->info = info;
    m_impl->hasInfo = true;
    if (!m_impl->attached || !m_impl->enabled)
        return;
    try {
        auto updater = m_impl->smtc.DisplayUpdater();
        updater.ClearAll();
        const winrt::hstring title = toHString(
            info.title.empty() ? "Unknown" : info.title);
        const std::string subtitleText =
            sourceLabel(info.source) + " - "
            + (info.subtitle.empty() ? "Unknown" : info.subtitle);
        const winrt::hstring subtitle = toHString(subtitleText);
        switch (info.kind) {
        case MediaKind::Audio: {
            updater.Type(WMedia::MediaPlaybackType::Music);
            auto music = updater.MusicProperties();
            music.Title(title);
            music.Artist(subtitle);
            music.AlbumTitle(toHString(info.album));
            break;
        }
        case MediaKind::Image: {
            updater.Type(WMedia::MediaPlaybackType::Image);
            auto image = updater.ImageProperties();
            image.Title(title);
            image.Subtitle(subtitle);
            break;
        }
        default: {
            updater.Type(WMedia::MediaPlaybackType::Video);
            auto video = updater.VideoProperties();
            video.Title(title);
            video.Subtitle(subtitle);
            break;
        }
        }
        if (!info.artworkSource.empty()) {
            try {
                const std::string lower =
                    mcast::avalonia::qtfree::lowercaseAscii(
                        info.artworkSource);
                if (lower.starts_with("file:")) {
                    const WFoundation::Uri uri(
                        toHString(info.artworkSource));
                    std::wstring path(uri.Path());
                    if (path.size() >= 3
                        && path.front() == L'/'
                        && std::isalpha(
                               static_cast<unsigned char>(path[1]))
                            != 0
                        && path[2] == L':') {
                        path.erase(path.begin());
                    }
                    const auto file =
                        WStorageFile::StorageFile::GetFileFromPathAsync(
                            winrt::hstring(path)).get();
                    updater.Thumbnail(
                        WStorage::RandomAccessStreamReference::
                            CreateFromFile(file));
                } else {
                    const WFoundation::Uri uri(
                        toHString(info.artworkSource));
                    updater.Thumbnail(
                        WStorage::RandomAccessStreamReference::
                            CreateFromUri(uri));
                }
            } catch (...) {
            }
        }
        updater.Update();
    } catch (const winrt::hresult_error &error) {
        logMessage.emit(
            "Windows media controls metadata failed: "
            + winrt::to_string(error.message()));
    }
}

void WindowsMediaControls::setTransportState(
    PlaybackController::State state)
{
    if (!m_impl->attached)
        return;
    const bool hasMedia = state != PlaybackController::State::NoMedia
        && m_impl->hasInfo && !m_impl->info.isEmpty();
    try {
        if (!hasMedia) {
            if (m_impl->enabled) {
                m_impl->smtc.PlaybackStatus(
                    WMedia::MediaPlaybackStatus::Closed);
                m_impl->enableForMedia(false);
            }
            return;
        }
        if (!m_impl->enabled) {
            m_impl->enableForMedia(true);
            setNowPlaying(m_impl->info);
        }
        WMedia::MediaPlaybackStatus status =
            WMedia::MediaPlaybackStatus::Stopped;
        switch (state) {
        case PlaybackController::State::NoMedia:
            status = WMedia::MediaPlaybackStatus::Closed;
            break;
        case PlaybackController::State::Stopped:
            status = WMedia::MediaPlaybackStatus::Stopped;
            break;
        case PlaybackController::State::Preparing:
            status = WMedia::MediaPlaybackStatus::Changing;
            break;
        case PlaybackController::State::Playing:
            status = WMedia::MediaPlaybackStatus::Playing;
            break;
        case PlaybackController::State::Paused:
            status = WMedia::MediaPlaybackStatus::Paused;
            break;
        }
        m_impl->smtc.PlaybackStatus(status);
    } catch (const winrt::hresult_error &error) {
        logMessage.emit(
            "Windows media controls state failed: "
            + winrt::to_string(error.message()));
    }
}

void WindowsMediaControls::setPosition(double seconds)
{
    m_impl->position = seconds;
    if (!m_impl->attached || !m_impl->enabled
        || !(m_impl->duration > 0)) {
        return;
    }
    try {
        WMedia::SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(toTimeSpan(0));
        timeline.EndTime(toTimeSpan(m_impl->duration));
        timeline.MinSeekTime(toTimeSpan(0));
        timeline.MaxSeekTime(toTimeSpan(m_impl->duration));
        timeline.Position(toTimeSpan(m_impl->position));
        m_impl->smtc.UpdateTimelineProperties(timeline);
    } catch (...) {
    }
}

void WindowsMediaControls::setDuration(double seconds)
{
    m_impl->duration = seconds;
    setPosition(m_impl->position);
}

void WindowsMediaControls::setQueueAvailability(
    bool hasNext, bool hasPrevious)
{
    if (!m_impl->attached)
        return;
    try {
        m_impl->smtc.IsNextEnabled(hasNext);
        m_impl->smtc.IsPreviousEnabled(hasPrevious);
    } catch (...) {
    }
}

} // namespace mcast::avalonia::qtfree
