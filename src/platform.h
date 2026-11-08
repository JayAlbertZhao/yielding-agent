// Win32 plumbing shared by the daemon and the CLI.
#pragma once
#include <windows.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "model.h"

namespace yielding {

constexpr wchar_t kMutexName[] = L"Local\\yielding-agent-daemon";
constexpr wchar_t kWindowClass[] = L"YieldingAgentWnd";
constexpr UINT kMsgOverride = WM_APP + 1;  // wParam = (WPARAM)Override
constexpr wchar_t kVersion[] = L"0.1.0";

struct Config {
    int idle_minutes = 30;
    double cpu_cap = 0.5, mem_cap = 0.5, gpu_util_cap = 0.75, vram_cap = 0.75;
    std::vector<int> avoid_cpus{0, 1};
    std::vector<std::wstring> roots{L"claude.exe", L"codex.exe"};
    std::vector<std::wstring> exclude_paths{L"\\windowsapps\\"};
    std::vector<std::wstring> suspend_tops{L"bash.exe", L"sh.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe",
                                           L"codex-command-runner.exe"};
    bool mute = true, focus_guard = true;
    int focus_grace_ms = 1000;  // focus changes within this long after user input count as user-caused
    int tick_ms = 2000;
};

std::wstring home_dir();  // %YIELDING_AGENT_HOME% or %LOCALAPPDATA%\yielding-agent (created)
std::wstring path_in_home(const wchar_t* name);
std::wstring exe_dir();
std::wstring lower(std::wstring s);
std::string utf8(const std::wstring& w);
std::wstring wide(const std::string& s);

void log_line(const wchar_t* fmt, ...);
Config load_config();  // writes defaults when the file is missing

bool read_file(const std::wstring& path, std::string* out);
bool write_file_atomic(const std::wstring& path, const std::string& data);
State read_state();
void write_state(const State& s);
bool daemon_alive();

uint64_t process_create_time(HANDLE h);

// Process snapshot with cached creation times and root detection.
class Scanner {
public:
    explicit Scanner(const Config& cfg) : cfg_(cfg) {}
    // Fills procs and returns governed processes (self excluded).
    std::vector<Governed> scan(std::vector<ProcInfo>* procs);

private:
    struct Cached {
        uint64_t create;
        bool root;
        uint32_t ppid;  // with name: detects PID reuse between scans
        std::wstring name;
    };
    const Config& cfg_;
    std::map<uint32_t, Cached> cache_;
};

// Affinity + priority cap for governed processes.
class CpuCap {
public:
    void set_mask(uint64_t mask) { mask_ = mask; applied_.clear(); }
    uint64_t mask() const { return mask_; }
    // Applies to processes not yet handled; `full` re-checks all of them.
    void apply(const std::vector<Governed>& gov, const std::vector<ProcInfo>& procs, bool full);
    void restore();

private:
    uint64_t mask_ = 0;
    std::set<std::pair<uint32_t, uint64_t>> applied_;
    std::map<std::pair<uint32_t, uint64_t>, DWORD> orig_priority_;  // (pid, create) -> class
};

// Undo a cap: every process pinned exactly to `mask` gets the system mask back, and its priority
// restored from `orig` or raised from BELOW_NORMAL to NORMAL.
void restore_cap_by_mask(uint64_t mask, const std::map<std::pair<uint32_t, uint64_t>, DWORD>& orig);

bool suspend_process(uint32_t pid, uint64_t create, bool trim);
// False only if the process still exists and could not be resumed.
bool resume_process(uint32_t pid, uint64_t create);
uint64_t working_set(uint32_t pid);

// Resume persisted suspensions and undo the persisted cap. Used at daemon start and by hooks
// when the daemon died (from_hook: leave the state file alone if a daemon appeared meanwhile).
void recover_from_state(bool from_hook);

}  // namespace yielding
