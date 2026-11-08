#include "audio.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "platform.h"

namespace yielding {

namespace {

class Notifier : public IAudioSessionNotification {
public:
    explicit Notifier(HANDLE work) : work_(work) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioSessionNotification)) {
            *out = static_cast<IAudioSessionNotification*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    // Called on an audio-service thread: only queue the session, work happens on our thread.
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* ctl) override {
        push(ctl);
        return S_OK;
    }
    void push(IAudioSessionControl* ctl) {
        ctl->AddRef();
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push_back(ctl);
        SetEvent(work_);
    }
    std::vector<IAudioSessionControl*> take() {
        std::lock_guard<std::mutex> lk(mu_);
        return std::move(pending_);
    }

private:
    LONG ref_ = 1;
    HANDLE work_;
    std::mutex mu_;
    std::vector<IAudioSessionControl*> pending_;
};

}  // namespace

void AudioMuter::start(IsGoverned fn) {
    fn_ = std::move(fn);
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, thread_main, this, 0, nullptr);
}

void AudioMuter::stop() {
    if (!thread_) return;
    SetEvent(stop_event_);
    WaitForSingleObject(thread_, 5000);
    CloseHandle(thread_);
    CloseHandle(stop_event_);
    thread_ = stop_event_ = nullptr;
}

DWORD WINAPI AudioMuter::thread_main(LPVOID p) {
    auto* self = static_cast<AudioMuter*>(p);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE work = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto* notifier = new Notifier(work);
    std::vector<IAudioSessionManager2*> managers;

    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* devs = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   (void**)&en)) &&
        SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devs))) {
        UINT n = 0;
        devs->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* dev = nullptr;
            IAudioSessionManager2* mgr = nullptr;
            if (FAILED(devs->Item(i, &dev))) continue;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) &&
                SUCCEEDED(mgr->RegisterSessionNotification(notifier))) {
                managers.push_back(mgr);
                IAudioSessionEnumerator* se = nullptr;  // also required to start notifications
                if (SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
                    int count = 0;
                    se->GetCount(&count);
                    for (int k = 0; k < count; ++k) {
                        IAudioSessionControl* ctl = nullptr;
                        if (SUCCEEDED(se->GetSession(k, &ctl))) {
                            notifier->push(ctl);
                            ctl->Release();
                        }
                    }
                    se->Release();
                }
            } else if (mgr) {
                mgr->Release();
            }
            dev->Release();
        }
        devs->Release();
    }

    std::set<std::wstring> muted;
    HANDLE waits[2] = {self->stop_event_, work};
    while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        for (IAudioSessionControl* ctl : notifier->take()) {
            IAudioSessionControl2* c2 = nullptr;
            if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2))) {
                LPWSTR id = nullptr;
                DWORD pid = 0;
                if (SUCCEEDED(c2->GetSessionInstanceIdentifier(&id)) && id && !muted.count(id) &&
                    c2->GetProcessId(&pid) == S_OK && pid && self->fn_(pid)) {
                    ISimpleAudioVolume* vol = nullptr;
                    if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&vol))) {
                        if (SUCCEEDED(vol->SetMute(TRUE, nullptr))) {
                            muted.insert(id);
                            log_line(L"muted audio session of pid %lu", pid);
                        }
                        vol->Release();
                    }
                }
                if (id) CoTaskMemFree(id);
                c2->Release();
            }
            ctl->Release();
        }
    }

    for (auto* m : managers) {
        m->UnregisterSessionNotification(notifier);
        m->Release();
    }
    notifier->Release();
    if (en) en->Release();
    CloseHandle(work);
    CoUninitialize();
    return 0;
}

}  // namespace yielding
