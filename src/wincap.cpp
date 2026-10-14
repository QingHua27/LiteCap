// wincap.cpp - 窗口捕获实现（Windows.Graphics.Capture）
//
// 只有这个文件需要 C++/WinRT 和 C++20：头文件用 Pimpl 把 WinRT 类型全挡在外面，
// 其余源码继续待在 C++17 上。
//
// 和 GDI 抓屏的本质区别：
//   GDI  从屏幕 DC 上抠一块矩形 —— 窗口被别的窗口盖住，录到的就是盖住它的那个
//   WGC  直接订阅窗口自己的合成流 —— 遮挡无所谓，窗口拖到屏幕外也照录
#include "wincap.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

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
#pragma comment(lib, "dwmapi.lib")

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

// ================= 窗口枚举 =================
namespace {

std::wstring ProcNameOf(DWORD pid) {
    std::wstring name;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return name;
    wchar_t buf[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, buf, &n) && n > 0) {
        std::wstring full(buf, n);
        size_t s = full.find_last_of(L"\\/");
        name = (s == std::wstring::npos) ? full : full.substr(s + 1);
    }
    CloseHandle(p);
    return name;
}

// UWP / 打包应用会在系统里留下成堆的隐形窗口（ApplicationFrame 之类），
// 光看 WS_VISIBLE 是过滤不掉的，得问 DWM 这个窗口到底有没有真正显示。
bool IsCloaked(HWND h) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

struct EnumCtx {
    std::vector<WinItem>* out = nullptr;
    DWORD self = 0;
};

BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    EnumCtx* c = (EnumCtx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;          // 跳过 owned 弹窗
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (IsCloaked(h)) return TRUE;
    if (GetWindowTextLengthW(h) <= 0) return TRUE;

    RECT r = {};
    GetWindowRect(h, &r);
    int rw = r.right - r.left, rh = r.bottom - r.top;
    if (rw < 32 || rh < 32) {
        // 最小化的窗口 GetWindowRect 给的是 -32000 附近的图标位置，
        // 要拿还原后的矩形得问 GetWindowPlacement。
        WINDOWPLACEMENT wp = { sizeof(wp) };
        if (GetWindowPlacement(h, &wp)) {
            r = wp.rcNormalPosition;
            rw = r.right - r.left;
            rh = r.bottom - r.top;
        }
    }
    if (rw < 32 || rh < 32) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == c->self) return TRUE;                     // 别把自己列进去

    WinItem it;
    it.hwnd = h;
    it.rect = r;
    wchar_t t[512] = {};
    GetWindowTextW(h, t, 512);
    it.title = t;
    it.exe = ProcNameOf(pid);
    c->out->push_back(std::move(it));
    return TRUE;
}

}  // namespace

std::vector<WinItem> EnumCaptureWindows() {
    std::vector<WinItem> v;
    EnumCtx c;
    c.out = &v;
    c.self = GetCurrentProcessId();
    EnumWindows(EnumProc, (LPARAM)&c);

    // 按进程名分组排一下，同标题的窗口挨在一起，长列表里好找
    std::sort(v.begin(), v.end(), [](const WinItem& a, const WinItem& b) {
        if (a.exe != b.exe) return a.exe < b.exe;
        return a.title < b.title;
    });
    return v;
}

HWND FindWindowByTitleAndExe(const std::wstring& title, const std::wstring& exe) {
    if (title.empty() && exe.empty()) return nullptr;
    std::vector<WinItem> list = EnumCaptureWindows();
    for (const WinItem& w : list)
        if (w.title == title && w.exe == exe) return w.hwnd;
    // 标题经常被程序自己改（未保存加个 *、加个页码之类），
    // 精确匹配落空就退回按进程名找 —— 总比直接报"找不到窗口"强。
    if (!exe.empty())
        for (const WinItem& w : list)
            if (w.exe == exe) return w.hwnd;
    return nullptr;
}

// ================= 捕获会话 =================

// 回调跑在线程池线程上，只带 shared_ptr 进来，绝不碰 Impl ——
// 否则 Shutdown 之后还在飞的那一次回调会踩到已经释放的 D3D 对象。
struct WgcState {
    std::mutex         mtx;
    std::vector<BYTE>  latest;
    bool               hasNew = false;
    int                w = 0, h = 0;

