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
    private bool? _lastControlConnected;
    private bool? _lastRendererLost;
    private bool? _lastRendererReady;
    private string? _lastFatalReason;
    private ulong _lastRenderGeneration = ulong.MaxValue;
    private int _lastFramebuffer = int.MinValue;
    private int _lastRenderWidth = -1;
    private int _lastRenderHeight = -1;

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
            if (_host.Current?.Confirmation is { } request)
                ShowConfirmation(request, ownsProcessLifecycle: false);
        }
        base.OnFrameworkInitializationCompleted();
    }

    private void ConfigureConfirmationOnly(
        IClassicDesktopStyleApplicationLifetime desktop)
    {
        desktop.ShutdownMode = ShutdownMode.OnExplicitShutdown;
        if (_host?.Current?.Confirmation is { } request)
            ShowConfirmation(request, ownsProcessLifecycle: true);
        else if (_host?.Current?.Shell.TestTakeoverWindow == true)
        {
            ShowConfirmation(new CastConfirmationProjection(
                9001,
                1,
                1,
                "standalone-peer",
                "Living Room Phone",
                "192.0.2.10",
                "Example Movie",
                10_000), ownsProcessLifecycle: true);
        }
    }

    private void OnHostChanged(HostProjection projection)
    {
        LogHostTransitions(projection);
        ApplyShellAppearance(projection.Shell);
        if (!projection.Shell.UiSessionConnected
            || projection.Renderer.ContextLost
            || !string.IsNullOrEmpty(projection.Shell.FatalReason))
        {
            RequestShutdown();
            return;
        }

        if (projection.Confirmation is { } request)
        {
            ShowConfirmation(
                request,
                ownsProcessLifecycle: projection.Shell.ConfirmationOnly);
        }
        else
        {
            _confirmationWindow?.CancelFromShell();
        }
    }

    private void LogHostTransitions(HostProjection projection)
    {
        var shell = projection.Shell;
        var renderer = projection.Renderer;
        if (_lastControlConnected != shell.UiSessionConnected
            || _lastRendererLost != renderer.ContextLost
            || _lastFatalReason != shell.FatalReason)
        {
            _lastControlConnected = shell.UiSessionConnected;
            _lastRendererLost = renderer.ContextLost;
            _lastFatalReason = shell.FatalReason;
            _log?.Write(
                "Host state "
                + $"controlConnected={shell.UiSessionConnected} "
                + $"rendererLost={renderer.ContextLost} "
                + $"fatal={shell.FatalReason}");
        }

        if (_lastRendererReady != renderer.RendererReady)
        {
            _lastRendererReady = renderer.RendererReady;
            _log?.Write(
                $"Renderer ready={renderer.RendererReady} "
                + $"generation={renderer.RenderContextGeneration}");
        }

        if (_lastRenderGeneration != renderer.RenderContextGeneration
            || _lastFramebuffer != renderer.LastFramebuffer
            || _lastRenderWidth != renderer.LastWidth
            || _lastRenderHeight != renderer.LastHeight)
        {
            _lastRenderGeneration = renderer.RenderContextGeneration;
            _lastFramebuffer = renderer.LastFramebuffer;
            _lastRenderWidth = renderer.LastWidth;
            _lastRenderHeight = renderer.LastHeight;
            _log?.Write(
                "Render target "
                + $"generation={renderer.RenderContextGeneration} "
                + $"framebuffer={renderer.LastFramebuffer} "
                + $"pixels={renderer.LastWidth}x{renderer.LastHeight}");
        }
    }

    private void ShowConfirmation(
        CastConfirmationProjection request,
        bool ownsProcessLifecycle)
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
        if (ownsProcessLifecycle && _desktop is not null)
            _desktop.MainWindow = window;
        window.Closed += (_, _) =>
        {
            if (ReferenceEquals(_confirmationWindow, window))
                _confirmationWindow = null;
            if (ownsProcessLifecycle && !_shutdownRequested)
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

