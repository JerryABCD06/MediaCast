// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ui_process/UiInteractionState.h"

namespace mcast::ui_process {

UiInteractionState::UiInteractionState(QObject *parent)
    : QObject(parent)
{
}

bool UiInteractionState::showCast()
{
    clearOverlay();
    setRoute(Route::Cast);
    setFocusRegion(FocusRegion::Media);
    return true;
}

bool UiInteractionState::showSettings()
{
    if (m_fullscreen)
        return false;

    clearOverlay();
    setRoute(Route::Settings);
    setFocusRegion(FocusRegion::Settings);
    return true;
}

bool UiInteractionState::focusMedia()
{
    if (m_route != Route::Cast || m_overlay != Overlay::None)
        return false;
    return setFocusRegion(FocusRegion::Media);
}

bool UiInteractionState::focusTopBar()
{
    if (m_route != Route::Cast || m_overlay != Overlay::None
        || m_fullscreen) {
        return false;
    }
    return setFocusRegion(FocusRegion::TopBar);
}

bool UiInteractionState::openInfo()
{
    if (m_route != Route::Cast || m_overlay != Overlay::None)
        return false;
    m_focusBeforeOverlay = m_focusRegion;
    setOverlay(Overlay::Info);
    setFocusRegion(FocusRegion::Overlay);
    return true;
}

bool UiInteractionState::openMediaMenu()
{
    if (m_route != Route::Cast || m_overlay != Overlay::None)
        return false;
    m_focusBeforeOverlay = m_focusRegion;
    setOverlay(Overlay::MediaMenu);
    setFocusRegion(FocusRegion::Overlay);
    return true;
}

bool UiInteractionState::closeOverlay()
{
    if (m_overlay == Overlay::None)
        return false;
    clearOverlay();
    if (!overlayFocusAllowed(m_focusBeforeOverlay))
        m_focusBeforeOverlay = m_route == Route::Settings
            ? FocusRegion::Settings : FocusRegion::Media;
    setFocusRegion(m_focusBeforeOverlay);
    return true;
}

bool UiInteractionState::setFullscreen(bool enabled)
{
    if (m_fullscreen == enabled)
        return true;

    m_fullscreen = enabled;
    emit fullscreenChanged();

    if (enabled) {
        clearOverlay();
        setRoute(Route::Cast);
        m_focusBeforeOverlay = FocusRegion::Media;
        setFocusRegion(FocusRegion::Media);
    } else {
        clearOverlay();
        if (m_route == Route::Settings)
            setFocusRegion(FocusRegion::Settings);
        else
            setFocusRegion(FocusRegion::Media);
    }
    return true;
}

void UiInteractionState::noteMouseInput()
{
    setInputMode(InputMode::Mouse);
}

void UiInteractionState::noteKeyboardInput()
{
    setInputMode(InputMode::Keyboard);
}

void UiInteractionState::noteTouchInput()
{
    setInputMode(InputMode::Touch);
}

bool UiInteractionState::setRoute(Route route)
{
    if (m_route == route)
        return false;
    m_route = route;
    emit routeChanged();
    return true;
}

bool UiInteractionState::setFocusRegion(FocusRegion region)
{
    if (m_focusRegion == region)
        return false;
    m_focusRegion = region;
    emit focusRegionChanged();
    return true;
}

bool UiInteractionState::setInputMode(InputMode mode)
{
    if (m_inputMode == mode)
        return false;
    m_inputMode = mode;
    emit inputModeChanged();
    return true;
}

bool UiInteractionState::setOverlay(Overlay overlay)
{
    if (m_overlay == overlay)
        return false;
    m_overlay = overlay;
    emit overlayChanged();
    return true;
}

bool UiInteractionState::clearOverlay()
{
    return setOverlay(Overlay::None);
}

bool UiInteractionState::overlayFocusAllowed(
    FocusRegion region) const
{
    if (region == FocusRegion::Overlay)
        return false;
    if (m_route == Route::Settings)
        return region == FocusRegion::Settings;
    if (region == FocusRegion::Settings)
        return false;
    if (m_fullscreen && region == FocusRegion::TopBar)
        return false;
    return true;
}

} // namespace mcast::ui_process
