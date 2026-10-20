// Test-only Media Foundation boundary: keep real encoding but inject sink errors.
#include <windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <atomic>
std::atomic<int> encoderFailureMode{0}; // 1: WriteSample, 2: Finalize
class FailingSinkWriter : public IMFSinkWriter {
    IMFSinkWriter* inner_;
    std::atomic<ULONG> refs_{1};
    int mode_;
public:
    explicit FailingSinkWriter(IMFSinkWriter* inner):inner_(inner),mode_(encoderFailureMode.load()){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;
        *out=nullptr;
        if(id==__uuidof(IUnknown) || id==__uuidof(IMFSinkWriter)) {
            *out=static_cast<IMFSinkWriter*>(this);AddRef();return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left=--refs_;if(!left){inner_->Release();delete this;}return left;
    }
    HRESULT STDMETHODCALLTYPE AddStream(IMFMediaType* t,DWORD* i) override {return inner_->AddStream(t,i);}
    HRESULT STDMETHODCALLTYPE SetInputMediaType(DWORD i,IMFMediaType* t,IMFAttributes* a) override {return inner_->SetInputMediaType(i,t,a);}
    HRESULT STDMETHODCALLTYPE BeginWriting() override {return inner_->BeginWriting();}
    HRESULT STDMETHODCALLTYPE WriteSample(DWORD i,IMFSample* s) override {return mode_==1?HRESULT_FROM_WIN32(ERROR_DISK_FULL):inner_->WriteSample(i,s);}
    HRESULT STDMETHODCALLTYPE SendStreamTick(DWORD i,LONGLONG t) override {return inner_->SendStreamTick(i,t);}
    HRESULT STDMETHODCALLTYPE PlaceMarker(DWORD i,void* p) override {return inner_->PlaceMarker(i,p);}
    HRESULT STDMETHODCALLTYPE NotifyEndOfSegment(DWORD i) override {return inner_->NotifyEndOfSegment(i);}
    HRESULT STDMETHODCALLTYPE Flush(DWORD i) override {return inner_->Flush(i);}
    HRESULT STDMETHODCALLTYPE Finalize() override {
        HRESULT actual=inner_->Finalize();return mode_==2?E_FAIL:actual;
    }
    HRESULT STDMETHODCALLTYPE GetServiceForStream(DWORD i,REFGUID s,REFIID id,void** p) override {return inner_->GetServiceForStream(i,s,id,p);}
    HRESULT STDMETHODCALLTYPE GetStatistics(DWORD i,MF_SINK_WRITER_STATISTICS* s) override {return inner_->GetStatistics(i,s);}
};
static HRESULT WINAPI CreateTestSink(LPCWSTR url,IMFByteStream* stream,IMFAttributes* attributes,IMFSinkWriter** writer) {
    IMFSinkWriter* real=nullptr;
    HRESULT result=MFCreateSinkWriterFromURL(url,stream,attributes,&real);
    if(SUCCEEDED(result))*writer=new FailingSinkWriter(real);
    return result;
}
#define MFCreateSinkWriterFromURL CreateTestSink
#include "../src/recorder.cpp"
