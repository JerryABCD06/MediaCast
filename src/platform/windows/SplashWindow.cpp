// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>
//
// The native D3D11/D2D/DirectComposition window stack is adapted from the
// author's NoUninstallIcon project.

#include "SplashWindow.h"
#include "AccentPalette.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT WM_MCAST_SPLASH_SHOW = WM_APP + 0x410;
constexpr UINT WM_MCAST_SPLASH_THEME = WM_APP + 0x411;
constexpr UINT WM_MCAST_SPLASH_FRONT = WM_APP + 0x412;
constexpr UINT WM_MCAST_SPLASH_DISMISS = WM_APP + 0x413;
constexpr UINT WM_MCAST_SPLASH_HIDE = WM_APP + 0x414;
constexpr UINT WM_MCAST_SPLASH_ACCENT = WM_APP + 0x415;

constexpr UINT kSplashLogoResource = 102;
constexpr wchar_t kWindowClass[] = L"MediaCast.SplashWindow";

constexpr float kWindowWidthDip = 560.0f;
constexpr float kWindowHeightDip = 300.0f;
constexpr float kLogoSizeDip = 96.0f;
constexpr float kLogoTopDip = 72.0f;
constexpr float kProgressWidthDip = 280.0f;
constexpr float kProgressHeightDip = 4.0f;
constexpr float kProgressTopDip = 232.0f;

constexpr auto kMinimumVisible = std::chrono::milliseconds(280);
constexpr auto kFadeDuration = std::chrono::milliseconds(150);
// Same indeterminate timing as FluentUI's FluProgressBar.
constexpr auto kAnimationPeriod = std::chrono::milliseconds(888);

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif

float Clamp(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

D2D1_COLOR_F ColorFromRgb(uint32_t rgb, float alpha = 1.0f)
{
    D2D1_COLOR_F color{};
    color.r = static_cast<float>((rgb >> 16) & 0xff) / 255.0f;
    color.g = static_cast<float>((rgb >> 8) & 0xff) / 255.0f;
    color.b = static_cast<float>(rgb & 0xff) / 255.0f;
    color.a = alpha;
    return color;
}

bool SystemDarkMode()
{
    DWORD light = 1;
    DWORD size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size)
        == ERROR_SUCCESS) {
        return light == 0;
    }
    return false;
}

struct Gfx
{
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> compositionTarget;
    ComPtr<IDCompositionVisual> compositionVisual;
    ComPtr<IDCompositionEffectGroup> compositionEffect;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> backBuffer;
    UINT pixelWidth = 0;
    UINT pixelHeight = 0;
    float dpi = 96.0f;
};

bool CreateBackBuffer(Gfx &gfx, UINT width, UINT height, float dpi)
{
    gfx.dc->SetTarget(nullptr);
    gfx.backBuffer.Reset();
    gfx.dc->SetDpi(dpi, dpi);

    ComPtr<IDXGISurface> surface;
    if (FAILED(gfx.swapChain->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf()))))
        return false;

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi, dpi);
    if (FAILED(gfx.dc->CreateBitmapFromDxgiSurface(
            surface.Get(), &properties, gfx.backBuffer.GetAddressOf())))
        return false;

    gfx.dc->SetTarget(gfx.backBuffer.Get());
    gfx.pixelWidth = width;
    gfx.pixelHeight = height;
    gfx.dpi = dpi;
    return true;
}

