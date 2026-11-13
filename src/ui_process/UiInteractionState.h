// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QObject>

namespace mcast::ui_process {

/**
 * Ui interaction state.
 *
 * This is transient process-local presentation state. It deliberately does
 * not own media or persistent settings: media commands still go through the
 * PlaybackBridge, and persisted settings still go through the Shell.
 */
class UiInteractionState final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(Route route READ route NOTIFY routeChanged)
    Q_PROPERTY(
        FocusRegion focusRegion READ focusRegion
        NOTIFY focusRegionChanged)
    Q_PROPERTY(
        InputMode inputMode READ inputMode
        NOTIFY inputModeChanged)
    Q_PROPERTY(Overlay overlay READ overlay NOTIFY overlayChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)

public:
    enum class Route {
        Cast,
        Settings,
    };
    Q_ENUM(Route)

    enum class FocusRegion {
        Media,
        TopBar,
        Settings,
        Overlay,
    };
    Q_ENUM(FocusRegion)

    enum class InputMode {
        Mouse,
        Keyboard,
        Touch,
    };
    Q_ENUM(InputMode)

    enum class Overlay {
        None,
        Info,
        MediaMenu,
    };
    Q_ENUM(Overlay)

    explicit UiInteractionState(QObject *parent = nullptr);

    Route route() const { return m_route; }
    FocusRegion focusRegion() const { return m_focusRegion; }
    InputMode inputMode() const { return m_inputMode; }
    Overlay overlay() const { return m_overlay; }
    bool fullscreen() const { return m_fullscreen; }

    Q_INVOKABLE bool showCast();
    Q_INVOKABLE bool showSettings();
    Q_INVOKABLE bool focusMedia();
    Q_INVOKABLE bool focusTopBar();

    Q_INVOKABLE bool openInfo();
    Q_INVOKABLE bool openMediaMenu();
    Q_INVOKABLE bool closeOverlay();

    Q_INVOKABLE bool setFullscreen(bool enabled);

    Q_INVOKABLE void noteMouseInput();
    Q_INVOKABLE void noteKeyboardInput();
    Q_INVOKABLE void noteTouchInput();

signals:
    void routeChanged();
    void focusRegionChanged();
    void inputModeChanged();
    void overlayChanged();
    void fullscreenChanged();

private:
    bool setRoute(Route route);
    bool setFocusRegion(FocusRegion region);
    bool setInputMode(InputMode mode);
    bool setOverlay(Overlay overlay);
    bool clearOverlay();
    bool overlayFocusAllowed(FocusRegion region) const;

    Route m_route = Route::Cast;
    FocusRegion m_focusRegion = FocusRegion::Media;
    InputMode m_inputMode = InputMode::Mouse;
    Overlay m_overlay = Overlay::None;
    FocusRegion m_focusBeforeOverlay = FocusRegion::Media;
    bool m_fullscreen = false;
};

} // namespace mcast::ui_process
