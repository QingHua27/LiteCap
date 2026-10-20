// Full app integration; inspect real clipboard data and observe feedback requests.
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdio>
#include <vector>
#include <string>
static std::atomic<int> notices{0}, sounds{0};
extern std::atomic<int> encoderFailureMode;
static BOOL WINAPI ObserveNotify(DWORD action, PNOTIFYICONDATAW icon) {
    if (action == NIM_MODIFY && (icon->uFlags & NIF_INFO)) {
        ++notices; // Avoid presenting test notifications on the user's desktop.
        return TRUE;
    }
    return Shell_NotifyIconW(action, icon);
}
static BOOL WINAPI ObserveBeep(UINT) { ++sounds; return TRUE; }
#define Shell_NotifyIconW ObserveNotify
#define MessageBeep ObserveBeep
#define wWinMain LiteCapTestWinMain
#include "../src/main.cpp"
#undef wWinMain
#undef MessageBeep
#undef Shell_NotifyIconW

static int failures = 0;
static void Check(bool value, const char* name) {
    printf("%s %s\n", value ? "PASS" : "FAIL", name); fflush(stdout);
    if (!value) ++failures;
}
template<class F> static bool Wait(F condition, int ms=6000) {
    auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    do {
        if(condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while(std::chrono::steady_clock::now()<end);
    return false;
}
static std::wstring Text(HWND h) {
    wchar_t value[1024]{};GetWindowTextW(h,value,1024);return value;
}
static std::vector<std::wstring> ClipboardFiles() {
    std::vector<std::wstring> paths;
    if (!OpenClipboard(nullptr)) return paths;
    auto drop=(HDROP)GetClipboardData(CF_HDROP);
    if (drop) for (UINT i=0,n=DragQueryFileW(drop,0xffffffff,nullptr,0);i<n;++i) {
        UINT size=DragQueryFileW(drop,i,nullptr,0);
        std::wstring path(size+1,L'\0');DragQueryFileW(drop,i,path.data(),size+1);
        path.resize(size);paths.push_back(path);
    }
    CloseClipboard();return paths;
}
static DWORD ClipboardEffect() {
    DWORD effect=0;
    if (OpenClipboard(nullptr)) {
        auto data=GetClipboardData(RegisterClipboardFormatW(L"Preferred DropEffect"));
        if(data) {auto p=(DWORD*)GlobalLock(data);if(p){effect=*p;GlobalUnlock(data);}}
        CloseClipboard();
    }
    return effect;
}
static void ClipboardSentinel(HWND owner) {
    if(!OpenClipboard(owner)) return;
    auto data=GlobalAlloc(GMEM_MOVEABLE,32*sizeof(wchar_t));
    auto text=(wchar_t*)GlobalLock(data);wcscpy_s(text,32,L"clipboard sentinel");
    GlobalUnlock(data);EmptyClipboard();SetClipboardData(CF_UNICODETEXT,data);CloseClipboard();
}

int main() {
    DWORD mainThread=GetCurrentThreadId();
    std::thread driver([mainThread] {
        HWND main=nullptr;
        if(!Wait([&] {
            EnumWindows([](HWND h,LPARAM p)->BOOL {
                DWORD pid=0;GetWindowThreadProcessId(h,&pid);
                wchar_t name[80]{};GetClassNameW(h,name,80);
                if(pid==GetCurrentProcessId() && wcscmp(name,kMainClass)==0) {
                    *reinterpret_cast<HWND*>(p)=h;return FALSE;
                }
                return TRUE;
            },reinterpret_cast<LPARAM>(&main));
            return main && GetDlgItem(main,IDC_LIST) && GetDlgItem(main,IDC_RECORD);
        })) {
            Check(false,"main window ready");
            if(main)PostMessageW(main,WM_COMMAND,IDM_TRAY_EXIT,0);
            else PostThreadMessageW(mainThread,WM_QUIT,1,0);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        HWND list=GetDlgItem(main,IDC_LIST), record=GetDlgItem(main,IDC_RECORD);
        std::vector<std::wstring> expected={JoinPath(ExeDir(),L"视频 一.mp4"),JoinPath(ExeDir(),L"视频 二.mp4")};
        ClipboardSentinel(main);
        ListView_SetItemState(list,0,LVIS_SELECTED,LVIS_SELECTED);
        ListView_SetItemState(list,1,LVIS_SELECTED,LVIS_SELECTED);
        SendMessageW(main,WM_COMMAND,4006,0);
        Check(ClipboardFiles()==expected,"history copy publishes both Unicode video files");
        Check(ClipboardEffect()==DROPEFFECT_COPY,"file paste requests copy, preserving originals");
        Check(!CopyVideoFiles(main,{JoinPath(ExeDir(),L"missing.mp4")}) && ClipboardFiles()==expected,
              "missing video does not replace clipboard");
        Check(!CopyVideoFiles(main,{}) && ClipboardFiles()==expected,"empty selection preserves clipboard");
        bool locked=Wait([]{return OpenClipboard(nullptr)!=FALSE;},500);
        if(locked)SendMessageW(main,WM_COMMAND,4006,0);
        if(locked)CloseClipboard();
        Check(locked && ClipboardFiles()==expected,"busy clipboard leaves existing video data intact");
        SendMessageW(main,WM_COMMAND,4003,0);
        bool textOnly=false;
        if(OpenClipboard(nullptr)) {
            auto h=GetClipboardData(CF_UNICODETEXT);
            auto p=h?(wchar_t*)GlobalLock(h):nullptr;
            textOnly=p && std::wstring(p)==expected[0]+L"\r\n"+expected[1];
            if(p)GlobalUnlock(h);CloseClipboard();
        }
        Check(textOnly,"existing copy-path command still copies text");
        ClipboardSentinel(main);
        int beforeNotices=notices.load(),beforeSounds=sounds.load();
        const auto idle=Text(record);
        SendMessageW(main,WM_COMMAND,IDC_RECORD,0);
        Check(Wait([&]{return notices>beforeNotices && sounds>beforeSounds;}),"recording start requests visual and audible feedback");
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        Check(notices==beforeNotices+1 && sounds==beforeSounds+1,"progress ticks do not repeat start feedback");
        SendMessageW(main,WM_COMMAND,IDC_RECORD,0);
        Check(Wait([&]{return Text(record)==idle;}),"recording stops");
        auto videos=ClipboardFiles();
        Check(videos.size()==1 && videos[0].find(L"LiteCap_")!=std::wstring::npos && FileSize(videos[0])>1024,
              "successful recording automatically copies finalized video");
        Check(notices==beforeNotices+2 && sounds==beforeSounds+2,"recording end requests visual and audible feedback");
        // A worker-side encoder failure must not claim success or replace video clipboard data.
        beforeNotices=notices.load();beforeSounds=sounds.load();
        SetWindowTextW(GetDlgItem(main,IDC_DIR),expected[0].c_str());
        SendMessageW(main,WM_COMMAND,IDC_RECORD,0);
        HWND dialog=nullptr;
        Check(Wait([&]{dialog=FindWindowExW(nullptr,nullptr,L"#32770",L"LiteCap");return dialog && GetWindow(dialog,GW_OWNER)==main;}),"unwritable output reports recording failure");
        Check(notices==beforeNotices && sounds==beforeSounds,"failed recording has no success feedback");
        Check(ClipboardFiles()==videos,"failed recording preserves clipboard");
        if(dialog)PostMessageW(dialog,WM_CLOSE,0,0);
        Wait([&]{return IsWindowEnabled(main);});
        auto failedDir=JoinPath(ExeDir(),L"failed-output");
        CreateDirectoryW(failedDir.c_str(),nullptr);
        SetWindowTextW(GetDlgItem(main,IDC_DIR),failedDir.c_str());
        for(int mode : {1,2}) {
            encoderFailureMode=mode;
            int initialNotices=notices.load();
            SendMessageW(main,WM_COMMAND,IDC_RECORD,0);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if(mode==2)SendMessageW(main,WM_COMMAND,IDC_RECORD,0);
            dialog=nullptr;
            bool failed=Wait([&] {
                dialog=FindWindowExW(nullptr,nullptr,L"#32770",L"LiteCap");
                return dialog && GetWindow(dialog,GW_OWNER)==main;
            },1500);
            if(!failed && mode==1) {
                SendMessageW(main,WM_COMMAND,IDC_RECORD,0);
                Wait([&]{return Text(record)==idle;});
            }
            Check(failed,mode==1?"frame-write error reports failure":"finalization error reports failure");
            Check(ClipboardFiles()==videos,"sink failure preserves previous video clipboard");
            Check(notices==initialNotices+(mode==2?1:0),"sink failure has no successful completion feedback");
            if(failed)PostMessageW(dialog,WM_CLOSE,0,0);
            Wait([&]{return IsWindowEnabled(main);});
        }
        encoderFailureMode=0;
        beforeNotices=notices.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(1050));
        SetWindowTextW(GetDlgItem(main,IDC_DIR),ExeDir().c_str());
        SendMessageW(main,WM_COMMAND,IDC_RECORD,0);
        Check(Wait([&]{return notices>beforeNotices;}),"recording can restart after worker-side failure");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        PostMessageW(main,WM_COMMAND,IDM_TRAY_EXIT,0);
    });
    LiteCapTestWinMain(GetModuleHandleW(nullptr),nullptr,nullptr,SW_HIDE);
    driver.join();return failures?1:0;
}