bool CreateGfx(Gfx &gfx, HWND hwnd, UINT width, UINT height, float dpi)
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   gfx.d3d.GetAddressOf(), nullptr,
                                   gfx.d3dContext.GetAddressOf());
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               gfx.d3d.GetAddressOf(), nullptr,
                               gfx.d3dContext.GetAddressOf());
    }
    if (FAILED(hr))
        return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(gfx.d3d.As(&dxgiDevice)))
        return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())))
        return false;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Three buffers leave one complete frame in flight while the compositor is
    // displaying another, which avoids stalls at 144 Hz.
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(factory->CreateSwapChainForComposition(
            gfx.d3d.Get(), &desc, nullptr, gfx.swapChain.GetAddressOf())))
        return false;

    const D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory1), &options,
                                 reinterpret_cast<void **>(gfx.d2dFactory.GetAddressOf()))))
        return false;
    if (FAILED(gfx.d2dFactory->CreateDevice(dxgiDevice.Get(),
                                            gfx.d2dDevice.GetAddressOf())))
        return false;
    if (FAILED(gfx.d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, gfx.dc.GetAddressOf())))
        return false;

    if (FAILED(DCompositionCreateDevice(
            dxgiDevice.Get(), IID_PPV_ARGS(gfx.composition.GetAddressOf()))))
        return false;
    if (FAILED(gfx.composition->CreateTargetForHwnd(
            hwnd, TRUE, gfx.compositionTarget.GetAddressOf())))
        return false;
    if (FAILED(gfx.composition->CreateVisual(
            gfx.compositionVisual.GetAddressOf())))
        return false;
    if (FAILED(gfx.composition->CreateEffectGroup(
            gfx.compositionEffect.GetAddressOf())))
        return false;
    gfx.compositionEffect->SetOpacity(1.0f);
    gfx.compositionVisual->SetEffect(gfx.compositionEffect.Get());

    gfx.compositionVisual->SetContent(gfx.swapChain.Get());
    gfx.compositionTarget->SetRoot(gfx.compositionVisual.Get());
    gfx.composition->Commit();

    if (!CreateBackBuffer(gfx, width, height, dpi))
        return false;

    gfx.dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    gfx.dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    gfx.dc->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    return true;
}

bool ResizeGfx(Gfx &gfx, UINT width, UINT height, float dpi)
{
    if (!gfx.swapChain || width == 0 || height == 0)
        return true;
    gfx.dc->SetTarget(nullptr);
    gfx.backBuffer.Reset();
    if (FAILED(gfx.swapChain->ResizeBuffers(
            3, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
        return false;
    return CreateBackBuffer(gfx, width, height, dpi);
}

void DestroyGfx(Gfx &gfx)
{
    if (gfx.dc)
        gfx.dc->SetTarget(nullptr);
    gfx.backBuffer.Reset();
    gfx.compositionEffect.Reset();
    gfx.compositionVisual.Reset();
    gfx.compositionTarget.Reset();
    gfx.composition.Reset();
    gfx.swapChain.Reset();
    gfx.dc.Reset();
    gfx.d2dDevice.Reset();
    gfx.d2dFactory.Reset();
    gfx.d3dContext.Reset();
    gfx.d3d.Reset();
}

class SplashUi
{
public:
    explicit SplashUi(SplashWindow::StateCallback callback)
        : m_callback(std::move(callback))
    {
    }

    ~SplashUi();

    bool initialize();
    void requestShow(std::uint64_t token);
    void setTheme(bool followSystem, bool dark);
    void setAccent(unsigned int lightRgb, unsigned int darkRgb);
    void bringToFront(std::uint64_t token);
    void requestDismiss(std::uint64_t token);
    void hideNow(std::uint64_t token);
    void tick();
    bool isVisible() const { return m_visible; }

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool createNativeWindow();
    bool loadLogo();
    void refreshSystemTheme();
    void applyTheme();
    void resizeToMonitor();
    void render();
    void drawFrame();
    void beginFade();
    void finishFade();
    void notify(SplashWindow::State state) const;

    HWND m_hwnd = nullptr;
    Gfx m_gfx;
    ComPtr<ID2D1Bitmap> m_logo;
    ComPtr<IWICImagingFactory> m_wic;
    float m_scale = 1.0f;
    bool m_followSystem = true;
    bool m_dark = false;
    unsigned int m_lightAccent = mcast::platform::WindowsBlueAccent.light;
    unsigned int m_darkAccent = mcast::platform::WindowsBlueAccent.dark;
    bool m_visible = false;
    bool m_dismissRequested = false;
    bool m_fading = false;
    std::uint64_t m_token = 0;
    SplashWindow::StateCallback m_callback;
    std::chrono::steady_clock::time_point m_shownAt{};
    std::chrono::steady_clock::time_point m_fadeAt{};
};

SplashUi::~SplashUi()
{
    if (m_hwnd && IsWindow(m_hwnd))
        DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
    DestroyGfx(m_gfx);
    m_logo.Reset();
    m_wic.Reset();
    UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
}

void SplashUi::notify(SplashWindow::State state) const
{
    if (m_callback)
        m_callback(m_token, state);
}

bool SplashUi::initialize()
{
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(m_wic.GetAddressOf()))))
        return false;

    m_dark = SystemDarkMode();
    return createNativeWindow() && loadLogo();
}

