// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#define MCAST_AVALONIA_HOST_EXPORT extern "C" __declspec(dllexport)
#else
#define MCAST_AVALONIA_HOST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using McastAvaloniaLogCallback =
    void (*)(const char *message, void *context);
using McastAvaloniaProjectionUpdateCallback =
    void (*)(std::uint32_t dirtyDomains, void *context);
using McastAvaloniaRenderUpdateCallback =
    void (*)(void *context);
using McastAvaloniaGetProcAddress =
    void *(*)(void *context, const char *name);

enum McastAvaloniaDirtyDomain : std::uint32_t
{
    MCAST_AVALONIA_DIRTY_SHELL = 1u << 0,
    MCAST_AVALONIA_DIRTY_PLAYBACK = 1u << 1,
    MCAST_AVALONIA_DIRTY_RENDERER = 1u << 2,
    MCAST_AVALONIA_DIRTY_CONFIRMATION = 1u << 3,
    MCAST_AVALONIA_DIRTY_ALL = (1u << 4) - 1,
};

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_create(
    int argumentCount, const char *const *arguments,
    McastAvaloniaLogCallback logCallback, void *context);

MCAST_AVALONIA_HOST_EXPORT void mcast_avalonia_host_destroy();

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_projection_update_callback(
    McastAvaloniaProjectionUpdateCallback callback, void *context);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_render_update_callback(
    McastAvaloniaRenderUpdateCallback callback, void *context);

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_read_shell_json(
    char *buffer, int capacity, int *required);
MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_read_language_json(
    char *buffer, int capacity, int *required);
MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_read_playback_json(
    char *buffer, int capacity, int *required);
MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_read_renderer_json(
    char *buffer, int capacity, int *required);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_read_confirmation_json(
    char *buffer, int capacity, int *required);

MCAST_AVALONIA_HOST_EXPORT void mcast_avalonia_host_notify_first_frame();

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_render_attach(
    McastAvaloniaGetProcAddress getProcAddress, void *context);
MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_render_frame(
    int framebuffer, int width, int height, int flipY);
MCAST_AVALONIA_HOST_EXPORT void mcast_avalonia_host_render_detach();
MCAST_AVALONIA_HOST_EXPORT void mcast_avalonia_host_render_lost(
    const char *reason);

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_toggle_play_pause();
MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_seek(double seconds);
MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_previous();
MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_next();
MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_set_volume(int percent);
MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_set_muted(int muted);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_picture_control(
    const char *name, int value);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_reset_picture_controls();
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_begin_control_adjustment();
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_end_control_adjustment();
MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_end_cast();
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_pause_and_flush_for_window_close();

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_language(const char *language);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_theme_mode(int mode);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_accent_mode(int mode);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_mica(int enabled);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_accept_new_cast(int enabled);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_broadcast(int enabled);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_broadcast_interval(int milliseconds);
MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_respond_cast_confirmation(
    std::uint64_t requestId, int accepted);
