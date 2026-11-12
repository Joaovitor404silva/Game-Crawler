// look: one frame of a game window (DXGI desktop duplication, cropped to the client rect), scaled, with an
// optional overlay (labels at projected world points), saved as JPEG via WIC. Windows only, no extra deps.
// The window must be visible on screen (duplication sees what the monitor shows).
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace look {

struct Label { float u, v; std::string text; COLORREF color; }; // u, v in 0..1 of the client area

inline HWND GameWindow(DWORD pid) {
    struct C { DWORD pid; HWND best; LONG area; } c{pid, nullptr, 0};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        C& c = *(C*)lp; DWORD p = 0; GetWindowThreadProcessId(h, &p);
        if (p != c.pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        RECT r; GetClientRect(h, &r); LONG a = (r.right - r.left) * (r.bottom - r.top);
        if (a > c.area) { c.area = a; c.best = h; }
        return TRUE;
    }, (LPARAM)&c);
    return c.best;
}

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

class Grabber {
public:
    ~Grabber() { Reset(); }
    // BGRA pixels of the client area of `hwnd` (w x h). Returns an error text or "".
    std::string Grab(HWND hwnd, std::vector<uint8_t>& px, int& w, int& h) {
        RECT cr; GetClientRect(hwnd, &cr);
        POINT tl{0, 0}; ClientToScreen(hwnd, &tl);
        w = cr.right; h = cr.bottom;
        if (w <= 0 || h <= 0 || IsIconic(hwnd)) return "game window is minimized";
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (mon != mon_ || !dup_) { std::string e = Open(mon); if (!e.empty()) return e; }
        // Take the next presented frame; the first frames of a new duplication can be empty (LastPresentTime 0).
        for (int attempt = 0; attempt < 10; ++attempt) {
            DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource* res = nullptr;
            HRESULT hr = dup_->AcquireNextFrame(100, &fi, &res);
            if (hr == DXGI_ERROR_ACCESS_LOST) { std::string e = Open(mon); if (!e.empty()) return e; continue; }
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) { if (have_) break; continue; } // screen unchanged: cached frame is current
            if (FAILED(hr)) return "AcquireNextFrame failed";
            bool got = false;
            if (fi.LastPresentTime.QuadPart) {
                ID3D11Texture2D* tex = nullptr; res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
                ctx_->CopyResource(full_, tex); have_ = got = true; Rel(tex);
            }
            Rel(res); dup_->ReleaseFrame();
            if (got) break;
        }
        if (!have_) return "no desktop frame yet";
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx_->Map(full_, 0, D3D11_MAP_READ, 0, &m))) return "map failed";
        const int x0 = tl.x - mon_rect_.left, y0 = tl.y - mon_rect_.top;
        px.assign((size_t)w * h * 4, 0);
        for (int y = 0; y < h; ++y) {
            const int sy = y0 + y; if (sy < 0 || sy >= mon_h_) continue;
            for (int x = 0; x < w; ++x) {
                const int sx = x0 + x; if (sx < 0 || sx >= mon_w_) continue;
                *(uint32_t*)&px[((size_t)y * w + x) * 4] = *(const uint32_t*)((const uint8_t*)m.pData + (size_t)sy * m.RowPitch + sx * 4);
            }
        }
        ctx_->Unmap(full_, 0);
        return "";
    }

private:
    ID3D11Device* dev_ = nullptr; ID3D11DeviceContext* ctx_ = nullptr; IDXGIOutputDuplication* dup_ = nullptr;
    ID3D11Texture2D* full_ = nullptr; HMONITOR mon_ = nullptr; RECT mon_rect_{}; int mon_w_ = 0, mon_h_ = 0; bool have_ = false;
    void Reset() { Rel(full_); Rel(dup_); Rel(ctx_); Rel(dev_); have_ = false; mon_ = nullptr; }
    std::string Open(HMONITOR mon) {
        Reset();
        IDXGIFactory1* f = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return "no DXGI";
        IDXGIAdapter1* ad = nullptr; IDXGIOutput* out = nullptr;
        for (UINT i = 0; !out && f->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
            IDXGIOutput* o = nullptr;
            for (UINT j = 0; ad->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC d; o->GetDesc(&d);
                if (d.Monitor == mon) { out = o; mon_rect_ = d.DesktopCoordinates; break; }
                Rel(o);
            }
            if (!out) Rel(ad);
        }
        Rel(f);
        if (!out) return "monitor of the game window not found";
        HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        Rel(ad);
        IDXGIOutput1* o1 = nullptr;
        if (SUCCEEDED(hr)) out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&o1);
        Rel(out);
        if (!o1) return "D3D11 device failed";
        hr = o1->DuplicateOutput(dev_, &dup_);
        Rel(o1);
        if (FAILED(hr)) return "DuplicateOutput failed (exclusive fullscreen or another duplicator?)";
        DXGI_OUTDUPL_DESC dd; dup_->GetDesc(&dd);
        mon_w_ = dd.ModeDesc.Width; mon_h_ = dd.ModeDesc.Height;
        D3D11_TEXTURE2D_DESC td{}; td.Width = mon_w_; td.Height = mon_h_; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &full_))) return "staging texture failed";
        mon_ = mon;
        return "";
    }
};

