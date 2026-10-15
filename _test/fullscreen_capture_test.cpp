// Regression: a game creates an untitled fullscreen window while its launcher stays open.
#include "wincap.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static LRESULT CALLBACK TestProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HBRUSH brush = CreateSolidBrush(GetWindowLongPtrW(h, GWLP_USERDATA) ? RGB(0, 0, 255) : RGB(255, 0, 0));
        FillRect(dc, &r, brush);
        DeleteObject(brush);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool WaitColor(WindowCapture& capture, bool blue) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (capture.Grab()) {
            const BYTE* p = capture.Bits() + ((size_t)(capture.Height() / 2) * capture.Width() + capture.Width() / 2) * 4;
            if (blue ? (p[0] > 200 && p[2] < 40) : (p[2] > 200 && p[0] < 40)) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (capture.Grab()) {
        const BYTE* p = capture.Bits() + ((size_t)(capture.Height() / 2) * capture.Width() + capture.Width() / 2) * 4;
        printf("last center BGRA=%u,%u,%u,%u size=%dx%d\n", p[0],p[1],p[2],p[3],capture.Width(),capture.Height());
    }
    return false;
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc == 5 && std::string(argv[1]) == "--capture") {
        HWND target = reinterpret_cast<HWND>(std::strtoull(argv[2], nullptr, 16));
        WindowCapture capture;
        if (!capture.Init(target)) { printf("FAIL Init HRESULT=%08lX\n", capture.LastError()); return 1; }
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(argv[3]));
        int frames = 0;
        while (std::chrono::steady_clock::now() < until) {
            if (capture.Grab()) ++frames;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!frames) { puts("FAIL no frames"); return 1; }
        FILE* output = fopen(argv[4], "wb");
        if (!output) return 1;
        fwrite(capture.Bits(), 4, (size_t)capture.Width() * capture.Height(), output);
        fclose(output);
        printf("PASS production capture: %dx%d, %d frames, %s\n", capture.Width(), capture.Height(), frames, argv[4]);
        return 0;
    }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW cls = {};
    cls.lpfnWndProc = TestProc; cls.hInstance = inst; cls.lpszClassName = L"LiteCapFullscreenTest";
    RegisterClassW(&cls);
    HWND root = CreateWindowExW(0, cls.lpszClassName, L"Capture regression root",
                                WS_OVERLAPPEDWINDOW, 40, 40, 400, 400, nullptr, nullptr, inst, nullptr);
    ShowWindow(root, SW_SHOWNOACTIVATE);
    SetWindowPos(root, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    CreateWindowExW(0, cls.lpszClassName, L"embedded renderer", WS_CHILD | WS_VISIBLE,
                    0, 0, 20, 20, root, nullptr, inst, nullptr);
    MONITORINFO monitor = { sizeof(monitor) };
    GetMonitorInfoW(MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT r = monitor.rcMonitor;
    HWND sibling = CreateWindowExW(0, cls.lpszClassName, L"unrelated sibling", WS_POPUP,
                                  r.left, r.top, r.right-r.left, r.bottom-r.top, nullptr, nullptr, inst, nullptr);
    SetWindowLongPtrW(sibling, GWLP_USERDATA, 1);
    ShowWindow(sibling, SW_SHOWNOACTIVATE);
    SetWindowPos(sibling, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    HWND fullscreen = CreateWindowExW(0, cls.lpszClassName, L"", WS_POPUP,
                                     r.left, r.top, r.right-r.left, r.bottom-r.top, nullptr, nullptr, inst, nullptr);
    SetWindowLongPtrW(fullscreen, GWLP_USERDATA, 1);
    int result = 1;
    {
        WindowCapture capture;
        if (!capture.Init(root)) { printf("FAIL initial Init HRESULT=%08lX\n",capture.LastError()); goto done; }
        if (!WaitColor(capture, false)) { puts("FAIL initial window capture"); goto done; }
        const int width = capture.Width(), height = capture.Height();
        puts("PASS normal window capture ignores same-process fullscreen sibling");
        ShowWindow(fullscreen, SW_SHOWNOACTIVATE);
        SetWindowPos(fullscreen, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!WaitColor(capture, true)) { puts("FAIL did not follow untitled fullscreen render window"); goto done; }
        if (capture.Width() != width || capture.Height() != height) { puts("FAIL encoder canvas changed"); goto done; }
        puts("PASS recording follows fullscreen with a fixed encoder canvas");
        ShowWindow(fullscreen, SW_HIDE);
        if (!WaitColor(capture, false)) { puts("FAIL did not return to original window"); goto done; }
        puts("PASS return from fullscreen");
        SetWindowLongPtrW(root, GWLP_USERDATA, 1);
        SetWindowPos(root, nullptr, 0, 0, 600, 460, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(root, nullptr, FALSE);
        if (!WaitColor(capture, true) || capture.Width() != width || capture.Height() != height) {
            puts("FAIL resize changed encoder canvas"); goto done;
        }
        if (capture.Bits()[0] || capture.Bits()[1] || capture.Bits()[2]) {
            puts("FAIL resized content was not letterboxed"); goto done;
        }
        puts("PASS frame pool resize and aspect-preserving letterbox");
        capture.Shutdown();
        ShowWindow(fullscreen, SW_SHOWNOACTIVATE);
        SetWindowPos(fullscreen, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!capture.Init(root) || !WaitColor(capture, true)) { puts("FAIL start while already fullscreen"); goto done; }
        if (capture.Width() != (r.right-r.left) || capture.Height() != (r.bottom-r.top)) {
            puts("FAIL fullscreen initial dimensions"); goto done;
        }
        puts("PASS start after entering fullscreen");
        result = 0;
    }
done:
    DestroyWindow(fullscreen); DestroyWindow(sibling); DestroyWindow(root);
    return result;
}
