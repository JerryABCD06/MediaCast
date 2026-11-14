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
            LanguageCombo.ItemsSource = languages;
            LanguageCombo.SelectedItem =
                languages.FirstOrDefault(
                    item => item.Value == requested)
                ?? languages[0];
            ThemeCombo.SelectedItem =
                s_themes.FirstOrDefault(
                    item => item.Value == _shell.ThemeMode)
                ?? s_themes[0];
            AccentCombo.SelectedItem =
                s_accents.FirstOrDefault(
                    item => item.Value == _shell.AccentMode)
                ?? s_accents[0];
            MicaToggle.IsChecked = _shell.MicaEnabled;
            AcceptCastToggle.IsChecked = _shell.AcceptNewCast;
            BroadcastToggle.IsChecked = _shell.BroadcastEnabled;
            BroadcastIntervalCombo.SelectedIndex = Math.Max(
                0, Array.IndexOf(
                    s_intervals, _shell.BroadcastIntervalMs));
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
            _host.SetAccentMode(choice.Value);
        }
    }

    private void MicaClicked(object? sender, RoutedEventArgs e)
    {
        if (!_updating && _shell is not null)
            _host.SetMica(!_shell.MicaEnabled);
    }

    private void AcceptCastClicked(object? sender, RoutedEventArgs e)
    {
        if (!_updating && _shell is not null)
            _host.SetAcceptNewCast(!_shell.AcceptNewCast);
    }

    private void BroadcastClicked(object? sender, RoutedEventArgs e)
    {
        if (!_updating && _shell is not null)
            _host.SetBroadcast(!_shell.BroadcastEnabled);
    }

    private void BroadcastIntervalChanged(
        object? sender, SelectionChangedEventArgs e)
    {
        if (_updating)
            return;
        var index = BroadcastIntervalCombo.SelectedIndex;
        if (index >= 0 && index < s_intervals.Length)
            _host.SetBroadcastInterval(s_intervals[index]);
    }

    private sealed record Choice<T>(string Title, T Value)
    {
        public override string ToString() => Title;
    }
}
