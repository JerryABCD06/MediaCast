using Avalonia.Controls;
using Avalonia.Interactivity;

using MCast.Ui.Avalonia.Contracts;
using MCast.Ui.Avalonia.Native;

namespace MCast.Ui.Avalonia.Views;

internal sealed partial class SettingsPage : UserControl
{
    private static readonly Choice<ThemeMode>[] s_themes =
    [
        new("Use system setting", ThemeMode.System),
        new("Light", ThemeMode.Light),
        new("Dark", ThemeMode.Dark),
    ];

    private static readonly Choice<AccentMode>[] s_accents =
    [
        new("Windows blue", AccentMode.WindowsBlue),
        new("System accent", AccentMode.System),
    ];

    private static readonly int[] s_intervals =
        [5000, 10000, 30000, 60000];

    private readonly IShellSettingsIntentPort _host;
    private readonly Dictionary<string, PendingSetting> _pending = [];
    private ShellProjection? _shell;
    private bool _updating;

    internal SettingsPage(NativeAvaloniaHost host)
    {
        InitializeComponent();
        _host = host;
        ThemeCombo.ItemsSource = s_themes;
        AccentCombo.ItemsSource = s_accents;
        BroadcastIntervalCombo.ItemsSource =
            s_intervals.Select(ms => $"{ms / 1000} seconds").ToArray();
        Categories.SelectionChanged += (_, _) => UpdateCategory();
        Categories.SelectedItem = UiNavigationItem;
        UpdateCategory();
    }

    internal void ApplyProjection(HostProjection projection)
    {
        _shell = projection.Shell;
        _updating = true;
        try
        {
            var languages = new List<Choice<string>>
            {
                new("Use system setting", "System"),
            };
            languages.AddRange(
                projection.Languages
                    .Where(item => !string.IsNullOrWhiteSpace(item.Code))
                    .Select(item =>
                        new Choice<string>(item.Name, item.Code)));
            var requested = _shell.RequestedLanguage;
            if (requested.Length == 0)
                requested = "System";
            requested = PendingOrAuthority("language", requested);
            LanguageCombo.ItemsSource = languages;
            LanguageCombo.SelectedItem =
                languages.FirstOrDefault(
                    item => item.Value == requested)
                ?? languages[0];
            var themeMode = PendingOrAuthority(
                "theme", _shell.ThemeMode);
            ThemeCombo.SelectedItem =
                s_themes.FirstOrDefault(
                    item => item.Value == themeMode)
                ?? s_themes[0];
            var accentMode = PendingOrAuthority(
                "accent", _shell.AccentMode);
            AccentCombo.SelectedItem =
                s_accents.FirstOrDefault(
                    item => item.Value == accentMode)
                ?? s_accents[0];
            MicaToggle.IsChecked = PendingOrAuthority(
                "mica", _shell.MicaEnabled);
            AcceptCastToggle.IsChecked = PendingOrAuthority(
                "accept", _shell.AcceptNewCast);
            BroadcastToggle.IsChecked = PendingOrAuthority(
                "broadcast", _shell.BroadcastEnabled);
            var interval = PendingOrAuthority(
                "interval", _shell.BroadcastIntervalMs);
            BroadcastIntervalCombo.SelectedIndex = Math.Max(
                0, Array.IndexOf(
                    s_intervals, interval));
        }
        finally
        {
            _updating = false;
        }
    }

    private void UpdateCategory()
    {
        var cast = ReferenceEquals(
            Categories.SelectedItem, CastNavigationItem);
        UiPane.IsVisible = !cast;
        CastPane.IsVisible = cast;
        UiPane.IsHitTestVisible = !cast;
        CastPane.IsHitTestVisible = cast;
    }

    private void LanguageChanged(
        object? sender, SelectionChangedEventArgs e)
    {
        if (_updating
            || LanguageCombo.SelectedItem
                is not Choice<string> choice)
        {
            return;
        }
        MarkPending("language", choice.Value);
        _host.SetLanguage(
            choice.Value == "System" ? string.Empty : choice.Value);
    }

    private void ThemeChanged(
        object? sender, SelectionChangedEventArgs e)
    {
        if (!_updating
            && ThemeCombo.SelectedItem
                is Choice<ThemeMode> choice)
        {
            MarkPending("theme", choice.Value);
            _host.SetThemeMode(choice.Value);
        }
    }

    private void AccentChanged(
        object? sender, SelectionChangedEventArgs e)
    {
        if (!_updating
            && AccentCombo.SelectedItem
                is Choice<AccentMode> choice)
        {
            MarkPending("accent", choice.Value);
            _host.SetAccentMode(choice.Value);
        }
    }

    private void MicaClicked(object? sender, RoutedEventArgs e)
    {
        if (!_updating && _shell is not null)
        {
            var desired = !PendingOrAuthority(
                "mica", _shell.MicaEnabled);
            MarkPending("mica", desired);
            _host.SetMica(desired);
        }
    }

    private void AcceptCastClicked(object? sender, RoutedEventArgs e)
    {
        if (!_updating && _shell is not null)
        {
            var desired = !PendingOrAuthority(
                "accept", _shell.AcceptNewCast);
            MarkPending("accept", desired);
            _host.SetAcceptNewCast(desired);
        }
    }

    private void BroadcastClicked(object? sender, RoutedEventArgs e)
    {
        if (!_updating && _shell is not null)
        {
            var desired = !PendingOrAuthority(
                "broadcast", _shell.BroadcastEnabled);
            MarkPending("broadcast", desired);
            _host.SetBroadcast(desired);
        }
    }

    private void BroadcastIntervalChanged(
        object? sender, SelectionChangedEventArgs e)
    {
        if (_updating)
            return;
        var index = BroadcastIntervalCombo.SelectedIndex;
        if (index >= 0 && index < s_intervals.Length)
        {
            MarkPending("interval", s_intervals[index]);
            _host.SetBroadcastInterval(s_intervals[index]);
        }
    }

    private T PendingOrAuthority<T>(string key, T authority)
        where T : notnull
    {
        if (!_pending.TryGetValue(key, out var pending))
            return authority;
        if (DateTimeOffset.UtcNow >= pending.Until
            || Equals(pending.Value, authority))
        {
            _pending.Remove(key);
            return authority;
        }
        return pending.Value is T value ? value : authority;
    }

    private void MarkPending<T>(string key, T value)
        where T : notnull
    {
        _pending[key] = new PendingSetting(
            value, DateTimeOffset.UtcNow.AddSeconds(1.5));
    }

    private sealed record PendingSetting(
        object Value,
        DateTimeOffset Until);

    private sealed record Choice<T>(string Title, T Value)
    {
        public override string ToString() => Title;
    }
}
