using MCast.Ui.Avalonia.Contracts;

namespace MCast.Ui.Avalonia.Contracts;

public interface IProjectionSource
{
    HostProjection? Current { get; }
    event Action<HostProjection>? Changed;
}

public interface IPlaybackIntentPort
{
    bool TogglePlayPause();
    bool SeekAbsolute(double seconds);
    bool Previous();
    bool Next();
    bool SetVolume(int percent);
    bool SetMuted(bool muted);
    bool SetPictureControl(string name, int value);
    bool ResetPictureControls();
    bool BeginControlAdjustment();
    bool EndControlAdjustment();
    bool RequestEndCast();
}

public interface IShellSettingsIntentPort
{
    bool SetLanguage(string code);
    bool SetThemeMode(ThemeMode mode);
    bool SetAccentMode(AccentMode mode);
    bool SetMica(bool enabled);
    bool SetAcceptNewCast(bool enabled);
    bool SetBroadcast(bool enabled);
    bool SetBroadcastInterval(int milliseconds);
}

public interface IWindowLifecyclePort
{
    void NotifyFirstFrame();
    bool PauseAndFlushForWindowClose();
}

public interface ICastConfirmationPort
{
    bool RespondCastConfirmation(ulong requestId, bool accepted);
}

