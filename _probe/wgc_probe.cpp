// wgc_probe.cpp —— 验证 Windows.Graphics.Capture 能否抓到指定窗口
//
// 这是方案 C 的最高风险点，所以先单独跑通再动主程序。
// 用法：
//   wgc_probe.exe <hwnd十六进制> [秒数] [输出.raw]
//   wgc_probe.exe --find <标题关键字> [秒数] [输出.raw]
//
// 输出：窗口尺寸、实际帧率、像素统计（用来判断是不是抓到全黑）。
// 同时把最后一帧以 BGRA 原始数据写出，交给 Python 转 PNG 肉眼确认。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

// ---------- 找窗口 ----------
struct FindCtx { const wchar_t* needle; HWND hit; };

static BOOL CALLBACK FindProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;   // 跳过 owned 弹窗
    wchar_t title[512] = {};
    GetWindowTextW(h, title, 512);
    if (title[0] == 0) return TRUE;
    if (wcsstr(title, c->needle)) { c->hit = h; return FALSE; }
    return TRUE;
}

static HWND FindByTitle(const wchar_t* needle) {
    FindCtx c{ needle, nullptr };
    EnumWindows(FindProc, (LPARAM)&c);
    return c.hit;
}

// ---------- 抓帧 ----------
struct Grabber {
    std::mutex mtx;
    std::vector<uint8_t> last;   // BGRA
    int w = 0, h = 0;
    long long frames = 0;
    bool first = true;
};

