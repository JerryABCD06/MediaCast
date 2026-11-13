// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ui_process/UiInteractionState.h"

#include <cstdlib>
#include <iostream>

using mcast::ui_process::UiInteractionState;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    UiInteractionState state;
    check(state.route() == UiInteractionState::Route::Cast,
          "initial route is cast");
    check(state.focusRegion() == UiInteractionState::FocusRegion::Media,
          "initial focus is media");
    check(state.inputMode() == UiInteractionState::InputMode::Mouse,
          "initial input mode is mouse");
    check(state.overlay() == UiInteractionState::Overlay::None,
          "initial overlay is closed");
    check(!state.fullscreen(), "initial window is not fullscreen");

    state.noteTouchInput();
    check(state.inputMode() == UiInteractionState::InputMode::Touch,
          "touch input mode is recorded");
    state.noteKeyboardInput();
    check(state.inputMode() == UiInteractionState::InputMode::Keyboard,
          "keyboard input mode is recorded");
    state.noteMouseInput();
    check(state.inputMode() == UiInteractionState::InputMode::Mouse,
          "mouse input mode is recorded");

    check(state.focusTopBar(), "cast top bar can take focus");
    check(state.focusRegion() == UiInteractionState::FocusRegion::TopBar,
          "top bar focus is recorded");
    check(state.showSettings(), "settings route can be entered");
    check(state.route() == UiInteractionState::Route::Settings
              && state.focusRegion()
                  == UiInteractionState::FocusRegion::Settings,
          "settings route owns settings focus");
    check(!state.focusMedia(),
          "media focus is unavailable while settings is active");
    check(state.showCast(), "cast route can be restored");
    check(state.route() == UiInteractionState::Route::Cast
              && state.focusRegion()
                  == UiInteractionState::FocusRegion::Media,
          "returning to cast restores media focus");

    check(state.focusTopBar(), "top bar focus before info");
    check(state.openInfo(), "info overlay opens");
    check(state.overlay() == UiInteractionState::Overlay::Info
              && state.focusRegion()
                  == UiInteractionState::FocusRegion::Overlay,
          "info overlay owns focus");
    check(!state.openMediaMenu(),
          "another overlay cannot replace the current overlay");
    check(state.closeOverlay(), "info overlay closes");
    check(state.focusRegion() == UiInteractionState::FocusRegion::TopBar,
          "closing info restores previous focus");

    check(state.focusMedia(), "media can own focus");
    check(state.openMediaMenu(), "media menu opens");
    check(state.overlay() == UiInteractionState::Overlay::MediaMenu,
          "media menu overlay is recorded");
    check(state.closeOverlay(), "media menu closes");
    check(state.focusRegion() == UiInteractionState::FocusRegion::Media,
          "closing media menu restores media focus");

    check(state.setFullscreen(true), "fullscreen can be entered");
    check(state.fullscreen()
              && state.route() == UiInteractionState::Route::Cast
              && state.focusRegion()
                  == UiInteractionState::FocusRegion::Media,
          "fullscreen locks route and focus to media");
    check(!state.focusTopBar(),
          "fullscreen prevents top-bar focus");
    check(!state.showSettings(),
          "fullscreen prevents opening settings");
    check(state.openMediaMenu(),
          "fullscreen still allows transient overlays");
    check(state.setFullscreen(false),
          "fullscreen can be exited");
    check(!state.fullscreen()
              && state.overlay() == UiInteractionState::Overlay::None
              && state.focusRegion()
                  == UiInteractionState::FocusRegion::Media,
          "exiting fullscreen clears overlay and restores media focus");

    std::cout << "Ui interaction state tests passed\n";
    return 0;
}
