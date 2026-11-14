using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;

using MCast.Ui.Avalonia.Contracts;
using MCast.Ui.Avalonia.Native;
using MCast.Ui.Avalonia.Rendering;

namespace MCast.Ui.Avalonia.Views;

internal sealed partial class CastPage : UserControl
{
    private static readonly TimeSpan s_authorityEchoTimeout =
        TimeSpan.FromSeconds(2);

    private readonly NativeAvaloniaHost _host;
    private readonly MpvOpenGlSurface _surface;
    private readonly Border _interactionLayer = new()
    {
        Background = global::Avalonia.Media.Brushes.Transparent,
        Focusable = true,
    };
    private readonly MenuFlyout _mediaMenu = new();
    private readonly MenuItem _menuPrevious = new() { Header = "Previous" };
    private readonly MenuItem _menuPlayPause = new() { Header = "Play" };
    private readonly MenuItem _menuNext = new() { Header = "Next" };
    private readonly MenuItem _menuSeekBack =
        new() { Header = "Back 10 seconds" };
    private readonly MenuItem _menuSeekForward =
        new() { Header = "Forward 10 seconds" };
    private readonly MenuItem _menuVolumeUp =
        new() { Header = "Volume up" };
    private readonly MenuItem _menuVolumeDown =
        new() { Header = "Volume down" };
    private readonly MenuItem _menuMute = new() { Header = "Mute" };
    private readonly MenuItem _menuInfo = new() { Header = "Media info" };
    private readonly MenuItem _menuPicture =
        new() { Header = "Display effects" };
    private readonly MenuItem _menuFullscreen =
        new() { Header = "Full screen" };
    private PlaybackProjection? _playback;
    private HostProjection? _hostProjection;
    private bool _seekDragging;
    private bool _volumeDragging;
    private bool _applyingProjection;
    private bool _pageActive = true;
    private bool _controlsAllowed = true;
    private bool _playbackCommandsAvailable;
    private double? _seekPendingValue;
    private DateTimeOffset _seekPendingUntil;
    private string? _seekPendingMediaUri;
    private int? _volumePendingValue;
    private DateTimeOffset _volumePendingUntil;
    private DateTimeOffset _lastSurfaceClick;
    private Point _lastSurfaceClickPoint;
    private CancellationTokenSource? _pendingSingleClick;

    public event Action? FullscreenRequested;
    public event Action? FrameRendered;

    internal CastPage(NativeAvaloniaHost host)
    {
        InitializeComponent();
        _host = host;
        _surface = new MpvOpenGlSurface(host);
        _surface.FrameRendered += () => FrameRendered?.Invoke();
        VideoHost.Children.Add(_surface);
        VideoHost.Children.Add(_interactionLayer);
        IdlePanel.IsHitTestVisible = false;
        InitializeMediaMenu();
        _interactionLayer.KeyDown += OnSurfaceKeyDown;
        _interactionLayer.AddHandler(
            PointerPressedEvent,
            OnSurfacePointerPressed,
            RoutingStrategies.Tunnel | RoutingStrategies.Bubble,
            handledEventsToo: true);

        SeekSlider.AddHandler(
            PointerPressedEvent,
            OnSeekPressed,
            RoutingStrategies.Tunnel,
            handledEventsToo: true);
        SeekSlider.AddHandler(
            PointerReleasedEvent,
            OnSeekReleased,
            RoutingStrategies.Bubble,
            handledEventsToo: true);
        VolumeSlider.AddHandler(
            PointerPressedEvent,
            OnVolumePressed,
            RoutingStrategies.Tunnel,
            handledEventsToo: true);
        VolumeSlider.AddHandler(
            PointerReleasedEvent,
            OnVolumeReleased,
            RoutingStrategies.Bubble,
            handledEventsToo: true);
        SeekSlider.ValueChanged += OnSeekValueChanged;
        VolumeSlider.ValueChanged += OnVolumeValueChanged;
        SeekSlider.PointerCaptureLost += OnSeekCaptureLost;
        VolumeSlider.PointerCaptureLost += OnVolumeCaptureLost;
        SeekSlider.LostFocus += OnSeekLostFocus;
        VolumeSlider.LostFocus += OnVolumeLostFocus;
    }