bool SplashUi::createNativeWindow()
{
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &SplashUi::windowProc;
    windowClass.hInstance = instance;
    // Explicit wide resources so this file also builds in the Qt-free Shell,
    // where the Win32 UNICODE define is not supplied by Qt.
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // NOREDIRECTIONBITMAP + DirectComposition lets DWM draw the ordinary
    // window background behind the transparent D2D surface. TOOLWINDOW keeps
    // this splash out of the taskbar.
    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP
                          | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    const DWORD windowStyle = WS_OVERLAPPEDWINDOW
        & ~(WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    m_hwnd = CreateWindowExW(exStyle, kWindowClass, L"Media Cast",
                             windowStyle | WS_CLIPCHILDREN,
                             0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!m_hwnd)
        return false;

    resizeToMonitor();

    RECT client{};
    GetClientRect(m_hwnd, &client);
    const UINT width = static_cast<UINT>(client.right - client.left);
    const UINT height = static_cast<UINT>(client.bottom - client.top);
    if (!CreateGfx(m_gfx, m_hwnd, width, height, m_scale * 96.0f))
        return false;

    // Attach DirectComposition first, then opt the HWND into the DWM backdrop.
    // This matches the proven order in the reference graphics stack.
    applyTheme();
    return true;
}

void SplashUi::resizeToMonitor()
{
    m_scale = static_cast<float>(GetDpiForWindow(m_hwnd)) / 96.0f;
    if (m_scale <= 0.0f)
        m_scale = 1.0f;

    const int pixelWidth = static_cast<int>(std::lround(kWindowWidthDip * m_scale));
    const int pixelHeight = static_cast<int>(std::lround(kWindowHeightDip * m_scale));

    HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const RECT &work = info.rcWork;
    const int x = work.left + ((work.right - work.left) - pixelWidth) / 2;
    const int y = work.top + ((work.bottom - work.top) - pixelHeight) / 2;
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, pixelWidth, pixelHeight,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void SplashUi::applyTheme()
{
    if (!m_hwnd)
        return;

    const BOOL dark = m_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &dark, sizeof(dark));

    const int corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));

    // The splash deliberately does not use Mica. DWM draws the ordinary window
    // background; the transparent composition surface carries only the logo
    // and progress indicator.
    const int backdrop = DWMSBT_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                          &backdrop, sizeof(backdrop));

    const MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
}

