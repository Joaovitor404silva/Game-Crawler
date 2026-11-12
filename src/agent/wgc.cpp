// Window capture with Windows.Graphics.Capture: the game window's own frames, also when other windows cover it,
// so the user can keep using the PC while an agent plays. Not for minimized windows (no frames then).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <string>
#include <vector>
#include "wgc.h"
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")

namespace WGC = winrt::Windows::Graphics::Capture;
namespace WD3D = winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::com_ptr;

namespace {
struct Cap {
    HWND hwnd = nullptr;
    com_ptr<ID3D11Device> dev; com_ptr<ID3D11DeviceContext> ctx; com_ptr<ID3D11Texture2D> staging;
    WD3D::IDirect3DDevice rtdev{nullptr};
    WGC::GraphicsCaptureItem item{nullptr};
    WGC::Direct3D11CaptureFramePool pool{nullptr};
    WGC::GraphicsCaptureSession session{nullptr};
    int w = 0, h = 0; bool have = false;
    winrt::Windows::Graphics::SizeInt32 size{};
    void Close() {
        if (session) session.Close();
        if (pool) pool.Close();
        session = nullptr; pool = nullptr; item = nullptr; staging = nullptr; have = false; hwnd = nullptr;
    }
};
Cap g;

std::string Open(HWND hwnd) {
    g.Close();
    static bool apartment = false;
    if (!apartment) { try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {} apartment = true; }
    if (!WGC::GraphicsCaptureSession::IsSupported()) return "Windows.Graphics.Capture not supported";
    if (!g.dev) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, g.dev.put(), nullptr, g.ctx.put())))
            return "D3D11 device failed";
        com_ptr<IDXGIDevice> dxgi = g.dev.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> insp;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()))) return "WinRT D3D device failed";
        g.rtdev = insp.as<WD3D::IDirect3DDevice>();
    }
    try {
        auto interop = winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        interop->CreateForWindow(hwnd, winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(g.item));
        g.size = g.item.Size();
        g.pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g.rtdev, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, g.size);
        g.session = g.pool.CreateCaptureSession(g.item);
        try { g.session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { g.session.IsBorderRequired(false); } catch (...) {} // no yellow frame (Win11)
        g.session.StartCapture();
    } catch (winrt::hresult_error const& e) {
        g.Close();
        return "window capture failed: " + winrt::to_string(e.message());
    }
    g.hwnd = hwnd;
    return "";
}

// Copies the newest frame (if any arrived) into the staging texture.
bool Pump() {
    WGC::Direct3D11CaptureFrame last{nullptr};
    for (auto f = g.pool.TryGetNextFrame(); f; f = g.pool.TryGetNextFrame()) last = f;
    if (!last) return false;
    auto cs = last.ContentSize();
    com_ptr<ID3D11Texture2D> tex;
    auto access = last.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    access->GetInterface(IID_PPV_ARGS(tex.put()));
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    if (!g.staging || (int)d.Width != g.w || (int)d.Height != g.h) {
        g.staging = nullptr; g.w = d.Width; g.h = d.Height;
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        if (FAILED(g.dev->CreateTexture2D(&d, nullptr, g.staging.put()))) return false;
    }
    g.ctx->CopyResource(g.staging.get(), tex.get());
    g.have = true;
    if (cs.Width != g.size.Width || cs.Height != g.size.Height) { // window resized
        g.size = cs;
        g.pool.Recreate(g.rtdev, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, g.size);
    }
    return true;
}
} // namespace

namespace wgc {
std::string Grab(HWND hwnd, std::vector<uint8_t>& px, int& w, int& h) {
    if (IsIconic(hwnd)) return "game window is minimized (restore it; it may stay behind other windows)";
    if (hwnd != g.hwnd || !g.session) { std::string e = Open(hwnd); if (!e.empty()) return e; }
    // Frames queued since the last grab are old (the 2-buffer pool fills right after it and then drops new
    // frames): drain them, then wait up to 200 ms for one that arrives after this call. A window whose content
    // does not change sends no new frames: then the drained one is current.
    Pump();
    bool fresh = false;
    for (int i = 0; i < 40 && !(fresh = Pump()); ++i) Sleep(5);
    if (!g.have) return "no frame from window capture yet";
    // The capture covers the DWM frame bounds; crop to the client area.
    RECT fb{}, cr{}; POINT tl{0, 0};
    DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof fb);
    GetClientRect(hwnd, &cr); ClientToScreen(hwnd, &tl);
    const int ox = tl.x - fb.left, oy = tl.y - fb.top;
    w = cr.right; h = cr.bottom;
    if (w <= 0 || h <= 0) return "empty client area";
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(g.staging.get(), 0, D3D11_MAP_READ, 0, &m))) return "map failed";
    px.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        const int sy = oy + y; if (sy < 0 || sy >= g.h) continue;
        const int x0 = std::max(0, -ox), x1 = std::min(w, g.w - ox);
        if (x1 > x0) memcpy(&px[((size_t)y * w + x0) * 4], (const uint8_t*)m.pData + (size_t)sy * m.RowPitch + (size_t)(ox + x0) * 4, (size_t)(x1 - x0) * 4);
    }
    g.ctx->Unmap(g.staging.get(), 0);
    (void)fresh;
    return "";
}
} // namespace wgc