    internal void ApplyProjection(HostProjection projection)
    {
        _hostProjection = projection;
        _playback = projection.Playback;
        _playbackCommandsAvailable =
            projection.Playback.PlayerAvailable
            && projection.Renderer.RendererReady
            && !projection.Renderer.ContextLost;
        _applyingProjection = true;
        try
        {
            var media = projection.Playback.CurrentMedia;
            TitleText.Text = string.IsNullOrWhiteSpace(media.Title)
                ? "Not casting" : media.Title;
            SubtitleText.Text = string.IsNullOrWhiteSpace(media.Subtitle)
                ? projection.Playback.CastSession.ControlPointName
                : media.Subtitle;
            IdleTitle.Text = projection.Playback.PeerConnected
                ? "Connected"
                : "Cast to this PC";
            IdleSubtitle.Text = projection.Playback.PeerConnected
                ? "Waiting for the next media"
                : "Waiting for a media cast";

            PreviousButton.IsEnabled =
                _playbackCommandsAvailable
                && projection.Playback.HasPrevious;
            NextButton.IsEnabled =
                _playbackCommandsAvailable
                && projection.Playback.HasNext;
            PlayPauseButton.IsEnabled =
                _playbackCommandsAvailable
                && projection.Playback.HasMedia;
            EndCastButton.IsEnabled = projection.Playback.PeerConnected;
            PlayPauseButton.Content = projection.Playback.Paused
                || projection.Playback.Idle
                ? "Play"
                : "Pause";
            MuteButton.Content = projection.Playback.Muted
                ? "Unmute"
                : "Mute";
            _menuPrevious.IsEnabled =
                _playbackCommandsAvailable
                && projection.Playback.HasPrevious;
            _menuNext.IsEnabled =
                _playbackCommandsAvailable
                && projection.Playback.HasNext;
            _menuPlayPause.IsEnabled =
                _playbackCommandsAvailable
                && projection.Playback.HasMedia;
            _menuPlayPause.Header = projection.Playback.Paused
                || projection.Playback.Idle
                ? "Play"
                : "Pause";
            _menuSeekBack.IsEnabled = _playbackCommandsAvailable
                && projection.Playback.HasMedia
                && projection.Playback.DurationSeconds > 0;
            _menuSeekForward.IsEnabled = _playbackCommandsAvailable
                && projection.Playback.HasMedia
                && projection.Playback.DurationSeconds > 0;
            _menuVolumeUp.IsEnabled = _playbackCommandsAvailable;
            _menuVolumeDown.IsEnabled = _playbackCommandsAvailable;
            _menuMute.IsEnabled = _playbackCommandsAvailable;
            _menuPicture.IsEnabled = _playbackCommandsAvailable;
            _menuMute.Header = projection.Playback.Muted
                ? "Unmute"
                : "Mute";
            _menuFullscreen.Header =
                TopLevel.GetTopLevel(this) is Window
                {
                    WindowState: WindowState.FullScreen,
                }
                    ? "Exit full screen"
                    : "Full screen";

            SeekSlider.Maximum = Math.Max(
                1, projection.Playback.DurationSeconds);
            ApplySeekProjection(projection.Playback);
            ApplyVolumeProjection(projection.Playback);
            SeekSlider.IsEnabled = _playbackCommandsAvailable;
            VolumeSlider.IsEnabled = _playbackCommandsAvailable;
            MuteButton.IsEnabled = _playbackCommandsAvailable;
            PictureButton.IsEnabled = _playbackCommandsAvailable;
            UpdateTimeText();
            UpdateVisibility();
        }
        finally
        {
            _applyingProjection = false;
        }
    }

    internal void SetControlsVisible(bool visible)
    {
        _controlsAllowed = visible;
        UpdateVisibility();
    }

    internal void SetPageActive(bool active)
    {
        _pageActive = active;
        UpdateVisibility();
    }

    internal void SetRenderingActive(bool active)
    {
        _surface.SetRenderingActive(active);
    }

    private void OnPlayPause(object? sender, RoutedEventArgs e)
        => _host.TogglePlayPause();

    private void OnPrevious(object? sender, RoutedEventArgs e)
        => _host.Previous();

    private void OnNext(object? sender, RoutedEventArgs e)
        => _host.Next();

