using System.Runtime.InteropServices;

using Avalonia.Controls;

namespace MCast.Ui.Avalonia.Platform;

internal sealed class WindowsWindowInteractionMonitor : IDisposable
{
    private const uint WmEnterSizeMove = 0x0231;
    private const uint WmExitSizeMove = 0x0232;
    private const uint WmNcDestroy = 0x0082;
    private static readonly UIntPtr SubclassId = new(1);
    private static readonly SubclassProc Procedure = WindowSubclass;
    private static readonly Dictionary<IntPtr, WindowsWindowInteractionMonitor>
        Monitors = [];

    private readonly IntPtr _handle;
    private readonly Action<bool> _changed;
    private bool _disposed;
    private bool _interacting;

    private WindowsWindowInteractionMonitor(
        IntPtr handle, Action<bool> changed)
    {
        _handle = handle;
        _changed = changed;
    }

    internal static WindowsWindowInteractionMonitor? TryCreate(
        TopLevel topLevel, Action<bool> changed)
    {
        var handle = topLevel.TryGetPlatformHandle()?.Handle
            ?? IntPtr.Zero;
        if (handle == IntPtr.Zero)
            return null;
        var monitor = new WindowsWindowInteractionMonitor(
            handle, changed);
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
        WindowsWindowInteractionMonitor? monitor = null;
        if (message is WmEnterSizeMove or WmExitSizeMove)
        {
            lock (Monitors)
                Monitors.TryGetValue(hWnd, out monitor);
            if (message == WmEnterSizeMove && monitor is not null
                && !monitor._interacting)
            {
                monitor._interacting = true;
                Notify(monitor, true);
            }
        }
        else if (message == WmNcDestroy)
        {
            lock (Monitors)
                Monitors.Remove(hWnd);
        }
        var result = DefSubclassProc(hWnd, message, wParam, lParam);
        if (message == WmExitSizeMove && monitor is not null
            && monitor._interacting)
        {
            monitor._interacting = false;
            Notify(monitor, false);
        }
        return result;
    }

    private static void Notify(
        WindowsWindowInteractionMonitor monitor, bool interacting)
    {
        try
        {
            monitor._changed(interacting);
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
