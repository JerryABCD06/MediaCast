namespace MCast.Ui.Avalonia.Contracts;

public enum ThemeMode
{
    System,
    Light,
    Dark,
}

public enum AccentMode
{
    WindowsBlue,
    System,
}

public enum PlaybackState
{
    NoMedia,
    Stopped,
    Preparing,
    Playing,
    Paused,
}

public enum LoadStatus
{
    Ok,
    Failed,
}

public enum MediaKind
{
    Unknown,
    Video,
    Audio,
    Image,
}

public enum MediaSource
{
    Cast,
    Local,
}

public enum CastUiState
{
    NoViewer,
    ViewerIdle,
    ViewerVideo,
    ViewerAudio,
    ViewerImage,
}

public sealed record LanguageProjection(string Code, string Name);

public sealed record ShellProjection(
    ulong SettingsVersion,
    string RequestedLanguage,
    string ResolvedLanguage,
    ThemeMode ThemeMode,
    AccentMode AccentMode,
    bool MicaEnabled,
    bool AcceptNewCast,
    bool BroadcastEnabled,
    int BroadcastIntervalMs,
    string DeviceName,
    string DeviceUdn,
    string DeviceAddress,
    string AppVersion,
    bool UiSessionConnected,
    bool ConfirmationOnly,
    bool TestTakeoverWindow,
    string FatalReason);

public sealed record MediaProjection(
    string Uri,
    string Title,
    string Artist,
    string Album,
    string Creator,
    string Subtitle,
    string Description,
    string LongDescription,
    string ArtworkUri,
    string Protocol,
    MediaKind Kind,
    MediaSource Source,
    ulong? MediaSizeBytes);

public sealed record CastSessionProjection(
    bool Connected,
    string Protocol,
    string ControlPointName,
    string ControlPointAddress,
    bool RemoteControlRegistered);

public sealed record PictureControlProjection(
    string Name,
    int Minimum,
    int Maximum,
    int Neutral,
    int Value);

public sealed record VideoStreamProjection(
    string Codec,
    string Profile,
    int Width,
    int Height,
    double Fps,
    long Bitrate,
    string PixelFormat,
    double AspectRatio);

public sealed record AudioStreamProjection(
    string Codec,
    string Profile,
    int SampleRate,
    int Channels,
    string ChannelLayout,
    string SampleFormat,
    long Bitrate);

public sealed record MediaInfoProjection(
    string Container,
    VideoStreamProjection? Video,
    AudioStreamProjection? Audio,
    double DurationSeconds);

public sealed record PlaybackProjection(
    ulong Generation,
    ulong StateVersion,
    PlaybackState State,
    LoadStatus LoadStatus,
    bool PlayerAvailable,
    bool PlaybackPlanIncluded,
    bool HasMedia,
    bool Idle,
    bool Paused,
    bool PeerConnected,
    CastUiState CastState,
    bool ShowsPicture,
    bool MediaIsVideo,
    MediaProjection CurrentMedia,
    MediaProjection NextMedia,
    bool HasNext,
    bool HasPrevious,
    CastSessionProjection CastSession,
    double PositionSeconds,
    double DurationSeconds,
    int VolumePercent,
    bool Muted,
    IReadOnlyList<PictureControlProjection> PictureControls,
    MediaInfoProjection MediaInfo);

public sealed record RendererProjection(
    bool PlayerRunning,
    bool RendererReady,
    bool ContextLost,
    ulong RenderContextGeneration,
    ulong RenderCalls,
    ulong RenderFailures,
    int LastFramebuffer,
    int LastWidth,
    int LastHeight,
    string GlBackend);

public sealed record CastConfirmationProjection(
    ulong RequestId,
    int Protocol,
    ulong ClaimId,
    string PeerId,
    string PeerName,
    string PeerAddress,
    string MediaTitle,
    ulong RemainingTimeoutMs);

public sealed record HostProjection(
    ShellProjection Shell,
    PlaybackProjection Playback,
    RendererProjection Renderer,
    CastConfirmationProjection? Confirmation,
    IReadOnlyList<LanguageProjection> Languages,
    string RawLanguageCatalogJson);

public sealed record UiInteractionState(
    bool SettingsOpen,
    bool Fullscreen,
    bool ControlAdjustmentActive,
    bool Seeking);

