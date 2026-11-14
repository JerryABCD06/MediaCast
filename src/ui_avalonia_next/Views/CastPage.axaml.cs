using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;

using MCast.Ui.Avalonia.Contracts;
using MCast.Ui.Avalonia.Native;
using MCast.Ui.Avalonia.Rendering;

namespace MCast.Ui.Avalonia.Views;

internal sealed partial class CastPage : UserControl
{
    private readonly NativeAvaloniaHost _host;
    private readonly MpvOpenGlSurface _surface;
    private PlaybackProjection? _playback;
    private bool _seekDragging;
    private bool _volumeDragging;
    private bool _applyingProjection;
    private bool _pageActive = true;
    private bool _controlsAllowed = true;

    public event Action? FullscreenRequested;
    public event Action? FrameRendered;

    internal CastPage(NativeAvaloniaHost host)
    {
        InitializeComponent();
        _host = host;
        _surface = new MpvOpenGlSurface(host);
        _surface.FrameRendered += () => FrameRendered?.Invoke();
        VideoHost.Children.Add(_surface);

        SeekSlider.AddHandler(
            PointerPressedEvent,
            OnSeekPressed,
            RoutingStrategies.Tunnel | RoutingStrategies.Bubble,
            handledEventsToo: true);
        SeekSlider.AddHandler(
            PointerReleasedEvent,
            OnSeekReleased,
            RoutingStrategies.Tunnel | RoutingStrategies.Bubble,
            handledEventsToo: true);
        VolumeSlider.AddHandler(
            PointerPressedEvent,
            OnVolumePressed,
            RoutingStrategies.Tunnel | RoutingStrategies.Bubble,
            handledEventsToo: true);
        VolumeSlider.AddHandler(
            PointerReleasedEvent,
            OnVolumeReleased,
            RoutingStrategies.Tunnel | RoutingStrategies.Bubble,
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
        _playback = projection.Playback;
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

            PreviousButton.IsEnabled = projection.Playback.HasPrevious;
            NextButton.IsEnabled = projection.Playback.HasNext;
            PlayPauseButton.IsEnabled = projection.Playback.HasMedia;
            EndCastButton.IsEnabled = projection.Playback.PeerConnected;
            PlayPauseButton.Content = projection.Playback.Paused
                || projection.Playback.Idle
                ? "Play"
                : "Pause";
            MuteButton.Content = projection.Playback.Muted
                ? "Unmute"
                : "Mute";

            SeekSlider.Maximum = Math.Max(
                1, projection.Playback.DurationSeconds);
            if (!_seekDragging)
                SeekSlider.Value = Math.Clamp(
                    projection.Playback.PositionSeconds,
                    0,
                    SeekSlider.Maximum);
            if (!_volumeDragging)
                VolumeSlider.Value = projection.Playback.VolumePercent;
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

    private void OnSeekPressed(object? sender, PointerPressedEventArgs e)
    {
        _seekDragging = true;
        e.Pointer.Capture(SeekSlider);
    }

    private void OnSeekReleased(object? sender, PointerReleasedEventArgs e)
    {
        if (!_seekDragging)
            return;
        _seekDragging = false;
        _host.SeekAbsolute(SeekSlider.Value);
        e.Pointer.Capture(null);
    }

    private void OnSeekValueChanged(
        object? sender, RangeBaseValueChangedEventArgs e)
    {
        if (_seekDragging)
            UpdateTimeText(preview: e.NewValue);
    }

    private void OnSeekCaptureLost(object? sender, RoutedEventArgs e)
    {
        if (!_seekDragging)
            return;
        _seekDragging = false;
        if (_playback is not null)
            SeekSlider.Value = Math.Clamp(
                _playback.PositionSeconds, 0, SeekSlider.Maximum);
        UpdateTimeText();
    }

    private void OnSeekLostFocus(object? sender, RoutedEventArgs e)
    {
        if (!_seekDragging)
            return;
        _seekDragging = false;
        if (_playback is not null)
            SeekSlider.Value = Math.Clamp(
                _playback.PositionSeconds, 0, SeekSlider.Maximum);
    }

    private void OnVolumePressed(object? sender, PointerPressedEventArgs e)
    {
        if (_volumeDragging)
            return;
        _volumeDragging = true;
        _host.BeginControlAdjustment();
        e.Pointer.Capture(VolumeSlider);
    }

    private void OnVolumeReleased(object? sender, PointerReleasedEventArgs e)
    {
        if (!_volumeDragging)
            return;
        FinishVolumeAdjustment();
        e.Pointer.Capture(null);
    }

    private void OnVolumeValueChanged(
        object? sender, RangeBaseValueChangedEventArgs e)
    {
        if (!_applyingProjection && _volumeDragging)
            _host.SetVolume((int)Math.Round(e.NewValue));
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
        _host.SetVolume((int)Math.Round(VolumeSlider.Value));
        _host.EndControlAdjustment();
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