// Box-filter downscale of BGRA to width `ow` (keeps aspect), draws labels + a header line, writes JPEG.
inline std::string Save(const std::vector<uint8_t>& px, int w, int h, int ow, const std::vector<Label>& labels,
                        const std::string& header, const std::wstring& path, int& out_w, int& out_h) {
    if (ow <= 0 || ow > w) ow = w;
    const int oh = (int)((int64_t)h * ow / w);
    out_w = ow; out_h = oh;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = ow; bi.bmiHeader.biHeight = -oh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return "DIB failed"; }
    uint8_t* o = (uint8_t*)bits;
    for (int y = 0; y < oh; ++y) {
        const int y0 = (int)((int64_t)y * h / oh), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * h / oh));
        for (int x = 0; x < ow; ++x) {
            const int x0 = (int)((int64_t)x * w / ow), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * w / ow));
            uint32_t s[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; yy += 1 + (y1 - y0) / 4)
                for (int xx = x0; xx < x1; xx += 1 + (x1 - x0) / 4) {
                    const uint8_t* p = &px[((size_t)yy * w + xx) * 4]; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; ++n;
                }
            uint8_t* d = &o[((size_t)y * ow + x) * 4]; d[0] = (uint8_t)(s[0] / n); d[1] = (uint8_t)(s[1] / n); d[2] = (uint8_t)(s[2] / n); d[3] = 255;
        }
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    HFONT font = CreateFontA(std::max(12, oh / 40), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    HGDIOBJ oldf = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    auto text = [&](int x, int y, const std::string& t, COLORREF c) {
        SetTextColor(dc, RGB(0, 0, 0));
        for (int d = 0; d < 4; ++d) TextOutA(dc, x + (d & 1 ? 1 : -1), y + (d & 2 ? 1 : -1), t.c_str(), (int)t.size());
        SetTextColor(dc, c); TextOutA(dc, x, y, t.c_str(), (int)t.size());
    };
    for (auto& l : labels) {
        const int x = (int)(l.u * ow), y = (int)(l.v * oh);
        HBRUSH b = CreateSolidBrush(l.color); RECT r{x - 3, y - 3, x + 4, y + 4}; FillRect(dc, &r, b); DeleteObject(b);
        text(x + 6, y - oh / 80 - 6, l.text, l.color);
    }
    if (!header.empty()) text(8, 6, header, RGB(255, 255, 255));
    GdiFlush();
    SelectObject(dc, oldf); DeleteObject(font);
    // WIC JPEG
    std::string err;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr; IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* props = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) err = "WIC unavailable";
    if (err.empty() && (FAILED(fac->CreateStream(&st)) || FAILED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))) err = "cannot write image file";
    if (err.empty() && (FAILED(fac->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) || FAILED(enc->Initialize(st, WICBitmapEncoderNoCache)))) err = "jpeg encoder failed";
    if (err.empty() && FAILED(enc->CreateNewFrame(&fr, &props))) err = "jpeg frame failed";
    if (err.empty()) {
        PROPBAG2 opt{}; opt.pstrName = (LPOLESTR)L"ImageQuality"; VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.82f;
        props->Write(1, &opt, &v);
        fr->Initialize(props);
        fr->SetSize(ow, oh);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR; fr->SetPixelFormat(&fmt);
        std::vector<uint8_t> rgb((size_t)ow * oh * 3);
        for (size_t i = 0, n = (size_t)ow * oh; i < n; ++i) { rgb[i * 3] = o[i * 4]; rgb[i * 3 + 1] = o[i * 4 + 1]; rgb[i * 3 + 2] = o[i * 4 + 2]; }
        if (FAILED(fr->WritePixels(oh, ow * 3, (UINT)rgb.size(), rgb.data())) || FAILED(fr->Commit()) || FAILED(enc->Commit())) err = "jpeg write failed";
    }
    Rel(props); Rel(fr); Rel(enc); Rel(st); Rel(fac);
    SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc);
    return err;
}

} // namespace look
