using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Threading;

using FluentAvalonia.UI.Controls;

using MCast.Ui.Avalonia.Contracts;
using MCast.Ui.Avalonia.Diagnostics;
using MCast.Ui.Avalonia.Native;
using MCast.Ui.Avalonia.Platform;
using MCast.Ui.Avalonia.Views;

namespace MCast.Ui.Avalonia;

internal sealed partial class MainWindow : Window
{
    private readonly NativeAvaloniaHost _host;
    private readonly UiDiagnosticLog _log;
    private readonly CastPage _castPage;
    private readonly SettingsPage _settingsPage;
    private WindowsWindowInteractionMonitor? _windowInteractionMonitor;
    private bool _firstFrameSent;
    private bool _allowClose;
    private bool _windowInteractionActive;
    private bool _closeDialogOpen;

    internal MainWindow()
        : this(null!, null!)
    {
    }

    internal MainWindow(
        NativeAvaloniaHost host, UiDiagnosticLog log)
    {
        InitializeComponent();
        _host = host;
        _log = log;
        _castPage = new CastPage(host);
        _settingsPage = new SettingsPage(host);
        ContentHost.Children.Add(_castPage);
        ContentHost.Children.Add(_settingsPage);
        _settingsPage.IsVisible = false;
        Navigation.SelectionChanged += (_, _) => UpdateRoute();
        Navigation.SelectedItem = CastNavigationItem;
        UpdateRoute();

        _castPage.FullscreenRequested += ToggleFullscreen;
        _castPage.FrameRendered += OnFrameRendered;
        host.Changed += OnHostChanged;
        if (host.Current is { } initial)
            OnHostChanged(initial);

        Opened += (_, _) =>
        {
            _windowInteractionMonitor =
                WindowsWindowInteractionMonitor.TryCreate(
                    this, OnWindowInteractionChanged);
            UpdateRenderingActive();
        };
        Closed += (_, _) =>
        {
            _windowInteractionMonitor?.Dispose();
            _windowInteractionMonitor = null;
            _host.Changed -= OnHostChanged;
        };
    }

    protected override void OnPropertyChanged(
        AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == WindowStateProperty)
        {
            Navigation.IsPaneVisible =
                WindowState != WindowState.FullScreen;
            UpdateRenderingActive();
        }
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.Key == Key.Escape
            && WindowState == WindowState.FullScreen)
        {
            WindowState = WindowState.Normal;
            e.Handled = true;
            return;
        }
        base.OnKeyDown(e);
    }

    protected override async void OnClosing(WindowClosingEventArgs e)
    {
        if (_allowClose
            || _host.Current?.Playback.PeerConnected != true)
        {
            base.OnClosing(e);
            return;
        }

        e.Cancel = true;
        if (_closeDialogOpen)
            return;
        _closeDialogOpen = true;
        var dialog = new FAContentDialog
        {
            Title = "Close Media Cast Receiver",
            Content =
                "A cast is still connected. Pause and keep the session, "
                + "or end the cast before closing.",
            PrimaryButtonText = "Pause and close",
            SecondaryButtonText = "End cast and close",
            CloseButtonText = "Cancel",
            DefaultButton = FAContentDialogButton.Primary,
        };
        try
        {
            var result = await dialog.ShowAsync(this);
            if (result == FAContentDialogResult.None)
                return;

            if (result == FAContentDialogResult.Secondary)
            {
                _host.RequestEndCast();
                var deadline =
                    DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5);
                while (_host.Current?.Playback.PeerConnected == true
                       && DateTimeOffset.UtcNow < deadline)
                {
                    await Task.Delay(50);
                }
            }
            var flushed = await Task.Run(
                _host.PauseAndFlushForWindowClose);
            _log.Write(
                flushed
                    ? "Window close: pause and state flush completed"
                    : "Window close: pause and state flush timed out");
            _allowClose = true;
            Dispatcher.UIThread.Post(Close);
        }
        finally
        {
            _closeDialogOpen = false;
        }
    }

    internal void CloseForHostShutdown()
    {
        _allowClose = true;
        Close();
    }

    private void OnHostChanged(HostProjection projection)
    {
        _settingsPage.ApplyProjection(projection);
        _castPage.ApplyProjection(projection);
        if (projection.Renderer.ContextLost
            && WindowState == WindowState.FullScreen)
        {
            WindowState = WindowState.Normal;
        }
    }

    private void UpdateRoute()
    {
        var settings = ReferenceEquals(
            Navigation.SelectedItem, SettingsNavigationItem);
        _castPage.SetControlsVisible(!settings);
        _castPage.SetPageActive(!settings);
        _settingsPage.IsVisible = settings;
        _settingsPage.IsHitTestVisible = settings;
        _settingsPage.ZIndex = settings ? 1 : 0;
        _castPage.ZIndex = settings ? 0 : 1;
    }

    private void ToggleFullscreen()
    {
        WindowState = WindowState == WindowState.FullScreen
            ? WindowState.Normal
            : WindowState.FullScreen;
    }

    private void OnWindowInteractionChanged(bool interacting)
    {
        if (_windowInteractionActive == interacting)
            return;
        _windowInteractionActive = interacting;
        _log.Write(
            interacting
                ? "Window move/resize entered; rendering suspended"
                : "Window move/resize exited; rendering resumed");
        UpdateRenderingActive();
    }

    private void UpdateRenderingActive()
    {
        _castPage.SetRenderingActive(
            !_windowInteractionActive
            && WindowState != WindowState.Minimized);
    }

    private void OnFrameRendered()
    {
        if (_firstFrameSent)
            return;
        Dispatcher.UIThread.Post(
            () =>
            {
                if (_firstFrameSent || !IsVisible)
                    return;
                _firstFrameSent = true;
                _log.Write("Avalonia first frame presented");
                _host.NotifyFirstFrame();
            },
            DispatcherPriority.Render);
    }
}
