using Avalonia;
using Avalonia.Controls;
using Avalonia.OpenGL;
using Avalonia.OpenGL.Controls;
using Avalonia.Threading;

using MCast.Ui.Avalonia.Native;

namespace MCast.Ui.Avalonia.Rendering;

internal sealed class MpvOpenGlSurface : OpenGlControlBase
{
    private readonly NativeAvaloniaHost _host;
    private readonly NativeAvaloniaHost.GetProcAddressCallback
        _getProc;
    private GlInterface? _gl;
    private bool _attached;
    private bool _subscribed;
    private bool _renderingActive = true;
    private bool _contextUsable;

    public event Action? FrameRendered;

    public MpvOpenGlSurface(NativeAvaloniaHost host)
    {
        _host = host;
        ClipToBounds = true;
        _getProc = GetProcAddress;
    }

    public void SetRenderingActive(bool active)
    {
        if (_renderingActive == active)
            return;
        _renderingActive = active;
        if (active)
            RequestVideoFrame();
    }

    protected override void OnAttachedToVisualTree(
        VisualTreeAttachmentEventArgs e)
    {
        if (!_subscribed)
        {
            _host.RenderUpdateRequested += OnRenderUpdateRequested;
            _subscribed = true;
        }
        base.OnAttachedToVisualTree(e);
    }

    protected override void OnDetachedFromVisualTree(
        VisualTreeAttachmentEventArgs e)
    {
        if (_subscribed)
        {
            _host.RenderUpdateRequested -= OnRenderUpdateRequested;
            _subscribed = false;
        }
        base.OnDetachedFromVisualTree(e);
    }

    protected override void OnOpenGlInit(GlInterface gl)
    {
        _gl = gl;
        _attached = _host.RenderAttach(_getProc, IntPtr.Zero);
        _contextUsable = _attached;
        Trace(
            $"GL init renderer={gl.Renderer} "
            + $"vendor={gl.Vendor} attached={_attached}");
    }

    protected override void OnOpenGlRender(GlInterface gl, int framebuffer)
    {
        if (!_renderingActive || !_contextUsable)
            return;
        var scaling = TopLevel.GetTopLevel(this)?.RenderScaling ?? 1;
        var width = Math.Max(
            1, (int)Math.Round(Bounds.Width * scaling));
        var height = Math.Max(
            1, (int)Math.Round(Bounds.Height * scaling));
        gl.Viewport(0, 0, width, height);
        gl.ClearColor(0.025f, 0.04f, 0.06f, 1);
        gl.Clear(0x00004000);
        if (_attached
            && _host.RenderFrame(framebuffer, width, height, false))
        {
            FrameRendered?.Invoke();
        }
    }

    protected override void OnOpenGlDeinit(GlInterface gl)
    {
        _attached = false;
        _contextUsable = false;
        _host.RenderDetach();
        _gl = null;
        Trace("GL deinit");
    }

    protected override void OnOpenGlLost()
    {
        _attached = false;
        _contextUsable = false;
        _gl = null;
        _host.RenderLost("Avalonia OpenGL context lost");
        Trace("GL context lost; fail closed");
    }

    private void OnRenderUpdateRequested()
    {
        if (_renderingActive && _contextUsable)
            RequestNextFrameRendering();
    }

    private void RequestVideoFrame()
    {
        if (_renderingActive && _contextUsable)
            RequestNextFrameRendering();
    }

    private IntPtr GetProcAddress(IntPtr _, string name)
        => _gl?.GetProcAddress(name) ?? IntPtr.Zero;

    private static void Trace(string message)
    {
        Console.WriteLine($"[MCast GL] {message}");
        System.Diagnostics.Trace.WriteLine($"[MCast GL] {message}");
    }
}

