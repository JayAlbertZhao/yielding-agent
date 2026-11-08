#include "platform.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <sstream>

namespace afk {

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring home_dir() {
    wchar_t buf[MAX_PATH];
    std::wstring dir;
    if (GetEnvironmentVariableW(L"AFK_GOVERNOR_HOME", buf, MAX_PATH)) {
        dir = buf;
    } else {
        GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        dir = std::wstring(buf) + L"\\afk-governor";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring path_in_home(const wchar_t* name) { return home_dir() + L"\\" + name; }

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    return p.substr(0, p.find_last_of(L'\\'));
}

void log_line(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    _snwprintf_s(line, _TRUNCATE, L"%04d-%02d-%02d %02d:%02d:%02d %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                 t.wMinute, t.wSecond, msg);
    std::wstring path = path_in_home(L"afk-governor.log");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (1u << 20))
        MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string s = utf8(line);
    DWORD w;
    WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr);
    CloseHandle(h);
}

static const char kDefaultConfig[] =
    "; afk-governor configuration. Restart the daemon after editing.\r\n"
    "[governor]\r\n"
    "; Minutes without keyboard/mouse input before switching to unattended mode.\r\n"
    "idle_minutes=30\r\n"
    "; Attended-mode budgets for all agent sessions combined (fractions of the machine).\r\n"
    "cpu_cap=0.5\r\n"
    "mem_cap=0.5\r\n"
    "gpu_util_cap=0.75\r\n"
    "vram_cap=0.75\r\n"
    "; Logical CPUs never used for pinning while others are available.\r\n"
    "avoid_cpus=0,1\r\n"
    "; Agent root processes; everything they start is governed.\r\n"
    "roots=claude.exe,codex.exe\r\n"
    "; Roots whose path contains one of these are ignored (desktop app UI processes).\r\n"
    "exclude_paths=\\WindowsApps\\\r\n"
    "; Only tool-call subtrees whose top process is one of these may be suspended.\r\n"
    "suspend_tops=bash.exe,sh.exe,cmd.exe,powershell.exe,pwsh.exe,codex-command-runner.exe\r\n"
    "; Mute audio of agent-started programs.\r\n"
    "mute=1\r\n"
    "; Minimize agent windows that take focus without user input (attended mode).\r\n"
    "focus_guard=1\r\n"
    "; Focus changes within this many ms of user input count as user-caused and are kept.\r\n"
    "focus_grace_ms=1000\r\n"
    "tick_ms=2000\r\n";

static std::vector<std::wstring> split_list(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstringstream in(s);
    std::wstring item;
    while (std::getline(in, item, L',')) {
        size_t a = item.find_first_not_of(L" \t"), b = item.find_last_not_of(L" \t");
        if (a != std::wstring::npos) out.push_back(lower(item.substr(a, b - a + 1)));
    }
    return out;
}

Config load_config() {
    std::wstring path = path_in_home(L"config.ini");
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        write_file_atomic(path, kDefaultConfig);
    Config c;
    auto str = [&](const wchar_t* key, const std::wstring& def) {
        wchar_t buf[1024];
        GetPrivateProfileStringW(L"governor", key, def.c_str(), buf, 1024, path.c_str());
        return std::wstring(buf);
    };
    auto num = [&](const wchar_t* key, double def) {
        std::wstring v = str(key, L"");
        return v.empty() ? def : _wtof(v.c_str());
    };
    c.idle_minutes = (int)num(L"idle_minutes", c.idle_minutes);
    c.cpu_cap = num(L"cpu_cap", c.cpu_cap);
    c.mem_cap = num(L"mem_cap", c.mem_cap);
    c.gpu_util_cap = num(L"gpu_util_cap", c.gpu_util_cap);
    c.vram_cap = num(L"vram_cap", c.vram_cap);
    c.tick_ms = (int)num(L"tick_ms", c.tick_ms);
    c.mute = num(L"mute", 1) != 0;
    c.focus_guard = num(L"focus_guard", 1) != 0;
    c.focus_grace_ms = (int)num(L"focus_grace_ms", c.focus_grace_ms);
    std::wstring v = str(L"avoid_cpus", L"0,1");
    c.avoid_cpus.clear();
    for (auto& s : split_list(v)) c.avoid_cpus.push_back(_wtoi(s.c_str()));
    if (!(v = str(L"roots", L"")).empty()) c.roots = split_list(v);
    if (!(v = str(L"exclude_paths", L"")).empty()) c.exclude_paths = split_list(v);
    if (!(v = str(L"suspend_tops", L"")).empty()) c.suspend_tops = split_list(v);
    return c;
}

bool read_file(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out->clear();
    char buf[4096];
    DWORD n;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) out->append(buf, n);
    CloseHandle(h);
    return true;
}

bool write_file_atomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

State read_state() {
    std::string text;
    if (!read_file(path_in_home(L"state.ini"), &text)) return State{};
    return parse_state(text);
}

void write_state(const State& s) { write_file_atomic(path_in_home(L"state.ini"), serialize_state(s)); }

bool daemon_alive() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
    if (!m) return false;
    CloseHandle(m);
    return true;
}

uint64_t process_create_time(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(h, &c, &e, &k, &u)) return 0;
    return (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}

