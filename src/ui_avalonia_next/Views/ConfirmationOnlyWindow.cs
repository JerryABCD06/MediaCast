using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

using MCast.Ui.Avalonia.Contracts;
using MCast.Ui.Avalonia.Native;

namespace MCast.Ui.Avalonia.Views;

internal sealed class ConfirmationOnlyWindow : Window
{
    private readonly NativeAvaloniaHost _host;
    private readonly Button _accept;
    private readonly Button _reject;
    private readonly TextBlock _countdown;
    private readonly DispatcherTimer _timer;
    private DateTimeOffset _deadline;
    private bool _closedFromShell;
    private bool _submitted;

    public CastConfirmationProjection Projection { get; }

    public ConfirmationOnlyWindow(
        NativeAvaloniaHost host,
        CastConfirmationProjection projection)
    {
        _host = host;
        Projection = projection;
        Title = "Cast confirmation";
        Width = 460;
        Height = 270;
        CanResize = false;
        WindowStartupLocation =
            WindowStartupLocation.CenterScreen;

        _accept = new Button
        {
            Content = "Allow",
            MinWidth = 110,
        };
        _reject = new Button
        {
            Content = "Keep current cast",
            MinWidth = 150,
        };
        _accept.Click += (_, _) => Submit(true);
        _reject.Click += (_, _) => Submit(false);

        _countdown = new TextBlock
        {
            Opacity = 0.72,
        };
        _deadline = DateTimeOffset.UtcNow.AddMilliseconds(
            projection.RemainingTimeoutMs);
        _timer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(250),
        };
        _timer.Tick += (_, _) => UpdateCountdown();

        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Spacing = 8,
            Children =
            {
                _reject,
                _accept,
            },
        };
        Grid.SetRow(buttons, 3);
        var body = CreateBody(projection, _countdown, row: 1);
        var layout = new Grid
        {
            RowDefinitions = new RowDefinitions("Auto,Auto,*,Auto"),
            Children =
            {
                new TextBlock
                {
                    Text = "Incoming cast",
                    FontSize = 24,
                    FontWeight = FontWeight.SemiBold,
                },
                body,
                buttons,
            },
        };
        Content = new Border
        {
            Padding = new Thickness(28),
            Child = layout,
        };
        Opened += (_, _) =>
        {
            _timer.Start();
            UpdateCountdown();
            _accept.Focus();
        };
        Closed += (_, _) => _timer.Stop();
    }

    public void CancelFromShell()
    {
        if (_closedFromShell)
            return;
        _closedFromShell = true;
        Close();
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.Key == Key.Escape)
        {
            Submit(false);
            e.Handled = true;
            return;
        }
        base.OnKeyDown(e);
    }

    protected override void OnClosing(WindowClosingEventArgs e)
    {
        if (!_submitted && !_closedFromShell)
        {
            _submitted = true;
            _host.RespondCastConfirmation(Projection.RequestId, false);
        }
        base.OnClosing(e);
    }

    private void Submit(bool accepted)
    {
        if (_submitted)
            return;
        _submitted = true;
        _host.RespondCastConfirmation(Projection.RequestId, accepted);
        Close();
    }

    private void UpdateCountdown()
    {
        var remaining = _deadline - DateTimeOffset.UtcNow;
        if (remaining <= TimeSpan.Zero)
        {
            _countdown.Text = "Request expired";
            _accept.IsEnabled = false;
            _reject.IsEnabled = false;
            Submit(false);
            return;
        }
        _countdown.Text =
            $"Respond within {(int)Math.Ceiling(remaining.TotalSeconds)} s";
    }

    private static Control CreateBody(
        CastConfirmationProjection projection,
        TextBlock countdown,
        int row)
    {
        var panel = new StackPanel
        {
            Spacing = 8,
            Margin = new Thickness(0, 18, 0, 18),
            Children =
            {
                new TextBlock
                {
                    Text = string.IsNullOrWhiteSpace(
                        projection.PeerName)
                        ? "An unknown device"
                        : projection.PeerName,
                    FontSize = 18,
                    FontWeight = FontWeight.SemiBold,
                },
                new TextBlock
                {
                    Text = string.IsNullOrWhiteSpace(
                        projection.MediaTitle)
                        ? "wants to cast media to this PC"
                        : $"wants to cast \"{projection.MediaTitle}\"",
                    TextWrapping = TextWrapping.Wrap,
                },
                countdown,
            },
        };
        Grid.SetRow(panel, row);
        return panel;
    }
}