    private void OnMute(object? sender, RoutedEventArgs e)
        => _host.SetMuted(_playback?.Muted != true);

    private void OnEndCast(object? sender, RoutedEventArgs e)
        => _host.RequestEndCast();

    private void OnFullscreen(object? sender, RoutedEventArgs e)
        => FullscreenRequested?.Invoke();

    private void OnPicture(object? sender, RoutedEventArgs e)
        => OpenPictureControls();

    private void OnInfo(object? sender, RoutedEventArgs e)
        => OpenInfo();

    private void InitializeMediaMenu()
    {
        _mediaMenu.Opened += (_, _) =>
        {
            Dispatcher.UIThread.Post(
                () =>
                {
                    foreach (var item in _mediaMenu.Items)
                    {
                        if (item is MenuItem { IsEnabled: true } menuItem)
                        {
                            menuItem.Focus();
                            break;
                        }
                    }
                },
                DispatcherPriority.Input);
        };
        _mediaMenu.Closed += (_, _) =>
            Dispatcher.UIThread.Post(
                () => _interactionLayer.Focus(),
                DispatcherPriority.Input);
        _menuPrevious.Click += (_, _) => _host.Previous();
        _menuPlayPause.Click += (_, _) => _host.TogglePlayPause();
        _menuNext.Click += (_, _) => _host.Next();
        _menuSeekBack.Click += (_, _) =>
        {
            if (_playback is not null)
                _host.SeekAbsolute(
                    Math.Max(0, _playback.PositionSeconds - 10));
        };
        _menuSeekForward.Click += (_, _) =>
        {
            if (_playback is not null)
                _host.SeekAbsolute(
                    Math.Min(
                        _playback.DurationSeconds,
                        _playback.PositionSeconds + 10));
        };
        _menuVolumeUp.Click += (_, _) =>
        {
            if (_playback is not null)
                _host.SetVolume(
                    Math.Min(100, _playback.VolumePercent + 5));
        };
        _menuVolumeDown.Click += (_, _) =>
        {
            if (_playback is not null)
                _host.SetVolume(
                    Math.Max(0, _playback.VolumePercent - 5));
        };
        _menuMute.Click += (_, _) =>
            _host.SetMuted(_playback?.Muted != true);
        _menuInfo.Click += (_, _) =>
            Dispatcher.UIThread.Post(OpenInfo);
        _menuPicture.Click += (_, _) =>
            Dispatcher.UIThread.Post(OpenPictureControls);
        _menuFullscreen.Click += (_, _) =>
            FullscreenRequested?.Invoke();

        _mediaMenu.Items.Add(_menuPrevious);
        _mediaMenu.Items.Add(_menuPlayPause);
        _mediaMenu.Items.Add(_menuNext);
        _mediaMenu.Items.Add(new Separator());
        _mediaMenu.Items.Add(_menuSeekBack);
        _mediaMenu.Items.Add(_menuSeekForward);
        _mediaMenu.Items.Add(new Separator());
        _mediaMenu.Items.Add(_menuVolumeUp);
        _mediaMenu.Items.Add(_menuVolumeDown);
        _mediaMenu.Items.Add(_menuMute);
        _mediaMenu.Items.Add(_menuInfo);
        _mediaMenu.Items.Add(_menuPicture);
        _mediaMenu.Items.Add(_menuFullscreen);
    }

    private async void OnSurfacePointerPressed(
        object? sender, PointerPressedEventArgs e)
    {
        var point = e.GetCurrentPoint(_interactionLayer);
        if (point.Properties.PointerUpdateKind
            == PointerUpdateKind.RightButtonPressed)
        {
            OpenMediaMenu();
            e.Handled = true;
            return;
        }
        if (point.Properties.PointerUpdateKind
            != PointerUpdateKind.LeftButtonPressed)
            return;
        _interactionLayer.Focus();
        var now = DateTimeOffset.UtcNow;
        var elapsed = now - _lastSurfaceClick;
        var close = Math.Abs(point.Position.X - _lastSurfaceClickPoint.X)
                < 12
            && Math.Abs(point.Position.Y - _lastSurfaceClickPoint.Y) < 12;
        if (elapsed < TimeSpan.FromMilliseconds(400) && close)
        {
            _pendingSingleClick?.Cancel();
            _pendingSingleClick?.Dispose();
            _pendingSingleClick = null;
            _lastSurfaceClick = default;
            FullscreenRequested?.Invoke();
            return;
        }

