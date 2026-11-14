using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

using Avalonia.Threading;

using MCast.Ui.Avalonia.Contracts;

namespace MCast.Ui.Avalonia.Native;

internal sealed class NativeAvaloniaHost :
    IProjectionSource,
    IPlaybackIntentPort,
    IShellSettingsIntentPort,
    IWindowLifecyclePort,
    ICastConfirmationPort,
    IDisposable
{
    private const string LibraryName = "MCast.AvaloniaHost.Native";

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void LogCallback(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string message,
        IntPtr context);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void ProjectionUpdateCallback(
        uint dirtyDomains, IntPtr context);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void RenderUpdateCallback(IntPtr context);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr GetProcAddressCallback(
        IntPtr context,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    private delegate int ReadJsonCallback(
        IntPtr buffer, int capacity, out int required);

    private static readonly LogCallback s_log = ReceiveLog;
    private static readonly ProjectionUpdateCallback s_projection =
        ReceiveProjectionUpdate;
    private static readonly RenderUpdateCallback s_render =
        ReceiveRenderUpdate;
    private static readonly JsonSerializerOptions s_jsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    private readonly GCHandle _selfHandle;
    private readonly object _refreshGate = new();
    private readonly object _startupLogGate = new();
    private readonly List<string> _startupLogs = [];
    private bool _captureStartupLogs = true;
    private string _lastShellJson = "{}";
    private int _refreshQueued;
    private int _renderRefreshQueued;
    private long _renderRevision;
    private bool _disposed;

    public HostProjection? Current { get; private set; }
    public event Action<HostProjection>? Changed;
    public event Action<string>? LogReceived;
    internal event Action? RenderUpdateRequested;

    private NativeAvaloniaHost()
    {
        _selfHandle = GCHandle.Alloc(this, GCHandleType.Normal);
    }

    public static NativeAvaloniaHost? TryCreate(
        IReadOnlyList<string> arguments, out string? error)
    {
        error = null;
        var host = new NativeAvaloniaHost();
        host.LogReceived += host.CaptureStartupLog;
        var argumentPointers = new List<IntPtr>();
        IntPtr argumentArray = IntPtr.Zero;
        try
        {
            foreach (var argument in arguments)
            {
                argumentPointers.Add(
                    Marshal.StringToCoTaskMemUTF8(argument));
            }
            if (argumentPointers.Count > 0)
            {
                argumentArray = Marshal.AllocHGlobal(
                    IntPtr.Size * argumentPointers.Count);
                for (var index = 0;
                     index < argumentPointers.Count; ++index)
                {
                    Marshal.WriteIntPtr(
                        argumentArray,
                        index * IntPtr.Size,
                        argumentPointers[index]);
                }
            }
            var context = GCHandle.ToIntPtr(host._selfHandle);
            if (NativeMethods.mcast_avalonia_host_create(
                    argumentPointers.Count,
                    argumentArray,
                    s_log,
                    context) == 0)
            {
                host.Dispose();
                error = "Native Avalonia host failed to start: "
                    + host.StartupLogText();
                return null;
            }
            if (NativeMethods
                    .mcast_avalonia_host_set_projection_update_callback(
                        s_projection, context) == 0
                || NativeMethods
                    .mcast_avalonia_host_set_render_update_callback(
                        s_render, context) == 0)
            {
                host.Dispose();
                error = "Native Avalonia callbacks failed to attach: "
                    + host.StartupLogText();
                return null;
            }
            try
            {
                host.RefreshNow();
            }
            catch (Exception exception)
            {
                host.Dispose();
                error = "Native Avalonia projections are invalid: "
                    + exception.Message
                    + " shell=" + host._lastShellJson;
                return null;
            }
            if (host.Current is null)
            {
                host.Dispose();
                error = "Native Avalonia projections could not be read: "
                    + host.StartupLogText();
                return null;
            }
            return host;
        }
        finally
        {
            foreach (var pointer in argumentPointers)
                Marshal.FreeCoTaskMem(pointer);
            if (argumentArray != IntPtr.Zero)
                Marshal.FreeHGlobal(argumentArray);
        }
    }

    public void NotifyFirstFrame()
    {
        if (!_disposed)
            NativeMethods.mcast_avalonia_host_notify_first_frame();
    }

    public bool PauseAndFlushForWindowClose()
        => !_disposed
            && NativeMethods
                .mcast_avalonia_host_pause_and_flush_for_window_close() != 0;

    public bool TogglePlayPause()
        => !_disposed
            && NativeMethods.mcast_avalonia_host_toggle_play_pause() != 0;

    public bool SeekAbsolute(double seconds)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_seek(seconds) != 0;

    public bool Previous()
        => !_disposed
            && NativeMethods.mcast_avalonia_host_previous() != 0;

    public bool Next()
        => !_disposed
            && NativeMethods.mcast_avalonia_host_next() != 0;

    public bool SetVolume(int percent)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_set_volume(percent) != 0;

    public bool SetMuted(bool muted)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_set_muted(muted ? 1 : 0) != 0;

    public bool SetPictureControl(string name, int value)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_set_picture_control(
                name, value) != 0;

    public bool ResetPictureControls()
        => !_disposed
            && NativeMethods
                .mcast_avalonia_host_reset_picture_controls() != 0;

    public bool BeginControlAdjustment()
        => !_disposed
            && NativeMethods
                .mcast_avalonia_host_begin_control_adjustment() != 0;

    public bool EndControlAdjustment()
        => !_disposed
            && NativeMethods
                .mcast_avalonia_host_end_control_adjustment() != 0;

    public bool RequestEndCast()
        => !_disposed
            && NativeMethods.mcast_avalonia_host_end_cast() != 0;

    public bool SetLanguage(string code)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_set_language(code) != 0;

    public bool SetThemeMode(ThemeMode mode)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_set_theme_mode(
                (int)mode) != 0;

    public bool SetAccentMode(AccentMode mode)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_set_accent_mode(
                (int)mode) != 0;

    public bool SetMica(bool enabled)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_set_mica(
                enabled ? 1 : 0) != 0;

    public bool SetAcceptNewCast(bool enabled)
        => !_disposed
            && NativeMethods
                .mcast_avalonia_host_set_accept_new_cast(
                    enabled ? 1 : 0) != 0;

    public bool SetBroadcast(bool enabled)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_set_broadcast(
                enabled ? 1 : 0) != 0;

    public bool SetBroadcastInterval(int milliseconds)
        => !_disposed
            && NativeMethods
                .mcast_avalonia_host_set_broadcast_interval(
                    milliseconds) != 0;

    public bool RespondCastConfirmation(ulong requestId, bool accepted)
        => !_disposed
            && NativeMethods
                .mcast_avalonia_host_respond_cast_confirmation(
                    requestId, accepted ? 1 : 0) != 0;

    public bool RenderAttach(
        GetProcAddressCallback callback, IntPtr context)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_render_attach(
                callback, context) != 0;

    public bool RenderFrame(
        int framebuffer, int width, int height, bool flipY)
        => !_disposed
            && NativeMethods.mcast_avalonia_host_render_frame(
                framebuffer, width, height, flipY ? 1 : 0) != 0;

    public void RenderDetach()
    {
        if (!_disposed)
            NativeMethods.mcast_avalonia_host_render_detach();
    }

    public void RenderLost(string reason)
    {
        if (!_disposed)
            NativeMethods.mcast_avalonia_host_render_lost(reason);
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        NativeMethods.mcast_avalonia_host_destroy();
        if (_selfHandle.IsAllocated)
            _selfHandle.Free();
    }

    internal IReadOnlyList<string> TakeStartupLogs()
    {
        lock (_startupLogGate)
        {
            _captureStartupLogs = false;
            var result = _startupLogs.ToArray();
            _startupLogs.Clear();
            return result;
        }
    }

    private void CaptureStartupLog(string message)
    {
        lock (_startupLogGate)
        {
            if (_captureStartupLogs)
                _startupLogs.Add(message);
        }
    }

    private string StartupLogText()
    {
        lock (_startupLogGate)
        {
            return _startupLogs.Count == 0
                ? "no native diagnostics"
                : string.Join(" | ", _startupLogs.TakeLast(8));
        }
    }

    private void ScheduleRefresh()
    {
        if (_disposed)
            return;
        if (Interlocked.Exchange(ref _refreshQueued, 1) != 0)
            return;
        Dispatcher.UIThread.Post(
            () =>
            {
                try
                {
                    RefreshNow();
                }
                finally
                {
                    Interlocked.Exchange(ref _refreshQueued, 0);
                    if (!_disposed && Current is not null)
                        ScheduleRefreshIfDirty();
                }
            },
            DispatcherPriority.Background);
    }

    private int _dirtyWhileRefreshing;

    private void ScheduleRefreshIfDirty()
    {
        if (Interlocked.Exchange(ref _dirtyWhileRefreshing, 0) != 0)
            ScheduleRefresh();
    }

    private void RefreshNow()
    {
        lock (_refreshGate)
        {
            if (_disposed)
                return;
            var shellJson = ReadUtf8(
                NativeMethods.mcast_avalonia_host_read_shell_json);
            _lastShellJson = shellJson;
            var playbackJson = ReadUtf8(
                NativeMethods.mcast_avalonia_host_read_playback_json);
            var rendererJson = ReadUtf8(
                NativeMethods.mcast_avalonia_host_read_renderer_json);
            var confirmationJson = ReadUtf8(
                NativeMethods
                    .mcast_avalonia_host_read_confirmation_json);
            var languageJson = ReadUtf8(
                NativeMethods.mcast_avalonia_host_read_language_json);

            var shell = JsonSerializer.Deserialize<ShellProjection>(
                shellJson, s_jsonOptions);
            var renderer = JsonSerializer.Deserialize<RendererProjection>(
                rendererJson, s_jsonOptions);
            if (shell is null || renderer is null)
                return;
            var playback = ParsePlayback(playbackJson);
            var confirmation = ParseConfirmation(confirmationJson);
            var languages = ParseLanguages(languageJson);
            var projection = new HostProjection(
                shell,
                playback,
                renderer,
                confirmation,
                languages,
                languageJson);
            Current = projection;
            Changed?.Invoke(projection);
        }
    }

    private static PlaybackProjection ParsePlayback(string json)
    {
        using var document = JsonDocument.Parse(
            string.IsNullOrWhiteSpace(json) ? "{}" : json);
        var root = document.RootElement;
        var controls = new List<PictureControlProjection>();
        if (root.TryGetProperty(
                "pictureControls", out var controlArray)
            && controlArray.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in controlArray.EnumerateArray())
            {
                controls.Add(new PictureControlProjection(
                    GetString(item, "name"),
                    GetInt32(item, "minimum"),
                    GetInt32(item, "maximum"),
                    GetInt32(item, "neutral"),
                    GetInt32(item, "value")));
            }
        }
        var cast = root.TryGetProperty(
                "castSession", out var castValue)
            ? castValue
            : default;
        return new PlaybackProjection(
            GetUInt64(root, "generation"),
            GetUInt64(root, "stateVersion"),
            (PlaybackState)GetInt32(root, "state"),
            (LoadStatus)GetInt32(root, "loadStatus"),
            GetBoolean(root, "playerAvailable"),
            GetBoolean(root, "playbackPlanIncluded", true),
            GetBoolean(root, "hasMedia"),
            GetBoolean(root, "idle", true),
            GetBoolean(root, "paused"),
            GetBoolean(root, "peerConnected"),
            (CastUiState)GetInt32(root, "castState"),
            GetBoolean(root, "showsPicture"),
            GetBoolean(root, "mediaIsVideo"),
            ParseMedia(
                root.TryGetProperty("currentMedia", out var current)
                    ? current : default),
            ParseMedia(
                root.TryGetProperty("nextMedia", out var next)
                    ? next : default),
            GetBoolean(root, "hasNext"),
            GetBoolean(root, "hasPrevious"),
            new CastSessionProjection(
                GetBoolean(cast, "connected"),
                GetString(cast, "protocol"),
                GetString(cast, "controlPointName"),
                GetString(cast, "controlPointAddress"),
                GetBoolean(cast, "remoteControlRegistered")),
            GetDouble(root, "positionSeconds"),
            GetDouble(root, "durationSeconds"),
            GetInt32(root, "volumePercent", 100),
            GetBoolean(root, "muted"),
            controls,
            ParseMediaInfo(
                root.TryGetProperty("mediaInfo", out var info)
                    ? info : default));
    }

    private static MediaProjection ParseMedia(JsonElement value)
    {
        ulong? size = null;
        if (value.ValueKind == JsonValueKind.Object
            && value.TryGetProperty(
                "mediaSizeBytes", out var sizeValue)
            && sizeValue.ValueKind == JsonValueKind.Number)
        {
            size = sizeValue.GetUInt64();
        }
        return new MediaProjection(
            GetString(value, "uri"),
            GetString(value, "title"),
            GetString(value, "artist"),
            GetString(value, "album"),
            GetString(value, "creator"),
            GetString(value, "subtitle"),
            GetString(value, "description"),
            GetString(value, "longDescription"),
            GetString(value, "artworkUri"),
            GetString(value, "protocol"),
            (MediaKind)GetInt32(value, "kind"),
            (MediaSource)GetInt32(value, "source"),
            size);
    }

    private static MediaInfoProjection ParseMediaInfo(JsonElement value)
    {
        VideoStreamProjection? video = null;
        AudioStreamProjection? audio = null;
        if (value.ValueKind == JsonValueKind.Object
            && value.TryGetProperty("video", out var videoValue)
            && videoValue.ValueKind == JsonValueKind.Object)
        {
            video = new VideoStreamProjection(
                GetString(videoValue, "codec"),
                GetString(videoValue, "profile"),
                GetInt32(videoValue, "width"),
                GetInt32(videoValue, "height"),
                GetDouble(videoValue, "fps"),
                GetInt64(videoValue, "bitrate"),
                GetString(videoValue, "pixel_format"),
                GetDouble(videoValue, "aspect"));
        }
        if (value.ValueKind == JsonValueKind.Object
            && value.TryGetProperty("audio", out var audioValue)
            && audioValue.ValueKind == JsonValueKind.Object)
        {
            audio = new AudioStreamProjection(
                GetString(audioValue, "codec"),
                GetString(audioValue, "profile"),
                GetInt32(audioValue, "sample_rate"),
                GetInt32(audioValue, "channels"),
                GetString(audioValue, "channel_layout"),
                GetString(audioValue, "sample_format"),
                GetInt64(audioValue, "bitrate"));
        }
        return new MediaInfoProjection(
            GetString(value, "container"),
            video,
            audio,
            GetDouble(value, "duration_seconds"));
    }

    private static CastConfirmationProjection? ParseConfirmation(
        string json)
    {
        using var document = JsonDocument.Parse(
            string.IsNullOrWhiteSpace(json) ? "{}" : json);
        var root = document.RootElement;
        var id = GetUInt64(root, "requestId");
        if (id == 0)
            return null;
        return new CastConfirmationProjection(
            id,
            GetInt32(root, "protocol"),
            GetUInt64(root, "claimId"),
            GetString(root, "peerId"),
            GetString(root, "peerName"),
            GetString(root, "peerAddress"),
            GetString(root, "mediaTitle"),
            GetUInt64(root, "remainingTimeoutMs"));
    }

    private static IReadOnlyList<LanguageProjection> ParseLanguages(
        string json)
    {
        using var document = JsonDocument.Parse(
            string.IsNullOrWhiteSpace(json) ? "{}" : json);
        if (!document.RootElement.TryGetProperty(
                "languages", out var languages)
            || languages.ValueKind != JsonValueKind.Array)
        {
            return [];
        }
        return languages
            .EnumerateArray()
            .Select(item => new LanguageProjection(
                GetString(item, "code"),
                GetString(item, "name")))
            .ToArray();
    }

    private static string ReadUtf8(ReadJsonCallback read)
    {
        for (var attempt = 0; attempt < 3; ++attempt)
        {
            read(IntPtr.Zero, 0, out var required);
            if (required <= 1)
                return "{}";
            var buffer = Marshal.AllocHGlobal(required);
            try
            {
                if (read(buffer, required, out _) != 0)
                    return Marshal.PtrToStringUTF8(buffer) ?? "{}";
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }
        throw new InvalidOperationException(
            "Native projection snapshot could not be read stably");
    }

    private static string GetString(JsonElement element, string name)
        => element.ValueKind == JsonValueKind.Object
            && element.TryGetProperty(name, out var value)
            && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;

    private static bool GetBoolean(
        JsonElement element, string name, bool fallback = false)
        => element.ValueKind == JsonValueKind.Object
            && element.TryGetProperty(name, out var value)
            ? value.ValueKind == JsonValueKind.True
            : fallback;

    private static int GetInt32(
        JsonElement element, string name, int fallback = 0)
        => element.ValueKind == JsonValueKind.Object
            && element.TryGetProperty(name, out var value)
            && value.ValueKind == JsonValueKind.Number
            ? value.GetInt32()
            : fallback;

    private static long GetInt64(
        JsonElement element, string name, long fallback = 0)
        => element.ValueKind == JsonValueKind.Object
            && element.TryGetProperty(name, out var value)
            && value.ValueKind == JsonValueKind.Number
            ? value.GetInt64()
            : fallback;

    private static ulong GetUInt64(
        JsonElement element, string name, ulong fallback = 0)
        => element.ValueKind == JsonValueKind.Object
            && element.TryGetProperty(name, out var value)
            && value.ValueKind == JsonValueKind.Number
            ? value.GetUInt64()
            : fallback;

    private static double GetDouble(
        JsonElement element, string name, double fallback = 0)
        => element.ValueKind == JsonValueKind.Object
            && element.TryGetProperty(name, out var value)
            && value.ValueKind == JsonValueKind.Number
            ? value.GetDouble()
            : fallback;

    private static void ReceiveLog(string message, IntPtr context)
    {
        var target = FromContext(context);
        if (target?.LogReceived is { } handlers)
            handlers.Invoke(message);
        else
            Console.Error.WriteLine($"[Native] {message}");
    }

    private static void ReceiveProjectionUpdate(
        uint dirtyDomains, IntPtr context)
    {
        _ = dirtyDomains;
        var target = FromContext(context);
        target?.ScheduleRefresh();
    }

    private static void ReceiveRenderUpdate(IntPtr context)
    {
        var target = FromContext(context);
        if (target is null)
            return;
        Interlocked.Increment(ref target._renderRevision);
        target.QueueRenderRefresh();
    }

    private void QueueRenderRefresh()
    {
        if (Interlocked.Exchange(
                ref _renderRefreshQueued, 1) != 0)
        {
            return;
        }
        Dispatcher.UIThread.Post(
            RunRenderRefresh,
            DispatcherPriority.Render);
    }

    private void RunRenderRefresh()
    {
        var revision = Volatile.Read(ref _renderRevision);
        try
        {
            if (!_disposed)
                RenderUpdateRequested?.Invoke();
        }
        finally
        {
            Interlocked.Exchange(ref _renderRefreshQueued, 0);
            if (Volatile.Read(ref _renderRevision) != revision)
                QueueRenderRefresh();
        }
    }

    private static NativeAvaloniaHost? FromContext(IntPtr context)
    {
        if (context == IntPtr.Zero)
            return null;
        return GCHandle.FromIntPtr(context).Target
            as NativeAvaloniaHost;
    }

    private static class NativeMethods
    {
        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_create(
            int argumentCount, IntPtr arguments,
            LogCallback logCallback, IntPtr context);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void mcast_avalonia_host_destroy();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int
            mcast_avalonia_host_set_projection_update_callback(
                ProjectionUpdateCallback callback, IntPtr context);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int
            mcast_avalonia_host_set_render_update_callback(
                RenderUpdateCallback callback, IntPtr context);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_read_shell_json(
            IntPtr buffer, int capacity, out int required);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_read_language_json(
            IntPtr buffer, int capacity, out int required);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_read_playback_json(
            IntPtr buffer, int capacity, out int required);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_read_renderer_json(
            IntPtr buffer, int capacity, out int required);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int
            mcast_avalonia_host_read_confirmation_json(
                IntPtr buffer, int capacity, out int required);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void mcast_avalonia_host_notify_first_frame();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_render_attach(
            GetProcAddressCallback callback, IntPtr context);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_render_frame(
            int framebuffer, int width, int height, int flipY);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void mcast_avalonia_host_render_detach();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void mcast_avalonia_host_render_lost(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string reason);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_toggle_play_pause();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_seek(double seconds);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_previous();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_next();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_set_volume(int percent);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_set_muted(int muted);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_set_picture_control(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name, int value);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int
            mcast_avalonia_host_reset_picture_controls();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int
            mcast_avalonia_host_begin_control_adjustment();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int
            mcast_avalonia_host_end_control_adjustment();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_end_cast();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int
            mcast_avalonia_host_pause_and_flush_for_window_close();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_set_language(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string language);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_set_theme_mode(int mode);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_set_accent_mode(int mode);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_set_mica(int enabled);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int
            mcast_avalonia_host_set_accept_new_cast(int enabled);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int mcast_avalonia_host_set_broadcast(int enabled);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int
            mcast_avalonia_host_set_broadcast_interval(int milliseconds);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int
            mcast_avalonia_host_respond_cast_confirmation(
                ulong requestId, int accepted);
    }
}