std::vector<Governed> Scanner::scan(std::vector<ProcInfo>* procs) {
    procs->clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W pe{sizeof pe};
    std::map<uint32_t, Cached> next;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        ProcInfo p;
        p.pid = pe.th32ProcessID;
        p.ppid = pe.th32ParentProcessID;
        p.name = lower(pe.szExeFile);
        auto it = cache_.find(p.pid);
        Cached c{0, false, p.ppid, p.name};
        if (it != cache_.end() && it->second.ppid == p.ppid && it->second.name == p.name) {  // else PID reused
            c = it->second;
        } else if (p.pid > 4) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
            if (h) {
                c.create = process_create_time(h);
                for (auto& r : cfg_.roots) {
                    if (p.name != r) continue;
                    wchar_t path[MAX_PATH * 2];
                    DWORD len = MAX_PATH * 2;
                    c.root = true;
                    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
                        std::wstring lp = lower(path);
                        for (auto& ex : cfg_.exclude_paths)
                            if (lp.find(ex) != std::wstring::npos) c.root = false;
                    }
                }
                CloseHandle(h);
            }
        }
        p.create = c.create;
        p.is_root = c.root;
        next[p.pid] = c;
        procs->push_back(std::move(p));
    }
    CloseHandle(snap);
    cache_.swap(next);

    std::vector<Governed> gov = build_governed(*procs);
    DWORD self = GetCurrentProcessId();
    std::vector<Governed> out;
    for (auto& g : gov)
        if (g.pid != self) out.push_back(g);
    return out;
}

static HANDLE open_for_set(uint32_t pid) {
    return OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid);
}

void CpuCap::apply(const std::vector<Governed>& gov, const std::vector<ProcInfo>& procs, bool full) {
    if (!mask_) return;
    std::map<uint32_t, uint64_t> create;
    for (auto& p : procs) create[p.pid] = p.create;
    if (full) applied_.clear();
    for (auto& g : gov) {
        auto key = std::make_pair(g.pid, create[g.pid]);
        if (applied_.count(key)) continue;
        applied_.insert(key);
        HANDLE h = open_for_set(g.pid);
        if (!h) continue;
        DWORD_PTR pm = 0, sm = 0;
        if (GetProcessAffinityMask(h, &pm, &sm) && pm != (DWORD_PTR)mask_)
            SetProcessAffinityMask(h, (DWORD_PTR)(mask_ & sm));
        DWORD pc = GetPriorityClass(h);
        if (pc == NORMAL_PRIORITY_CLASS || pc == ABOVE_NORMAL_PRIORITY_CLASS) {
            orig_priority_.emplace(key, pc);
            SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
        }
        CloseHandle(h);
    }
}

void restore_cap_by_mask(uint64_t mask, const std::map<std::pair<uint32_t, uint64_t>, DWORD>& orig) {
    // Scans every process, not just the current agent trees: pinned processes orphaned by an exited
    // parent are no longer descendants but still carry the (random, hence distinctive) mask.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID <= 4) continue;
        HANDLE h = open_for_set(pe.th32ProcessID);
        if (!h) continue;
        DWORD_PTR pm = 0, sm = 0;
        if (GetProcessAffinityMask(h, &pm, &sm) && pm != sm && (pm == (DWORD_PTR)mask || pm == (DWORD_PTR)(mask & sm))) {
            SetProcessAffinityMask(h, sm);
            auto it = orig.find({pe.th32ProcessID, process_create_time(h)});
            if (it != orig.end()) SetPriorityClass(h, it->second);
            else if (GetPriorityClass(h) == BELOW_NORMAL_PRIORITY_CLASS) SetPriorityClass(h, NORMAL_PRIORITY_CLASS);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

void CpuCap::restore() {
    if (mask_) restore_cap_by_mask(mask_, orig_priority_);
    mask_ = 0;
    applied_.clear();
    orig_priority_.clear();
}

typedef LONG(NTAPI* NtProcFn)(HANDLE);

static NtProcFn nt_fn(const char* name) {
    return (NtProcFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name);
}

bool suspend_process(uint32_t pid, uint64_t create, bool trim) {
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA |
                               PROCESS_QUERY_INFORMATION,
                           FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    bool ok = false;
    if (!create || process_create_time(h) == create) {
        static NtProcFn fn = nt_fn("NtSuspendProcess");
        ok = fn && fn(h) >= 0;
        if (ok && trim) K32EmptyWorkingSet(h);
    }
    CloseHandle(h);
    return ok;
}

bool resume_process(uint32_t pid, uint64_t create) {
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return true;  // gone
    bool ok = true;       // a different process now owns the pid: ours is gone
    if (!create || process_create_time(h) == create) {
        static NtProcFn fn = nt_fn("NtResumeProcess");
        ok = fn && fn(h) >= 0;
    }
    CloseHandle(h);
    return ok;
}

uint64_t working_set(uint32_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    PROCESS_MEMORY_COUNTERS pmc{sizeof pmc};
    uint64_t ws = K32GetProcessMemoryInfo(h, &pmc, sizeof pmc) ? pmc.WorkingSetSize : 0;
    CloseHandle(h);
    return ws;
}

void recover_from_state(bool from_hook) {
    State s = read_state();
    if (!s.running && s.suspended.empty() && !s.cpu_mask) return;
    int resumed = 0;
    for (auto& e : s.suspended) resumed += resume_process(e.pid, e.create) ? 1 : 0;
    if (s.cpu_mask) restore_cap_by_mask(s.cpu_mask, {});
    log_line(L"recovered stale state: resumed %d of %d suspended processes, cpu cap 0x%llx undone", resumed,
             (int)s.suspended.size(), (unsigned long long)s.cpu_mask);
    if (from_hook && daemon_alive()) return;  // a daemon started meanwhile and owns the state file now
    State clean;
    clean.ov = s.ov;
    write_state(clean);
}

}  // namespace afk
