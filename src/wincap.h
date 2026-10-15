// wincap.h - 窗口捕获（Windows.Graphics.Capture）
//
// 与 recorder.h 里的 GDI 抓屏并存：
//   GDI  ScreenCapture  —— 从屏幕 DC 抠一块矩形，被遮挡就录到挡住它的窗口
//   WGC  WindowCapture  —— 直接拿窗口自身的合成流，遮挡无影响
//
// 头文件里不出现任何 WinRT 类型，实现全放 .cpp 的 Pimpl 里，
// 这样只有 wincap.cpp 需要 C++/WinRT 与 C++20。
#pragma once
#include "common.h"
#include <string>
#include <vector>

// 可供选择的窗口
struct WinItem {
    HWND         hwnd = nullptr;
    std::wstring title;
    std::wstring exe;      // 进程可执行文件名，用来区分同标题的窗口
    RECT         rect = {};
};

// 枚举所有「值得录」的顶层窗口：可见、有标题、非工具窗口、不是本进程自己
std::vector<WinItem> EnumCaptureWindows();

// 按标题 + 进程名找回窗口（重启后 hwnd 会变，配置里存的是这两样）
HWND FindWindowByTitleAndExe(const std::wstring& title, const std::wstring& exe);

class WindowCapture {
public:
    WindowCapture() = default;
    ~WindowCapture();
    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    // 建立捕获会话。成功后 Width()/Height() 就是固定的输出尺寸。
    bool Init(HWND target);
    void Shutdown();

    // Init 失败时的 HRESULT。
    // 这个不能省：0x80070005 是目标窗口提权了（UIPI 限制，录不了），
    // 0x80004005 是当前系统/会话不支持 WGC —— 两者对用户的处置方式完全不同，
    // 只丢一句「初始化失败」等于没说。
    HRESULT LastError() const { return lastError_; }

    // 取最新一帧。返回 false 表示这次没有新帧（窗口静止或最小化）。
    bool Grab();

    // 有效数据指针，生命周期到下一次成功的 Grab 为止
    const BYTE* Bits() const { return front_.data(); }
    int  Width()  const { return w_; }
    int  Height() const { return h_; }
    bool TargetClosed() const;

private:
    bool InitSession(HWND target);
    void CloseSession();
    struct Impl;
    Impl* impl_ = nullptr;

    std::vector<BYTE> front_;   // 对外暴露的那一份
    std::vector<BYTE> source_;
    HWND rootTarget_ = nullptr;
    ULONGLONG lastResolve_ = 0;
    int  w_ = 0, h_ = 0;
    bool inited_ = false;
    HRESULT lastError_ = S_OK;
};
