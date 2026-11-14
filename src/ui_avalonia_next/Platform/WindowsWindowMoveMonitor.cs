using System.Runtime.InteropServices;

using Avalonia.Controls;

namespace MCast.Ui.Avalonia.Platform;

internal sealed class WindowsWindowMoveMonitor : IDisposable
{
    private const uint WmMoving = 0x0216;
    private const uint WmExitSizeMove = 0x0232;
    private const uint WmNcDestroy = 0x0082;
    private static readonly UIntPtr SubclassId = new(1);
    private static readonly SubclassProc Procedure = WindowSubclass;
    private static readonly Dictionary<IntPtr, WindowsWindowMoveMonitor>
        Monitors = [];

    private readonly IntPtr _handle;
    private readonly Action<bool> _changed;
    private bool _disposed;
    private bool _moving;

    private WindowsWindowMoveMonitor(
        IntPtr handle, Action<bool> changed)
    {
        _handle = handle;
        _changed = changed;
    }

    internal static WindowsWindowMoveMonitor? TryCreate(
        TopLevel topLevel, Action<bool> changed)
    {
        var handle = topLevel.TryGetPlatformHandle()?.Handle
            ?? IntPtr.Zero;
        if (handle == IntPtr.Zero)
            return null;
        var monitor = new WindowsWindowMoveMonitor(handle, changed);
        lock (Monitors)
            Monitors[handle] = monitor;
        if (!SetWindowSubclass(
                handle, Procedure, SubclassId, UIntPtr.Zero))
        {
            lock (Monitors)
                Monitors.Remove(handle);
            return null;
        }
        return monitor;
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        RemoveWindowSubclass(_handle, Procedure, SubclassId);
        lock (Monitors)
        {
            if (Monitors.TryGetValue(_handle, out var current)
                && ReferenceEquals(current, this))
            {
                Monitors.Remove(_handle);
            }
        }
    }

    private static IntPtr WindowSubclass(
        IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam,
        UIntPtr subclassId, UIntPtr refData)
    {
        if (message is WmMoving or WmExitSizeMove)
        {
            WindowsWindowMoveMonitor? monitor;
            lock (Monitors)
                Monitors.TryGetValue(hWnd, out monitor);
            if (monitor is not null)
            {
                if (message == WmMoving && !monitor._moving)
                {
                    monitor._moving = true;
                    Notify(monitor, true);
                }
                else if (message == WmExitSizeMove && monitor._moving)
                {
                    monitor._moving = false;
                    Notify(monitor, false);
                }
            }
        }
        else if (message == WmNcDestroy)
        {
            lock (Monitors)
                Monitors.Remove(hWnd);
        }
        return DefSubclassProc(hWnd, message, wParam, lParam);
    }

    private static void Notify(
        WindowsWindowMoveMonitor monitor, bool moving)
    {
        try
        {
            monitor._changed(moving);
        }
        catch
        {
            // Managed exceptions must not cross the Win32 callback boundary.
        }
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate IntPtr SubclassProc(
        IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam,
        UIntPtr subclassId, UIntPtr refData);

    [DllImport(
        "comctl32.dll", SetLastError = true,
        CallingConvention = CallingConvention.Winapi)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowSubclass(
        IntPtr hWnd, SubclassProc procedure,
        UIntPtr subclassId, UIntPtr refData);

    [DllImport(
        "comctl32.dll", SetLastError = true,
        CallingConvention = CallingConvention.Winapi)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RemoveWindowSubclass(
        IntPtr hWnd, SubclassProc procedure, UIntPtr subclassId);

    [DllImport(
        "comctl32.dll",
        CallingConvention = CallingConvention.Winapi)]
    private static extern IntPtr DefSubclassProc(
        IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
}
