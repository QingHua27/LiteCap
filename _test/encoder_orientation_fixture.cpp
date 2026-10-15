// Encode a known top-down BGRA pattern through each production encoder mode.
#include "recorder.h"
#include <mfapi.h>
#include <cstdio>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) return 1;
    std::filesystem::create_directories(argv[1]);
    const int w=160, h=120;
    std::vector<BYTE> image(w*h*4);
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        BYTE* p=image.data()+((size_t)y*w+x)*4;
        int quadrant=(y>=h/2?2:0)+(x>=w/2?1:0);
        p[0]=quadrant==2?255:0;
        p[1]=quadrant==1 || quadrant==3?255:0;
        p[2]=quadrant==0 || quadrant==3?255:0;
        p[3]=255;
    }
    const EncMode modes[]={EncMode::Auto,EncMode::Software,EncMode::Hardware};
    const wchar_t* names[]={L"auto",L"software",L"hardware"};
    bool ok=true;
    for (int i=0;i<3;++i) {
        Mp4Encoder encoder;
        std::wstring path=(std::filesystem::path(argv[1])/(std::wstring(names[i])+L".mp4")).wstring();
        if (!encoder.Open(path,w,h,10,1000000,modes[i])) {
            if (modes[i]==EncMode::Hardware) { puts("SKIP unavailable hardware mode"); continue; }
            ok=false; break;
        }
        for (int frame=0;frame<10;++frame)
            if (!encoder.WriteFrame(image.data(),frame*1000000LL,1000000LL)) ok=false;
        encoder.Close();
        wprintf(L"Encoded %s\n",names[i]);
    }
    MFShutdown(); CoUninitialize();
    return ok?0:1;
}