    ID3D11Device*        d3d = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Texture2D*     staging = nullptr;
    UINT                 sw = 0, sh = 0;

    ~WgcState() {
        if (staging) staging->Release();
        if (ctx)     ctx->Release();
        if (d3d)     d3d->Release();
    }
};

struct WindowCapture::Impl {
    std::shared_ptr<WgcState> st = std::make_shared<WgcState>();
    HWND                      target = nullptr;
    GraphicsCaptureItem       item{ nullptr };
    Direct3D11CaptureFramePool pool{ nullptr };
    GraphicsCaptureSession    session{ nullptr };
    IDirect3DDevice           device{ nullptr };
    bool                      apartment = false;
};

namespace {

bool CopyFrameTo(const std::shared_ptr<WgcState>& st, const Direct3D11CaptureFrame& frame) {
    auto access = frame.Surface().as<
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> src;
    if (FAILED(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), src.put_void())))
        return false;

    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);
    if (sd.Width < 2 || sd.Height < 2) return false;

    // H.264 要求偶数尺寸，而 WGC 给的是窗口的真实尺寸，可能是奇数。
    // 在这里就裁掉多出来的那一行一列，后面编码器就能当普通紧凑 RGB 缓冲用。
    const UINT ew = sd.Width  & ~1u;
    const UINT eh = sd.Height & ~1u;

    if (!st->staging || st->sw != sd.Width || st->sh != sd.Height) {
        if (st->staging) { st->staging->Release(); st->staging = nullptr; }
        D3D11_TEXTURE2D_DESC dd = sd;
        dd.Usage          = D3D11_USAGE_STAGING;
        dd.BindFlags      = 0;
        dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dd.MiscFlags      = 0;
        dd.ArraySize      = 1;
        dd.MipLevels      = 1;
        if (FAILED(st->d3d->CreateTexture2D(&dd, nullptr, &st->staging))) return false;
        st->sw = sd.Width;
        st->sh = sd.Height;
    }

    st->ctx->CopyResource(st->staging, src.get());

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(st->ctx->Map(st->staging, 0, D3D11_MAP_READ, 0, &m))) return false;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        st->w = (int)ew;
        st->h = (int)eh;
        const size_t need = (size_t)ew * eh * 4;
        if (st->latest.size() != need) st->latest.resize(need);
        // WGC 给的已经是自上而下、行间距独立的 BGRA，逐行拷紧就行。
        // （GDI 那条路也是自上而下，所以后面编码器的翻转逻辑两边共用。）
        for (UINT y = 0; y < eh; ++y)
            memcpy(st->latest.data() + (size_t)y * ew * 4,
                   (const BYTE*)m.pData + (size_t)y * m.RowPitch,
                   (size_t)ew * 4);
        st->hasNew = true;
    }
    st->ctx->Unmap(st->staging, 0);
    return true;
}

}  // namespace

WindowCapture::~WindowCapture() { Shutdown(); }

