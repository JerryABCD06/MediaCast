using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Styling;
using Avalonia.Threading;

using FluentAvalonia.Styling;

using MCast.Ui.Avalonia.Contracts;
using MCast.Ui.Avalonia.Diagnostics;
using MCast.Ui.Avalonia.Native;
using MCast.Ui.Avalonia.Views;

namespace MCast.Ui.Avalonia;

public sealed class App : Application
{
    private NativeAvaloniaHost? _host;
    private UiDiagnosticLog? _log;
    private FluentAvaloniaTheme? _theme;
    private IClassicDesktopStyleApplicationLifetime? _desktop;
    private MainWindow? _mainWindow;
    private ConfirmationOnlyWindow? _confirmationWindow;
    private bool _shutdownRequested;

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
        _theme = Styles.OfType<FluentAvaloniaTheme>().FirstOrDefault();
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime
            is not IClassicDesktopStyleApplicationLifetime desktop)
        {
            base.OnFrameworkInitializationCompleted();
            return;
        }

        _desktop = desktop;
        _log = new UiDiagnosticLog(
            Path.Combine(
                AppContext.BaseDirectory,
                "MCast.Ui.Avalonia.log"));
        var arguments = Environment.GetCommandLineArgs().Skip(1).ToArray();
        _log.Write(
            "Managed arguments: "
                + string.Join(" | ", arguments));
        _host = NativeAvaloniaHost.TryCreate(arguments, out var error);
        if (_host is null)
        {
            _log.Write(error ?? "Native host creation failed");
            throw new InvalidOperationException(
                error ?? "Native host creation failed");
        }
        foreach (var startupLog in _host.TakeStartupLogs())
            _log.Write($"Native startup  {startupLog}");

        _host.LogReceived += message =>
        {
            Console.WriteLine($"[Native] {message}");
            _log.Write($"Native  {message}");
        };
        _host.Changed += OnHostChanged;
        ApplyShellAppearance(_host.Current?.Shell);

        desktop.Exit += (_, _) =>
        {
            _log.Write("Avalonia UI exiting");
            _host?.Dispose();
            _host = null;
            _mainWindow = null;
            _confirmationWindow = null;
            _desktop = null;
            _log?.Dispose();
            _log = null;
        };

        if (_host.Current?.Shell.ConfirmationOnly == true)
        {
            ConfigureConfirmationOnly(desktop);
        }
        else
        {
            var window = new MainWindow(_host, _log);
            _mainWindow = window;
            desktop.MainWindow = window;
        }
        base.OnFrameworkInitializationCompleted();
    }

    private void ConfigureConfirmationOnly(
        IClassicDesktopStyleApplicationLifetime desktop)
    {
        desktop.ShutdownMode = ShutdownMode.OnExplicitShutdown;
        if (_host?.Current?.Confirmation is { } request)
            ShowConfirmation(request);
        else
            Dispatcher.UIThread.Post(RequestShutdown);
    }

    private void OnHostChanged(HostProjection projection)
    {
        ApplyShellAppearance(projection.Shell);
        if (projection.Shell.ConfirmationOnly)
        {
            if (projection.Confirmation is { } request)
                ShowConfirmation(request);
            else if (!_shutdownRequested)
                RequestShutdown();
            return;
        }

        if (!projection.Shell.UiSessionConnected
            || projection.Renderer.ContextLost
            || !string.IsNullOrEmpty(projection.Shell.FatalReason))
        {
            RequestShutdown();
        }
    }

    private void ShowConfirmation(CastConfirmationProjection request)
    {
        if (_host is null)
            return;
        if (_confirmationWindow?.Projection.RequestId
            == request.RequestId)
        {
            return;
        }
        _confirmationWindow?.CancelFromShell();
        var window = new ConfirmationOnlyWindow(_host, request);
        _confirmationWindow = window;
        if (_desktop is not null)
            _desktop.MainWindow = window;
        window.Closed += (_, _) =>
        {
            if (ReferenceEquals(_confirmationWindow, window))
                _confirmationWindow = null;
            if (!_shutdownRequested)
                RequestShutdown();
        };
        window.Show();
    }

    private void ApplyShellAppearance(ShellProjection? shell)
    {
        if (shell is null)
            return;
        RequestedThemeVariant = shell.ThemeMode switch
        {
            ThemeMode.Light => ThemeVariant.Light,
            ThemeMode.Dark => ThemeVariant.Dark,
            _ => ThemeVariant.Default,
        };
        if (_theme is null)
            return;
        _theme.PreferSystemTheme = shell.ThemeMode == ThemeMode.System;
        _theme.PreferUserAccentColor =
            shell.AccentMode == AccentMode.System;
        _theme.CustomAccentColor = null;
    }

    private void RequestShutdown()
    {
        if (_shutdownRequested)
            return;
        _shutdownRequested = true;
        _log?.Write("Host/session failure; failing closed");
        Dispatcher.UIThread.Post(() =>
        {
            _mainWindow?.CloseForHostShutdown();
            _confirmationWindow?.CancelFromShell();
            _desktop?.Shutdown();
        });
    }
}