static void CopyFrame(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                      const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& frame,
                      Grabber& g) {
    auto access = frame.Surface().as<
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> src;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(),
                                              src.put_void()));

    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);

    if (g.first) {
        D3D11_TEXTURE2D_DESC dd = sd;
        dd.Usage = D3D11_USAGE_STAGING;
        dd.BindFlags = 0;
        dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dd.MiscFlags = 0;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(dev->CreateTexture2D(&dd, nullptr, staging.put()));
        // 把 staging 存在 frame 的生命周期外不方便，这里每次新建即可（POC 够用）
        ctx->CopyResource(staging.get(), src.get());

        D3D11_MAPPED_SUBRESOURCE m = {};
        winrt::check_hresult(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m));

        std::lock_guard<std::mutex> lk(g.mtx);
        g.w = (int)sd.Width;
        g.h = (int)sd.Height;
        g.last.resize((size_t)sd.Width * sd.Height * 4);
        for (UINT y = 0; y < sd.Height; ++y) {
            memcpy(g.last.data() + (size_t)y * sd.Width * 4,
                   (const uint8_t*)m.pData + (size_t)y * m.RowPitch,
                   (size_t)sd.Width * 4);
        }
        ctx->Unmap(staging.get(), 0);
        g.first = false;
    }
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        g.frames++;
    }
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);

    if (argc < 2) {
        wprintf(L"用法: wgc_probe.exe <hwnd十六进制> [秒数] [输出.raw]\n");
        wprintf(L"      wgc_probe.exe --find <标题关键字> [秒数] [输出.raw]\n");
        return 2;
    }

    HWND target = nullptr;
    int argi = 1;
    if (wcscmp(argv[1], L"--find") == 0 && argc >= 3) {
        target = FindByTitle(argv[2]);
        argi = 3;
    } else {
        target = (HWND)(ULONG_PTR)wcstoull(argv[1], nullptr, 16);
        argi = 2;
    }
    double seconds = (argc > argi) ? _wtof(argv[argi]) : 3.0;
    const wchar_t* outPath = (argc > argi + 1) ? argv[argi + 1] : nullptr;

    if (!target || !IsWindow(target)) { wprintf(L"[x] 找不到窗口\n"); return 1; }
    wchar_t title[512] = {};
    GetWindowTextW(target, title, 512);
    wprintf(L"[*] 目标窗口: %p  \"%s\"\n", (void*)target, title);

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // D3D11 设备
    winrt::com_ptr<ID3D11Device> d3dDev;
    winrt::com_ptr<ID3D11DeviceContext> d3dCtx;
    D3D_FEATURE_LEVEL fl = {};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, d3dDev.put(), &fl, d3dCtx.put());
    if (FAILED(hr)) {
        wprintf(L"[!] 硬件设备失败(0x%08X)，改用 WARP\n", (unsigned)hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, d3dDev.put(), &fl, d3dCtx.put());
    }
    if (FAILED(hr)) { wprintf(L"[x] D3D11CreateDevice 失败 0x%08X\n", (unsigned)hr); return 1; }
    wprintf(L"[ok] D3D11 设备就绪 (feature level 0x%04X)\n", (unsigned)fl);

    winrt::com_ptr<IDXGIDevice> dxgiDev = d3dDev.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> insp;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), insp.put()));
    IDirect3DDevice device = insp.as<IDirect3DDevice>();

    // 窗口 -> CaptureItem
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem,
                                                 IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    hr = interop->CreateForWindow(target,
                                  winrt::guid_of<GraphicsCaptureItem>(),
                                  winrt::put_abi(item));
    if (FAILED(hr)) {
        wprintf(L"[x] CreateForWindow 失败 0x%08X\n", (unsigned)hr);
        return 1;
    }
    auto size = item.Size();
    wprintf(L"[ok] CaptureItem 尺寸 = %d x %d\n", size.Width, size.Height);

    Grabber g;
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    auto session = pool.CreateCaptureSession(item);

    session.IsCursorCaptureEnabled(true);
    // 关掉那圈黄色边框。老系统上没有这个属性，失败就算了。
    try {
        session.IsBorderRequired(false);
        wprintf(L"[ok] 已关闭捕获边框\n");
    } catch (winrt::hresult_error const& e) {
        wprintf(L"[!] 关闭捕获边框失败: 0x%08X %s\n",
                (unsigned)e.code().value, e.message().c_str());
    }

    pool.FrameArrived([&](Direct3D11CaptureFramePool const& sender,
                          winrt::Windows::Foundation::IInspectable const&) {
        auto frame = sender.TryGetNextFrame();
        if (frame) {
            try {
                CopyFrame(d3dDev.get(), d3dCtx.get(), frame, g);
            } catch (...) {}
        }
    });

    session.StartCapture();
    wprintf(L"[*] 采集 %g 秒...\n", seconds);
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(seconds * 1000)));
    auto t1 = std::chrono::steady_clock::now();
    session.Close();

    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    long long n = 0;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        n = g.frames;
    }
    wprintf(L"[ok] 收到 %lld 帧 / %.2f 秒 = %.1f fps\n", n, elapsed,
            elapsed > 0 ? n / elapsed : 0.0);

    if (g.last.empty()) { wprintf(L"[x] 一帧都没拿到\n"); return 1; }

    // 像素统计：判断是不是全黑 / 单色（部分程序会给出黑帧）
    size_t uniq = 0;
    long long sum = 0;
    uint8_t mn = 255, mx = 0;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        for (size_t i = 0; i < g.last.size(); i += 4 * 97) {   // 抽样
            uint8_t v = g.last[i];
            sum += v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        uniq = (size_t)(mx - mn);
    }
    size_t samples = g.last.size() / (4 * 97) + 1;
    wprintf(L"[*] 像素抽样: 平均=%lld 最小=%u 最大=%u 极差=%zu\n",
            sum / (long long)samples, mn, mx, uniq);
    if (mx - mn < 4) wprintf(L"[!] 画面几乎纯色，可能是黑屏\n");

    if (outPath) {
        FILE* f = _wfopen(outPath, L"wb");
        if (f) {
            std::lock_guard<std::mutex> lk(g.mtx);
            fwrite(g.last.data(), 1, g.last.size(), f);
            fclose(f);
            wprintf(L"[ok] 已写出 %s  (%d x %d BGRA)\n", outPath, g.w, g.h);
        }
    }
    return 0;
}
