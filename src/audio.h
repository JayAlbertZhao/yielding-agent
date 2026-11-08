// Mutes audio sessions of governed processes once, as they appear (event-driven).
#pragma once
#include <windows.h>

#include <functional>

namespace afk {

class AudioMuter {
public:
    using IsGoverned = std::function<bool(DWORD pid)>;
    void start(IsGoverned fn);  // spawns its own COM (MTA) thread
    void stop();

private:
    static DWORD WINAPI thread_main(LPVOID self);
    HANDLE thread_ = nullptr;
    HANDLE stop_event_ = nullptr;
    IsGoverned fn_;
};

}  // namespace afk