bool SplashUi::loadLogo()
{
    HRSRC resource = FindResourceW(GetModuleHandleW(nullptr),
                                   MAKEINTRESOURCEW(kSplashLogoResource),
                                   MAKEINTRESOURCEW(10));
    if (!resource)
        return false;

    HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
    if (!loaded)
        return false;

    const DWORD size = SizeofResource(GetModuleHandleW(nullptr), resource);
    const auto *bytes = static_cast<const BYTE *>(LockResource(loaded));
    if (!bytes || size == 0)
        return false;

    ComPtr<IWICStream> stream;
    if (FAILED(m_wic->CreateStream(stream.GetAddressOf())))
        return false;
    if (FAILED(stream->InitializeFromMemory(
            const_cast<BYTE *>(bytes), size)))
        return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(m_wic->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
            decoder.GetAddressOf())))
        return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
        return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(m_wic->CreateFormatConverter(converter.GetAddressOf())))
        return false;
    if (FAILED(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
        return false;

    return SUCCEEDED(m_gfx.dc->CreateBitmapFromWicBitmap(
        converter.Get(), nullptr, m_logo.GetAddressOf()));
}

void SplashUi::requestShow(std::uint64_t token)
{
    if (!m_hwnd || !m_gfx.dc)
        return;

    m_token = token;
    if (m_visible) {
        m_dismissRequested = false;
        m_fading = false;
        m_shownAt = std::chrono::steady_clock::now();
        m_gfx.compositionEffect->SetOpacity(1.0f);
        m_gfx.composition->Commit();
        refreshSystemTheme();
        applyTheme();
        render();
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
                         | SWP_SHOWWINDOW);
        notify(SplashWindow::State::Shown);
        return;
    }

    // Read the system theme once more immediately before the first visible
    // frame. This closes the race while the Qt event loop has not started yet.
    refreshSystemTheme();
    m_visible = true;
    m_dismissRequested = false;
    m_fading = false;
    m_shownAt = std::chrono::steady_clock::now();
    applyTheme();
    render();
    ShowWindow(m_hwnd, SW_SHOW);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetForegroundWindow(m_hwnd);
    notify(SplashWindow::State::Shown);
}

void SplashUi::refreshSystemTheme()
{
    if (!m_followSystem)
        return;

    const bool dark = SystemDarkMode();
    if (dark == m_dark)
        return;
    m_dark = dark;
    applyTheme();
    if (m_visible)
        render();
}

void SplashUi::setTheme(bool followSystem, bool dark)
{
    m_followSystem = followSystem;
    const bool actualDark = followSystem ? SystemDarkMode() : dark;
    if (m_dark == actualDark) {
        applyTheme();
        return;
    }

    m_dark = actualDark;
    applyTheme();
    if (m_visible)
        render();
}

void SplashUi::setAccent(unsigned int lightRgb, unsigned int darkRgb)
{
    lightRgb &= 0x00ffffff;
    darkRgb &= 0x00ffffff;
    if (lightRgb == m_lightAccent && darkRgb == m_darkAccent)
        return;
    m_lightAccent = lightRgb;
    m_darkAccent = darkRgb;
    if (m_visible)
        render();
}

void SplashUi::bringToFront(std::uint64_t token)
{
    if (!m_hwnd || !m_visible || (token != 0 && token != m_token))
        return;
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetForegroundWindow(m_hwnd);
}

void SplashUi::requestDismiss(std::uint64_t token)
{
    if (!m_visible || m_dismissRequested
        || (token != 0 && token != m_token))
        return;

    m_dismissRequested = true;
    m_fadeAt = std::max(std::chrono::steady_clock::now(),
                        m_shownAt + kMinimumVisible);
}

void SplashUi::hideNow(std::uint64_t token)
{
    if (!m_hwnd || (token != 0 && token != m_token))
        return;
    const bool wasVisible = m_visible;
    ShowWindow(m_hwnd, SW_HIDE);
    m_visible = false;
    m_dismissRequested = false;
    m_fading = false;
    if (m_gfx.compositionEffect) {
        m_gfx.compositionEffect->SetOpacity(1.0f);
        m_gfx.composition->Commit();
    }
    applyTheme();
    m_shownAt = {};
    m_fadeAt = {};
    if (wasVisible)
        notify(SplashWindow::State::Hidden);
}

void SplashUi::tick()
{
    if (!m_visible)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_dismissRequested && !m_fading && now >= m_fadeAt)
        beginFade();

    if (m_fading) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_fadeAt);
        if (elapsed >= kFadeDuration) {
            finishFade();
            return;
        }
        const float progress = Clamp(
            static_cast<float>(elapsed.count())
                / static_cast<float>(kFadeDuration.count()),
            0.0f, 1.0f);
        m_gfx.compositionEffect->SetOpacity(1.0f - progress);
        m_gfx.composition->Commit();
        DwmFlush();
        return;
    }

    // The dedicated splash thread has no other work. Rendering directly lets
    // the flip-model swap chain's Present(1, 0) use the monitor VBlank as the
    // pacing clock instead of stacking WM_TIMER latency on top of it.
    render();
}

