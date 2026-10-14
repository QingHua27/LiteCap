#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <cstdio>
int main(){
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    printf("MFStartup hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        IMFAttributes* a=nullptr;
        hr = MFCreateAttributes(&a, 1);
        printf("MFCreateAttributes hr=0x%08lX\n", (unsigned long)hr);
        if(a) a->Release();
        MFShutdown();
    }
    printf("sizeof(void*)=%d\n",(int)sizeof(void*));
    return 0;
}