        _lastSurfaceClick = now;
        _lastSurfaceClickPoint = point.Position;
        _pendingSingleClick?.Cancel();
        _pendingSingleClick?.Dispose();
        var cancellation = new CancellationTokenSource();
        _pendingSingleClick = cancellation;
        try
        {
            await Task.Delay(250, cancellation.Token);
            Dispatcher.UIThread.Post(() =>
            {
                if (!cancellation.IsCancellationRequested
                    && _playbackCommandsAvailable)
                    _host.TogglePlayPause();
            });
        }
        catch (OperationCanceledException)
        {
        }
        finally
        {
            if (ReferenceEquals(_pendingSingleClick, cancellation))
            {
                _pendingSingleClick.Dispose();
                _pendingSingleClick = null;
            }
        }
    }

    private void OnSurfaceKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Apps
            || (e.Key == Key.F10
                && e.KeyModifiers.HasFlag(KeyModifiers.Shift)))
        {
            OpenMediaMenu();
            e.Handled = true;
        }
    }

    private void OpenMediaMenu()
    {
        _interactionLayer.Focus();
        _mediaMenu.ShowAt(_interactionLayer);
    }

    private void OpenPictureControls()
    {
        if (_hostProjection is null)
            return;
        var panel = new StackPanel
        {
            Spacing = 8,
            MinWidth = 280,
        };
        panel.Children.Add(new TextBlock
        {
            Text = "Display effects",
            FontSize = 18,
            FontWeight = global::Avalonia.Media.FontWeight.SemiBold,
        });
        foreach (var control in _hostProjection.Playback.PictureControls)
        {
            var label = new TextBlock
            {
                Text = PictureControlLabel(control.Name),
            };
            var slider = new Slider
            {
                Minimum = control.Minimum,
                Maximum = control.Maximum,
                Value = Math.Clamp(
                    control.Value, control.Minimum, control.Maximum),
            };
            var dragging = false;
            var lastSentValue =
                (int)Math.Round(slider.Value);
            var pendingValue = lastSentValue;
            // Pointer input can outrun video frames. Keep only the newest
            // value between display ticks, then force the release value.
            var flushTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(16),
            };
            void PushLatest(bool force)
            {
                if (!force && pendingValue == lastSentValue)
                    return;
                lastSentValue = pendingValue;
                _host.SetPictureControl(
                    control.Name, pendingValue);
            }
            void QueueLatest()
            {
                pendingValue = (int)Math.Round(slider.Value);
                if (flushTimer.IsEnabled)
                    return;
                PushLatest(force: false);
                flushTimer.Start();
            }
            flushTimer.Tick += (_, _) =>
                PushLatest(force: false);
            void Finish()
            {
                if (!dragging)
                    return;
                dragging = false;
                flushTimer.Stop();
                pendingValue = (int)Math.Round(slider.Value);
                PushLatest(force: true);
                _host.EndControlAdjustment();
            }

            slider.AddHandler(
                PointerPressedEvent,
                (_, args) =>
                {
                    if (dragging)
                        return;
                    dragging = true;
                    _host.BeginControlAdjustment();
                },
                RoutingStrategies.Tunnel,
                handledEventsToo: true);
            slider.AddHandler(
                PointerReleasedEvent,
                (_, args) =>
                {
                    Finish();
                },
                RoutingStrategies.Bubble,
                handledEventsToo: true);
            slider.ValueChanged += (_, _) =>
            {
                if (dragging)
                    QueueLatest();
            };
            slider.PointerCaptureLost += (_, _) => Finish();
            slider.LostFocus += (_, _) => Finish();
            panel.Children.Add(label);
            panel.Children.Add(slider);
        }
        var reset = new Button { Content = "Reset all" };
        reset.Click += (_, _) => _host.ResetPictureControls();
        panel.Children.Add(reset);
        var scroller = new ScrollViewer
        {
            Content = panel,
            MaxHeight = 520,
            VerticalScrollBarVisibility =
                global::Avalonia.Controls.Primitives.ScrollBarVisibility.Auto,
        };
        new Flyout { Content = scroller }.ShowAt(_interactionLayer);
    }

    private void OpenInfo()
    {
        if (_hostProjection is null)
            return;
        var playback = _hostProjection.Playback;
        var panel = new StackPanel
        {
            Spacing = 6,
            MinWidth = 360,
            MaxWidth = 520,
        };
        panel.Children.Add(new TextBlock
        {
            Text = "Media info",
            FontSize = 18,
            FontWeight = global::Avalonia.Media.FontWeight.SemiBold,
        });
        AddInfoLine(
            panel,
            "Protocol",
            playback.CastSession.Connected
                ? playback.CastSession.Protocol
                : "Not casting");
        AddInfoLine(
            panel,
            "Control point",
            playback.CastSession.ControlPointName);
        AddInfoLine(
            panel,
            "Address",
            playback.CastSession.ControlPointAddress);
        AddInfoLine(
            panel,
            "Media",
            playback.CurrentMedia.Title);
        AddInfoLine(
            panel,
            "Detail",
            playback.CurrentMedia.Subtitle);
        AddInfoLine(
            panel,
            "Container",
            playback.MediaInfo.Container);
        AddInfoLine(
            panel,
            "Duration",
            FormatTime(playback.MediaInfo.DurationSeconds));
        if (playback.MediaInfo.Video is { } video)
        {
            AddInfoLine(
                panel,
                "Video",
                $"{video.Codec} {video.Width}×{video.Height} "
                + $"{video.Fps:0.###} fps");
        }
        if (playback.MediaInfo.Audio is { } audio)
        {
            AddInfoLine(
                panel,
                "Audio",
                $"{audio.Codec} {audio.SampleRate} Hz "
                + $"{audio.ChannelLayout}");
        }
        new Flyout { Content = panel }.ShowAt(_interactionLayer);
    }

    private static void AddInfoLine(
        StackPanel panel, string label, string value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return;
        panel.Children.Add(new TextBlock
        {
            Text = $"{label}: {value}",
            TextWrapping = global::Avalonia.Media.TextWrapping.Wrap,
        });
    }

    private static string PictureControlLabel(string name) => name switch
    {
        "brightness" => "Brightness",
        "contrast" => "Contrast",
        "saturation" => "Saturation",
        "gamma" => "Gamma",
        "hue" => "Hue",
        "sharpen" => "Sharpness",
        "zoom" => "Zoom",
        "pan-x" => "Pan X",
        "pan-y" => "Pan Y",
        "rotate" => "Rotate",
        _ => name,
    };

    private void OnSeekPressed(object? sender, PointerPressedEventArgs e)
    {
        _seekDragging = true;
    }

    private void OnSeekReleased(object? sender, PointerReleasedEventArgs e)
    {
        if (!_seekDragging)
            return;
        _seekDragging = false;
        CommitSeek(SeekSlider.Value);
    }

    private void OnSeekValueChanged(
        object? sender, RangeBaseValueChangedEventArgs e)
    {
        if (_applyingProjection)
            return;
        if (_seekDragging) {
            UpdateTimeText(preview: e.NewValue);
            return;
        }
        UpdateTimeText(preview: e.NewValue);
        CommitSeek(e.NewValue);
    }

    private void OnSeekCaptureLost(object? sender, RoutedEventArgs e)
    {
        if (!_seekDragging)
            return;
        _seekDragging = false;
        _seekPendingValue = null;
        _applyingProjection = true;
        try
        {
            if (_playback is not null)
                SeekSlider.Value = Math.Clamp(
                    _playback.PositionSeconds, 0, SeekSlider.Maximum);
        }
        finally
        {
            _applyingProjection = false;
        }
        UpdateTimeText();
    }

    private void OnSeekLostFocus(object? sender, RoutedEventArgs e)
    {
        if (!_seekDragging)
            return;
        _seekDragging = false;
        _seekPendingValue = null;
        _applyingProjection = true;
        try
        {
            if (_playback is not null)
                SeekSlider.Value = Math.Clamp(
                    _playback.PositionSeconds, 0, SeekSlider.Maximum);
        }
        finally
        {
            _applyingProjection = false;
        }
    }

    private void OnVolumePressed(object? sender, PointerPressedEventArgs e)
    {
        if (_volumeDragging)
            return;
        _volumeDragging = true;
        _volumePendingValue =
            (int)Math.Round(VolumeSlider.Value);
        _host.BeginControlAdjustment();
    }

    private void OnVolumeReleased(object? sender, PointerReleasedEventArgs e)
    {
        if (!_volumeDragging)
            return;
        FinishVolumeAdjustment();
    }

    private void OnVolumeValueChanged(
        object? sender, RangeBaseValueChangedEventArgs e)
    {
        if (_applyingProjection)
            return;
        if (_volumeDragging)
        {
            var value = (int)Math.Round(e.NewValue);
            _volumePendingValue = value;
            _host.SetVolume(value);
            return;
        }
        var committed = (int)Math.Round(e.NewValue);
        _volumePendingValue = committed;
        _volumePendingUntil =
            DateTimeOffset.UtcNow + s_authorityEchoTimeout;
        _host.BeginControlAdjustment();
        _host.SetVolume(committed);
        _host.EndControlAdjustment();
    }

    private void OnVolumeCaptureLost(object? sender, RoutedEventArgs e)
    {
        if (_volumeDragging)
            FinishVolumeAdjustment();
    }

    private void OnVolumeLostFocus(object? sender, RoutedEventArgs e)
    {
        if (_volumeDragging)
            FinishVolumeAdjustment();
    }

    private void FinishVolumeAdjustment()
    {
        _volumeDragging = false;
        var value = (int)Math.Round(VolumeSlider.Value);
        _volumePendingValue = value;
        _volumePendingUntil =
            DateTimeOffset.UtcNow + s_authorityEchoTimeout;
        _host.SetVolume(value);
        _host.EndControlAdjustment();
    }

    private void CommitSeek(double value)
    {
        _seekPendingValue = value;
        _seekPendingUntil =
            DateTimeOffset.UtcNow + s_authorityEchoTimeout;
        _seekPendingMediaUri = _playback?.CurrentMedia.Uri;
        _host.SeekAbsolute(value);
    }

    private void ApplySeekProjection(PlaybackProjection playback)
    {
        if (_seekDragging)
            return;
        if (_seekPendingValue is double pending)
        {
            var sameMedia = string.Equals(
                _seekPendingMediaUri,
                playback.CurrentMedia.Uri,
                StringComparison.Ordinal);
            var echoed = Math.Abs(
                playback.PositionSeconds - pending) <= 0.75;
            if (sameMedia
                && DateTimeOffset.UtcNow < _seekPendingUntil
                && !echoed)
            {
                SeekSlider.Value = Math.Clamp(
                    pending, 0, SeekSlider.Maximum);
                return;
            }
            _seekPendingValue = null;
            _seekPendingMediaUri = null;
        }
        SeekSlider.Value = Math.Clamp(
            playback.PositionSeconds,
            0,
            SeekSlider.Maximum);
    }

    private void ApplyVolumeProjection(PlaybackProjection playback)
    {
        if (_volumeDragging)
            return;
        if (_volumePendingValue is int pending)
        {
            if (playback.VolumePercent != pending
                && DateTimeOffset.UtcNow < _volumePendingUntil)
            {
                VolumeSlider.Value = pending;
                return;
            }
            _volumePendingValue = null;
        }
        VolumeSlider.Value = playback.VolumePercent;
    }

    private void UpdateTimeText(double? preview = null)
    {
        var position = preview ?? SeekSlider.Value;
        var duration = _playback?.DurationSeconds ?? 0;
        TimeText.Text =
            $"{FormatTime(position)} / {FormatTime(duration)}";
    }

    private void UpdateVisibility()
    {
        IsHitTestVisible = _pageActive;
        ControlsPanel.IsVisible = _pageActive && _controlsAllowed;
        ControlsPanel.IsHitTestVisible = _pageActive && _controlsAllowed;
        IdlePanel.IsVisible = _pageActive
            && (_playback is null
                || !_playback.HasMedia
                || _playback.Idle);
    }

    private static string FormatTime(double seconds)
    {
        if (!(seconds > 0))
            return "00:00";
        var total = (long)Math.Round(seconds);
        var hours = total / 3600;
        var minutes = total % 3600 / 60;
        var secs = total % 60;
        return hours > 0
            ? $"{hours}:{minutes:00}:{secs:00}"
            : $"{minutes:00}:{secs:00}";
    }
}
