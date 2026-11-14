using System.Text;

namespace MCast.Ui.Avalonia.Diagnostics;

internal sealed class UiDiagnosticLog : IDisposable
{
    private readonly object _gate = new();
    private readonly StreamWriter? _writer;

    public UiDiagnosticLog(string path)
    {
        try
        {
            _writer = new StreamWriter(
                new FileStream(
                    path,
                    FileMode.Append,
                    FileAccess.Write,
                    FileShare.ReadWrite),
                new UTF8Encoding(false))
            {
                AutoFlush = true,
            };
            Write($"===== Avalonia Ui {DateTimeOffset.Now:O} =====");
        }
        catch
        {
            _writer = null;
        }
    }

    public void Write(string message)
    {
        lock (_gate)
        {
            _writer?.WriteLine(
                $"{DateTimeOffset.Now:HH:mm:ss.fff}  Ui  {message}");
        }
    }

    public void Dispose()
    {
        lock (_gate)
        {
            _writer?.Dispose();
        }
    }
}