void SplashUi::beginFade()
{
    m_fading = true;
    m_fadeAt = std::chrono::steady_clock::now();

    // The DWM background itself cannot be faded through a composition visual.
    // For the last 150 ms, draw the matching solid Fluent base in the visual
    // and fade the complete content.
    const int backdrop = DWMSBT_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                          &backdrop, sizeof(backdrop));
    render();
    m_gfx.compositionEffect->SetOpacity(1.0f);
    m_gfx.composition->Commit();
}

void SplashUi::finishFade()
{
    ShowWindow(m_hwnd, SW_HIDE);
    m_gfx.compositionEffect->SetOpacity(1.0f);
    m_gfx.composition->Commit();
    m_visible = false;
    m_dismissRequested = false;
    m_fading = false;
    applyTheme();
    notify(SplashWindow::State::Hidden);
}

void SplashUi::render()
{
    if (!m_visible || !m_gfx.dc || !m_gfx.swapChain)
        return;

    m_gfx.dc->BeginDraw();
    m_gfx.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    // Transparent clear: DWM's ordinary window background remains visible.
    m_gfx.dc->Clear(D2D1::ColorF(0, 0.0f));
    drawFrame();
    const HRESULT drawResult = m_gfx.dc->EndDraw();
    if (SUCCEEDED(drawResult))
        m_gfx.swapChain->Present(1, 0);
    ValidateRect(m_hwnd, nullptr);
}

