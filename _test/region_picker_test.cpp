// Exercise the real picker and its snapshot/paint/cleanup path.
#define wWinMain LiteCapTestWinMain
#include "../src/main.cpp"
#undef wWinMain
#include <dwmapi.h>
#include <cstdio>

static LRESULT CALLBACK BackdropProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        RECT left{0,0,r.right/2,r.bottom}, right{r.right/2,0,r.right,r.bottom};
        HBRUSH red = CreateSolidBrush(RGB(255,0,0)), green = CreateSolidBrush(RGB(0,255,0));
        FillRect(dc,&left,red); FillRect(dc,&right,green);
        DeleteObject(red); DeleteObject(green);
        EndPaint(h,&ps);
        return 0;
    }
    return DefWindowProcW(h,m,w,l);
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = GetModuleHandleW(nullptr);
    g_i18n.LoadBuiltin(g_inst);
    WNDCLASSW cls{};
    cls.hInstance = g_inst; cls.lpszClassName = L"LiteCapPickerBackdrop"; cls.lpfnWndProc=BackdropProc;
    RegisterClassW(&cls);
    cls.lpszClassName=kSelClass; cls.lpfnWndProc=SelProc;
    RegisterClassW(&cls);
    int x=GetSystemMetrics(SM_XVIRTUALSCREEN), y=GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width=GetSystemMetrics(SM_CXVIRTUALSCREEN), height=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HWND backdrop=CreateWindowExW(WS_EX_TOPMOST, L"LiteCapPickerBackdrop",L"",WS_POPUP,
                                  x,y,width,height,nullptr,nullptr,g_inst,nullptr);
    ShowWindow(backdrop,SW_SHOWNOACTIVATE);
    SetWindowPos(backdrop,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    UpdateWindow(backdrop);
    DwmFlush();
    Sleep(160); // Let the compositor present the fixture before PickRegion snapshots it.
    bool visible=false, bright=false, outside=false;
    std::thread driver([&] {
        HWND picker=nullptr;
        for (int i=0;i<100 && !picker;++i) {
            picker=FindWindowW(kSelClass,nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!picker) return;
        SendMessageW(picker,WM_PAINT,0,0);
        HDC dc=GetDC(picker);
        COLORREF initial=GetPixel(dc,width/4,height/4);
        visible=GetRValue(initial)>100 && GetRValue(initial)<240 && GetGValue(initial)<30;
        printf("initial background RGB=%u,%u,%u\n",GetRValue(initial),GetGValue(initial),GetBValue(initial));
        SendMessageW(picker,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(width/8,height/8));
        SendMessageW(picker,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(width*3/8,height*3/8));
        SendMessageW(picker,WM_PAINT,0,0);
        COLORREF inside=GetPixel(dc,width/4,height/4), dimmed=GetPixel(dc,width*3/4,height/4);
        bright=GetRValue(inside)>240 && GetGValue(inside)<30;
        outside=GetGValue(dimmed)>100 && GetGValue(dimmed)<240 && GetRValue(dimmed)<30;
        printf("selection RGB=%u,%u,%u outside RGB=%u,%u,%u\n",GetRValue(inside),GetGValue(inside),GetBValue(inside),GetRValue(dimmed),GetGValue(dimmed),GetBValue(dimmed));
        ReleaseDC(picker,dc);
        SendMessageW(picker,WM_LBUTTONUP,0,MAKELPARAM(width*3/8,height*3/8));
    });
    RECT region{};
    bool selected=PickRegion(backdrop,region);
    driver.join();
    bool buffersReleased=!g_sel.memDC && !g_sel.bmp && !g_sel.paintDC && !g_sel.paintBmp;
    DWORD objectsBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    bool cancels=true;
    for (int attempt=0;attempt<5;++attempt) {
        std::thread cancel([&] {
            HWND picker=nullptr;
            for (int i=0;i<100 && !picker;++i) {
                picker=FindWindowW(kSelClass,nullptr);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (picker) {
                SendMessageW(picker,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(10,10));
                if (attempt%2) SendMessageW(picker,WM_LBUTTONUP,0,MAKELPARAM(11,11));
                else PostMessageW(picker,WM_KEYDOWN,VK_ESCAPE,0);
            }
        });
        RECT ignored{1,2,3,4};
        cancels = !PickRegion(backdrop,ignored) && cancels;
        cancel.join();
        cancels = cancels && ignored.left==1 && ignored.top==2 && ignored.right==3 && ignored.bottom==4;
    }
    bool noLeak=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)<=objectsBefore;
    DestroyWindow(backdrop);
    bool coordinates=selected && region.left==x+width/8 && region.top==y+height/8 &&
                     region.right==x+width*3/8 && region.bottom==y+height*3/8;
    printf("%s readable dimmed background; %s bright selection; %s dimmed surroundings; %s coordinates\n",
           visible?"PASS":"FAIL",bright?"PASS":"FAIL",outside?"PASS":"FAIL",coordinates?"PASS":"FAIL");
    printf("%s buffer cleanup; %s drag cancellation/small selection; %s repeated-picker GDI cleanup\n",
           buffersReleased?"PASS":"FAIL",cancels?"PASS":"FAIL",noLeak?"PASS":"FAIL");
    return visible && bright && outside && coordinates && buffersReleased && cancels && noLeak ? 0 : 1;
}