bool WindowCapture::Init(HWND target) {
    Shutdown();
    lastError_ = S_OK;
    if (!target || !IsWindow(target)) { lastError_ = E_INVALIDARG; return false; }

    impl_ = new Impl();
    impl_->target = target;

    // 初始化失败统一走这里：记下 HRESULT 再清理。
    // 光返回 false 调用方只能报「初始化失败」，用户没法自救；
    // 带上错误码，至少能分清是「窗口提权了」还是「系统不支持 WGC」。
    auto bail = [this](HRESULT hr) {
        lastError_ = hr;
        Shutdown();
        return false;
    };

    // WGC 不是所有系统/会话都有（老版本 Win10、某些远程桌面会话、Windows Server）。
    // 先问一句，比让后面的 CreateForWindow 抛一个没头没脑的异常强。
    if (!GraphicsCaptureSession::IsSupported())
        return bail(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));

    try {
        // 录制线程是裸 std::thread，没有 apartment，而 WGC 的激活必须先有。
        // 主线程已经占了 STA，这里只能起 MTA。
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        impl_->apartment = true;
    } catch (winrt::hresult_error const&) {
        // RPC_E_CHANGED_MODE：本线程已经有别的 apartment 了，继续用即可
    }

    WgcState& st = *impl_->st;

    D3D_FEATURE_LEVEL fl = {};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &st.d3d, &fl, &st.ctx);
    if (FAILED(hr)) {
        // 没有独显 / 远程桌面会话下硬件设备可能起不来，WARP 是软件光栅化兜底
        if (st.d3d) { st.d3d->Release(); st.d3d = nullptr; }
        if (st.ctx) { st.ctx->Release(); st.ctx = nullptr; }
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &st.d3d, &fl, &st.ctx);
    }
    if (FAILED(hr)) return bail(hr);

    {
        winrt::com_ptr<IDXGIDevice> dxgi;
        HRESULT qhr = st.d3d->QueryInterface(winrt::guid_of<IDXGIDevice>(), dxgi.put_void());
        if (FAILED(qhr)) return bail(qhr);

        winrt::com_ptr<::IInspectable> insp;
        HRESULT dhr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
        if (FAILED(dhr)) return bail(dhr);
        impl_->device = insp.as<IDirect3DDevice>();
    }

    try {
        auto interop = winrt::get_activation_factory<GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();
        // 提权窗口会返回 E_ACCESSDENIED（0x80070005）—— 这是 UIPI 的正常限制，不是 bug。
        // 无边框窗口会返回 E_INVALIDARG（0x80070057）：实测 CreateForWindow 只接受
        // 带 WS_CAPTION 的窗口，WS_POPUP 和 WS_POPUP|WS_THICKFRAME 都被拒。
        // 调用方（Recorder）会据此退回 GDI 抓那块矩形。
        HRESULT chr = interop->CreateForWindow(target, winrt::guid_of<GraphicsCaptureItem>(),
                                               winrt::put_abi(impl_->item));
        if (FAILED(chr)) return bail(chr);

        const auto size = impl_->item.Size();
        w_ = (int)(size.Width  & ~1u);
        h_ = (int)(size.Height & ~1u);
        if (w_ < 16 || h_ < 16) return bail(E_INVALIDARG);

        impl_->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        impl_->session = impl_->pool.CreateCaptureSession(impl_->item);
        impl_->session.IsCursorCaptureEnabled(true);
        // 关掉那圈黄色捕获边框。这是 Win11 才有的属性，Win10 上会直接抛
        // 0x80004002（不支持此接口），关不掉就关不掉，不影响录制。
        try { impl_->session.IsBorderRequired(false); } catch (...) {}
    } catch (winrt::hresult_error const& e) {
        return bail(e.code());
    }

    auto s = impl_->st;
    impl_->pool.FrameArrived(
        [s](Direct3D11CaptureFramePool const& sender,
            winrt::Windows::Foundation::IInspectable const&) {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;
            try { CopyFrameTo(s, frame); } catch (...) {}
        });

    impl_->session.StartCapture();
    inited_ = true;
    return true;
}

void WindowCapture::Shutdown() {
    if (!impl_) return;

    try {
        if (impl_->pool)    impl_->pool.FrameArrived(nullptr);
        if (impl_->session) impl_->session.Close();
        if (impl_->pool)    impl_->pool.Close();
    } catch (...) {
        // 会话已经关掉时再关一次会抛，忽略
    }

    impl_->session = nullptr;
    impl_->pool    = nullptr;
    impl_->item    = nullptr;
    impl_->device  = nullptr;
    delete impl_;            // st 是 shared_ptr：还在飞的回调自己撑住 WgcState
    impl_ = nullptr;

    front_.clear();
    w_ = h_ = 0;
    inited_ = false;
}

bool WindowCapture::Grab() {
    if (!impl_ || !inited_) return false;

    WgcState& st = *impl_->st;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        if (st.hasNew) {
            // vector 交换是 O(1)，不用为了拿一帧去 memcpy 8MB
            std::swap(front_, st.latest);
            st.hasNew = false;
            w_ = st.w;
            h_ = st.h;
        }
    }
    // WGC 只在画面变化时给帧，窗口静止时这里会一直返回上一帧。
    // 这是故意的：编码器需要按目标帧率匀速喂帧，重复上一帧正是想要的效果。
    return !front_.empty();
}

bool WindowCapture::TargetClosed() const {
    if (!impl_) return true;
    return !IsWindow(impl_->target);
}
