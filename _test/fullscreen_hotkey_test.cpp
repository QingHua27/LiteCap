// Integration: independently expose key state and suppress WM_HOTKEY, matching
// the observed game failure without injecting input into the running game.
#include <windows.h>
#include <atomic>
static std::atomic<bool> testKeyDown{false};
static SHORT WINAPI TestAsyncKeyState(int vk) { return vk==VK_F8 && testKeyDown.load() ? (SHORT)0x8000 : 0; }
#define GetAsyncKeyState TestAsyncKeyState
#define wWinMain LiteCapTestWinMain
#include "../src/main.cpp"
#undef wWinMain
#undef GetAsyncKeyState
#include <filesystem>

static WNDPROC originalProc = nullptr;
static std::atomic<int> systemMessages{0};
static LRESULT CALLBACK SuppressSystemHotkey(HWND h,UINT m,WPARAM w,LPARAM l) {
    if (m == WM_HOTKEY) { ++systemMessages; return 0; }
    return CallWindowProcW(originalProc,h,m,w,l);
}

static std::wstring ButtonText(HWND main) {
    wchar_t text[128]{};GetWindowTextW(GetDlgItem(main,IDC_RECORD),text,128);return text;
}

template<class Predicate> static bool Wait(Predicate predicate,int milliseconds=4000) {
    auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(std::chrono::steady_clock::now()<end) {
        if(predicate())return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int main() {
    // This executable must be copied into an isolated directory with its own
    // settings.ini (single F8, silent160x120 region, close_action=exit).
    bool started=false,held=false,stopped=false;
    std::thread driver([&] {
        HWND main=nullptr;
        if(!Wait([&] {
            EnumWindows([](HWND h,LPARAM lp)->BOOL {
                DWORD pid=0;GetWindowThreadProcessId(h,&pid);
                wchar_t name[128]{};GetClassNameW(h,name,128);
                if(pid==GetCurrentProcessId() && wcscmp(name,kMainClass)==0) {
                    *reinterpret_cast<HWND*>(lp)=h;return FALSE;
                }
                return TRUE;
            },reinterpret_cast<LPARAM>(&main));
            return main && GetDlgItem(main,IDC_RECORD);
        }))return;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto idle=ButtonText(main);
        originalProc=(WNDPROC)GetWindowLongPtrW(main,GWLP_WNDPROC);
        SetWindowLongPtrW(main,GWLP_WNDPROC,(LONG_PTR)SuppressSystemHotkey);
        testKeyDown=true;
        PostMessageW(main,WM_HOTKEY,kHotkeyId,VK_F8<<16);
        started=Wait([&] {return ButtonText(main)!=idle;},1500);
        if(started) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            held=ButtonText(main)!=idle;
        }
        testKeyDown=false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(started) {
            testKeyDown=true;
            PostMessageW(main,WM_HOTKEY,kHotkeyId,VK_F8<<16);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            testKeyDown=false;
            stopped=Wait([&] {return ButtonText(main)==idle;},8000);
        }
        SetWindowLongPtrW(main,GWLP_WNDPROC,(LONG_PTR)originalProc);
        PostMessageW(main,WM_COMMAND,IDM_TRAY_EXIT,0);
    });
    LiteCapTestWinMain(GetModuleHandleW(nullptr),nullptr,nullptr,SW_HIDE);
    driver.join();
    printf("%s fallback starts when WM_HOTKEY is suppressed (%d messages suppressed)\n",started?"PASS":"FAIL",systemMessages.load());
    printf("%s holding the key does not toggle repeatedly\n",held?"PASS":"FAIL");
    printf("%s next press stops recording\n",stopped?"PASS":"FAIL");
    return started && held && stopped && systemMessages.load()>0 ? 0:1;
}