void SplashUi::drawFrame()
{
    if (m_fading) {
        const D2D1_COLOR_F base = m_dark
            ? ColorFromRgb(0x2b2b2b)
            : ColorFromRgb(0xffffff);
        ComPtr<ID2D1SolidColorBrush> baseBrush;
        if (SUCCEEDED(m_gfx.dc->CreateSolidColorBrush(
                base, baseBrush.GetAddressOf()))) {
            m_gfx.dc->FillRectangle(
                D2D1::RectF(0.0f, 0.0f, kWindowWidthDip, kWindowHeightDip),
                baseBrush.Get());
        }
    }

    if (m_logo) {
        const float left = (kWindowWidthDip - kLogoSizeDip) * 0.5f;
        const D2D1_RECT_F destination = D2D1::RectF(
            left, kLogoTopDip, left + kLogoSizeDip, kLogoTopDip + kLogoSizeDip);
        m_gfx.dc->DrawBitmap(m_logo.Get(), destination, 1.0f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    const float trackLeft = (kWindowWidthDip - kProgressWidthDip) * 0.5f;
    const D2D1_RECT_F track = D2D1::RectF(
        trackLeft, kProgressTopDip,
        trackLeft + kProgressWidthDip, kProgressTopDip + kProgressHeightDip);

    ComPtr<ID2D1SolidColorBrush> trackBrush;
    ComPtr<ID2D1SolidColorBrush> accentBrush;
    // Same track colours as FluentUI's FluProgressBar.
    const D2D1_COLOR_F trackColor = ColorFromRgb(m_dark ? 0x636363 : 0xd6d6d6);
    if (FAILED(m_gfx.dc->CreateSolidColorBrush(trackColor, trackBrush.GetAddressOf())))
        return;
    if (FAILED(m_gfx.dc->CreateSolidColorBrush(
            ColorFromRgb(m_dark ? m_darkAccent : m_lightAccent),
            accentBrush.GetAddressOf())))
        return;

    const float radius = kProgressHeightDip * 0.5f;
    m_gfx.dc->FillRoundedRectangle(
        D2D1::RoundedRect(track, radius, radius), trackBrush.Get());

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_shownAt);
    const double rawPhase = static_cast<double>(elapsed.count() % kAnimationPeriod.count())
        / static_cast<double>(kAnimationPeriod.count());
    const float phase = static_cast<float>(rawPhase);
    // This is the same shape and linear travel as FluProgressBar's
    // indeterminate content item: a 50%-wide rounded segment crossing the track.
    const float segmentWidth = kProgressWidthDip * 0.5f;
    const float travel = kProgressWidthDip + segmentWidth;
    const float segmentLeft = trackLeft - segmentWidth + travel * phase;
    const D2D1_RECT_F segment = D2D1::RectF(
        segmentLeft, kProgressTopDip,
        segmentLeft + segmentWidth, kProgressTopDip + kProgressHeightDip);

    m_gfx.dc->PushAxisAlignedClip(track, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    m_gfx.dc->FillRoundedRectangle(
        D2D1::RoundedRect(segment, radius, radius), accentBrush.Get());
    m_gfx.dc->PopAxisAlignedClip();
}

LRESULT CALLBACK SplashUi::windowProc(HWND hwnd, UINT message,
                                      WPARAM wParam, LPARAM lParam)
{
    SplashUi *self = nullptr;
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = static_cast<SplashUi *>(create->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SplashUi *>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self)
        return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handleMessage(hwnd, message, wParam, lParam);
}

LRESULT SplashUi::handleMessage(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(m_hwnd, &paint);
        render();
        EndPaint(m_hwnd, &paint);
        return 0;
    }
    case WM_NCHITTEST:
        // The splash is modal in effect: clicks must not fall through to the
        // partially constructed main window behind it.
        return HTCLIENT;
    case WM_NCCALCSIZE:
        // DWM still draws the ordinary framed-window background, but the
        // non-client area is removed from the visible result.
        if (wParam)
            return 0;
        break;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_SETTINGCHANGE:
        // Only follow the system in System mode. Forced Light/Dark must not be
        // overwritten by unrelated Windows setting changes.
        refreshSystemTheme();
        return 0;
    case WM_DPICHANGED: {
        const auto *suggested = reinterpret_cast<const RECT *>(lParam);
        SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        m_scale = static_cast<float>(HIWORD(wParam)) / 96.0f;
        RECT client{};
        GetClientRect(m_hwnd, &client);
        ResizeGfx(m_gfx, static_cast<UINT>(client.right - client.left),
                  static_cast<UINT>(client.bottom - client.top),
                  m_scale * 96.0f);
        render();
        return 0;
    }
    case WM_DESTROY:
        {
        const bool wasVisible = m_visible;
        DestroyGfx(m_gfx);
        m_logo.Reset();
        m_wic.Reset();
        m_visible = false;
        if (wasVisible)
            notify(SplashWindow::State::Hidden);
        return 0;
        }
    case WM_NCDESTROY:
        if (m_hwnd == hwnd)
            m_hwnd = nullptr;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

struct SplashWindow::Impl
{
    explicit Impl(SplashWindow::StateCallback callback)
        : m_callback(std::move(callback))
    {
        // Do this before Qt creates its first native window. Otherwise Windows
        // initially virtualises the splash at 125% and Qt later changes the
        // process awareness, making the already-visible window jump in size.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_ready)
            return;
        m_thread = std::thread([this] { threadMain(); });
        WaitForSingleObject(m_ready, 2000);
    }

    ~Impl()
    {
        if (m_threadId)
            PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        if (m_thread.joinable())
            m_thread.join();
        if (m_ready)
            CloseHandle(m_ready);
    }

    bool post(UINT message, WPARAM wParam = 0, LPARAM lParam = 0)
    {
        const DWORD threadId = m_threadId.load();
        return threadId
            && PostThreadMessageW(threadId, message, wParam, lParam);
    }

    void report(std::uint64_t token, SplashWindow::State state)
    {
        if (m_callback)
            m_callback(token, state);

        {
            std::scoped_lock lock(m_stateMutex);
            if (state == SplashWindow::State::Failed)
                m_failed = true;
            if (token == m_waitToken)
                m_observed = state;
        }
        m_stateChanged.notify_all();
    }

    bool showAndWait(std::uint64_t token)
    {
        {
            std::scoped_lock lock(m_stateMutex);
            if (m_failed)
                return false;
            m_waitToken = token;
            m_observed.reset();
        }
        if (!post(WM_MCAST_SPLASH_SHOW, static_cast<WPARAM>(token)))
            return false;

        std::unique_lock lock(m_stateMutex);
        return m_stateChanged.wait_for(
            lock, std::chrono::milliseconds(2000), [this, token] {
                return m_failed
                    || (m_waitToken == token && m_observed
                        && *m_observed == SplashWindow::State::Shown);
            })
            && !m_failed;
    }

    void threadMain()
    {
        const HRESULT comResult = CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        MSG queueProbe{};
        PeekMessageW(&queueProbe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        m_threadId = GetCurrentThreadId();
        SetEvent(m_ready);

        {
            SplashUi ui([this](
                std::uint64_t token, SplashWindow::State state) {
                report(token, state);
            });
            if (ui.initialize()) {
            auto handleMessage = [&](MSG &message) {
                switch (message.message) {
                case WM_MCAST_SPLASH_SHOW:
                    ui.requestShow(
                        static_cast<std::uint64_t>(message.wParam));
                    break;
                case WM_MCAST_SPLASH_THEME:
                    ui.setTheme(message.wParam != 0, message.lParam != 0);
                    break;
                case WM_MCAST_SPLASH_ACCENT:
                    ui.setAccent(static_cast<unsigned int>(message.wParam),
                                 static_cast<unsigned int>(message.lParam));
                    break;
                case WM_MCAST_SPLASH_FRONT:
                    ui.bringToFront(
                        static_cast<std::uint64_t>(message.wParam));
                    break;
                case WM_MCAST_SPLASH_DISMISS:
                    ui.requestDismiss(
                        static_cast<std::uint64_t>(message.wParam));
                    break;
                case WM_MCAST_SPLASH_HIDE:
                    ui.hideNow(
                        static_cast<std::uint64_t>(message.wParam));
                    break;
                default:
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                    break;
                }
                return message.message != WM_QUIT;
            };

            bool running = true;
            while (running) {
                MSG message{};
                if (!ui.isVisible()) {
                    if (GetMessageW(&message, nullptr, 0, 0) <= 0)
                        break;
                    running = handleMessage(message);
                    continue;
                }

                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    running = handleMessage(message);
                    if (!running)
                        break;
                }
                if (!running)
                    break;
                ui.tick();
            }
            }
            else {
                report(0, SplashWindow::State::Failed);
            }
        }

        m_threadId.store(0);
        if (SUCCEEDED(comResult))
            CoUninitialize();
    }

    std::thread m_thread;
    HANDLE m_ready = nullptr;
    std::atomic<DWORD> m_threadId{0};
    SplashWindow::StateCallback m_callback;
    std::mutex m_stateMutex;
    std::condition_variable m_stateChanged;
    std::uint64_t m_waitToken = 0;
    std::optional<SplashWindow::State> m_observed;
    bool m_failed = false;
};

SplashWindow::SplashWindow(StateCallback callback)
    : m_impl(std::make_unique<Impl>(std::move(callback)))
{
}

SplashWindow::~SplashWindow() = default;

bool SplashWindow::show(std::uint64_t token)
{
    return m_impl->showAndWait(token);
}

void SplashWindow::setTheme(bool followSystem, bool dark)
{
    m_impl->post(WM_MCAST_SPLASH_THEME,
                 followSystem ? 1 : 0, dark ? 1 : 0);
}

void SplashWindow::setAccent(unsigned int lightRgb, unsigned int darkRgb)
{
    m_impl->post(WM_MCAST_SPLASH_ACCENT,
                 static_cast<WPARAM>(lightRgb),
                 static_cast<LPARAM>(darkRgb));
}

void SplashWindow::bringToFront(std::uint64_t token)
{
    m_impl->post(
        WM_MCAST_SPLASH_FRONT, static_cast<WPARAM>(token));
}

void SplashWindow::dismiss(std::uint64_t token)
{
    m_impl->post(
        WM_MCAST_SPLASH_DISMISS, static_cast<WPARAM>(token));
}

void SplashWindow::hideImmediately(std::uint64_t token)
{
    m_impl->post(
        WM_MCAST_SPLASH_HIDE, static_cast<WPARAM>(token));
}
