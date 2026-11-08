// The resident governor: presence detection, CPU pinning, suspension, focus guard, tray.
#include <windows.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include <random>

#include "audio.h"
#include "gpu.h"
#include "platform.h"

namespace afk {

namespace {

enum { kTimerId = 1, kTrayMsg = WM_APP + 2, kCmdAuto = 101, kCmdAttended, kCmdUnattended, kCmdFolder, kCmdExit };
constexpr uint64_t kFullRecheckMs = 30000, kMinHoldMs = 30000, kNoNvmlUtilScanMs = 10000;
constexpr double kResumeRatio = 0.9;

struct Held {
    uint64_t top_create = 0;
    std::vector<SuspendedEntry> members;  // only those actually suspended
    uint64_t at = 0;
    double saved[kResCount] = {0, 0, 0};
};

struct Daemon {
    Config cfg;
    Scanner scanner{cfg};
    CpuCap cap;
    Gpu gpu;
    AudioMuter audio;
    Mode mode = Mode::Unattended;
    Override ov = Override::Auto;
    bool locked = false;
    std::mt19937_64 rng{std::random_device{}()};
    std::map<uint32_t, Held> held;  // by top pid
    std::vector<Governed> gov;
    std::vector<ProcInfo> procs;
    uint64_t last_govern = 0, last_full = 0, last_gpu_scan = 0, last_state_write = 0, total_ram = 0;
    int ncpu = 0;
    HWND hwnd = nullptr;
    HWINEVENTHOOK fg_hook = nullptr;
    UINT taskbar_created = 0;
    std::string last_state;
};

Daemon* g_d = nullptr;

uint64_t idle_ms() {
    LASTINPUTINFO li{sizeof li};
    GetLastInputInfo(&li);
    return (DWORD)(GetTickCount() - li.dwTime);
}

void save_state(Daemon& d, bool running) {
    State s;
    s.running = running;
    s.mode = d.mode;
    s.ov = d.ov;
    if (running && d.mode == Mode::Attended) {
        s.cpu_mask = d.cap.mask();
        s.cpu_count = popcount64(s.cpu_mask);
        s.mem_cap = (uint64_t)(d.total_ram * d.cfg.mem_cap);
        s.vram_cap = (uint64_t)(d.gpu.vram_total() * d.cfg.vram_cap);
        s.gpu_util_cap = (int)(d.cfg.gpu_util_cap * 100 + 0.5);
    }
    for (auto& [top, h] : d.held) s.suspended.insert(s.suspended.end(), h.members.begin(), h.members.end());
    std::string text = serialize_state(s);
    uint64_t now = GetTickCount64();
    // Periodic rewrite too: a hook that wrongly judged the daemon dead may have overwritten the file.
    if (text != d.last_state || now - d.last_state_write >= kFullRecheckMs) {
        write_state(s);
        d.last_state = text;
        d.last_state_write = now;
    }
}

HICON make_icon(COLORREF color) {
    int sz = GetSystemMetrics(SM_CXSMICON);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color_bmp = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    double c = (sz - 1) / 2.0, r = sz * 0.42;
    auto* px = (DWORD*)bits;
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            double dist = sqrt((x - c) * (x - c) + (y - c) * (y - c));
            double a = dist <= r - 0.5 ? 1.0 : dist >= r + 0.5 ? 0.0 : r + 0.5 - dist;
            BYTE A = (BYTE)(a * 255);
            // premultiplied BGRA
            px[y * sz + x] = (DWORD(A) << 24) | (DWORD(GetRValue(color) * a) << 16) |
                             (DWORD(GetGValue(color) * a) << 8) | DWORD(GetBValue(color) * a);
        }
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color_bmp};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color_bmp);
    return icon;
}

void update_tray(Daemon& d, DWORD msg) {
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = d.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = kTrayMsg;
    bool on = d.mode == Mode::Attended;
    nid.hIcon = make_icon(on ? RGB(0xF0, 0x9A, 0x1F) : RGB(0x8A, 0x8F, 0x98));
    _snwprintf_s(nid.szTip, _TRUNCATE, L"afk-governor: %s%s", on ? L"attended, agents limited" : L"unattended, no limits",
                 d.ov == Override::Auto ? L"" : L" (forced)");
    Shell_NotifyIconW(msg, &nid);
    DestroyIcon(nid.hIcon);
}

void resume_group(Daemon& d, uint32_t top) {
    auto it = d.held.find(top);
    if (it == d.held.end()) return;
    std::vector<SuspendedEntry> failed;
    for (auto& m : it->second.members)
        if (!resume_process(m.pid, m.create)) failed.push_back(m);
    log_line(L"resumed group %lu (%d of %d processes)", top, (int)(it->second.members.size() - failed.size()),
             (int)it->second.members.size());
    if (failed.empty()) d.held.erase(it);
    else it->second.members = failed;  // retried next time; stays in the state file for recovery
}

void enter_mode(Daemon& d, Mode m) {
    d.mode = m;
    d.gov = d.scanner.scan(&d.procs);
    if (m == Mode::Attended) {
        d.cap.set_mask(pick_cpu_mask(d.ncpu, d.cfg.cpu_cap, d.cfg.avoid_cpus, d.rng));
        save_state(d, true);  // persist the mask before pinning anything, for crash recovery
        d.cap.apply(d.gov, d.procs, true);
        d.last_full = d.last_govern = GetTickCount64();
        log_line(L"attended: agents pinned to cpu mask 0x%llx (%d cpus)", (unsigned long long)d.cap.mask(),
                 popcount64(d.cap.mask()));
    } else {
        std::vector<uint32_t> tops;
        for (auto& [top, h] : d.held) tops.push_back(top);
        for (uint32_t top : tops) resume_group(d, top);
        d.cap.restore();
        d.gpu.close_query();
        log_line(L"unattended: limits lifted");
    }
    save_state(d, true);
    update_tray(d, NIM_MODIFY);
}

void govern(Daemon& d) {
    uint64_t now = GetTickCount64();
    d.last_govern = now;
    d.gov = d.scanner.scan(&d.procs);
    bool full = now - d.last_full >= kFullRecheckMs;
    if (full) d.last_full = now;
    d.cap.apply(d.gov, d.procs, full);

    std::map<uint32_t, const ProcInfo*> by_pid;
    for (auto& p : d.procs) by_pid[p.pid] = &p;
    std::vector<uint32_t> gone;  // top process exited (e.g. killed by a tool timeout): free the rest
    for (auto& [top, h] : d.held) {
        auto p = by_pid.find(top);
        if (p == by_pid.end() || p->second->create != h.top_create) gone.push_back(top);
    }
    for (uint32_t top : gone) {
        resume_group(d, top);
        d.held.erase(top);  // leftover failures belong to a dead group
    }

    std::map<uint32_t, Group> groups;
    for (auto& g : d.gov) {
        Group& gr = groups[g.top];
        gr.top = g.top;
        if (g.top != g.root && by_pid.count(g.top))
            for (auto& t : d.cfg.suspend_tops)
                if (by_pid[g.top]->name == t) gr.suspendable = true;
    }
    for (auto& [top, h] : d.held) {
        Group& gr = groups[top];
        gr.top = top;
        gr.suspendable = true;
        gr.suspended = true;
        gr.suspended_at_ms = h.at;
        for (int k = 0; k < kResCount; ++k) gr.saved[k] = h.saved[k];
    }

    // Lazy measurement: system-wide numbers bound the agents' share; per-process numbers are
    // read only when that bound exceeds a cap or a suspended group awaits a resume decision.
    bool any_held = !d.held.empty();
    Group bound;  // non-suspendable stand-in carrying system-wide usage
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    double caps[kResCount] = {d.total_ram * d.cfg.mem_cap, -1, -1};
    uint64_t used = ms.ullTotalPhys - ms.ullAvailPhys;
    if (any_held || used > caps[kMem]) {
        for (auto& g : d.gov) groups[g.top].usage[kMem] += (double)working_set(g.pid);
    } else {
        bound.usage[kMem] = (double)used;
    }
    if (d.gpu.present()) {
        caps[kVram] = d.gpu.vram_total() * d.cfg.vram_cap;
        caps[kGpu] = d.cfg.gpu_util_cap * 100;
        GpuUsage gl;
        bool cheap = d.gpu.global(&gl);
        // Without an adapter-wide utilization source (no NVML), per-engine counters are sampled at
        // a slower cadence because the wildcard query is the expensive one.
        bool util_due = gl.util >= 0 || now - d.last_gpu_scan >= kNoNvmlUtilScanMs;
        if (any_held || !cheap || gl.vram > caps[kVram] || (gl.util < 0 ? util_due : gl.util > caps[kGpu])) {
            d.last_gpu_scan = now;
            auto per = d.gpu.per_process();
            std::map<uint32_t, std::map<std::wstring, double>> group_types;
            for (auto& g : d.gov) {
                auto it = per.find(g.pid);
                if (it == per.end()) continue;
                for (auto& [t, v] : it->second.util_by_type) group_types[g.top][t] += v;
                groups[g.top].usage[kVram] += (double)it->second.vram;
            }
            for (auto& [top, types] : group_types)  // engines run in parallel: busiest engine type
                for (auto& [t, v] : types) groups[top].usage[kGpu] = std::max(groups[top].usage[kGpu], v);
        } else {
            if (gl.util >= 0) d.gpu.close_query();
            bound.usage[kGpu] = std::max(gl.util, 0.0);
            bound.usage[kVram] = (double)gl.vram;
        }
    }

    std::vector<Group> list;
    for (auto& [top, g] : groups) {
        if (g.suspended) g.usage[kMem] = g.usage[kGpu] = g.usage[kVram] = 0;
        list.push_back(g);
    }
    list.push_back(bound);
    PolicyResult pr = decide_policy(list, caps, now, kMinHoldMs, kResumeRatio);

    for (uint32_t top : pr.suspend) {
        Held h;
        h.at = now;
        for (auto& gr : list)
            if (gr.top == top)
                for (int k = 0; k < kResCount; ++k) h.saved[k] = gr.usage[k];
        if (!by_pid.count(top)) continue;
        h.top_create = by_pid[top]->create;
        for (auto& g : d.gov)
            if (g.top == top && by_pid.count(g.pid) && suspend_process(g.pid, by_pid[g.pid]->create, true))
                h.members.push_back({g.pid, by_pid[g.pid]->create});
        if (h.members.empty()) continue;
        log_line(L"suspended group %lu (%d processes): mem %.0f MB, vram %.0f MB, gpu %.0f%%", top,
                 (int)h.members.size(), h.saved[kMem] / 1048576, h.saved[kVram] / 1048576, h.saved[kGpu]);
        d.held[top] = h;
    }
    for (uint32_t top : pr.resume) resume_group(d, top);
    save_state(d, true);
}

void evaluate(Daemon& d) {
    Mode m = decide_mode(d.ov, d.locked, idle_ms(), (uint64_t)d.cfg.idle_minutes * 60000);
    if (m != d.mode) enter_mode(d, m);
    else if (m == Mode::Attended && GetTickCount64() - d.last_govern >= (uint64_t)d.cfg.tick_ms) govern(d);
}

void CALLBACK on_foreground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG obj, LONG, DWORD, DWORD) {
    Daemon& d = *g_d;
    if (obj != OBJID_WINDOW || !hwnd || d.mode != Mode::Attended || idle_ms() <= (uint64_t)d.cfg.focus_grace_ms)
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto governed = [&] {
        for (auto& g : d.gov)
            if (g.pid == pid) return true;
        return false;
    };
    if (!governed()) d.gov = d.scanner.scan(&d.procs);  // window of a process newer than the last tick
    if (!governed()) return;
    ShowWindowAsync(hwnd, SW_MINIMIZE);
    std::wstring name;
    for (auto& p : d.procs)
        if (p.pid == pid) name = p.name;
    log_line(L"focus guard: minimized window of pid %lu (%s)", pid, name.c_str());
}

void show_menu(Daemon& d) {
    HMENU m = CreatePopupMenu();
    auto item = [&](UINT id, const wchar_t* text, bool checked) {
        AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0), id, text);
    };
    item(kCmdAuto, L"Auto-detect", d.ov == Override::Auto);
    item(kCmdAttended, L"Attended: limit agents", d.ov == Override::Attended);
    item(kCmdUnattended, L"Unattended: no limits", d.ov == Override::Unattended);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(kCmdFolder, L"Open config && log folder", false);
    item(kCmdExit, L"Exit", false);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(d.hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, d.hwnd, nullptr);
    DestroyMenu(m);
}

void set_override(Daemon& d, Override o) {
    d.ov = o;
    log_line(L"override: %s", override_name(o));
    Mode m = decide_mode(d.ov, d.locked, idle_ms(), (uint64_t)d.cfg.idle_minutes * 60000);
    if (m != d.mode) enter_mode(d, m);
    save_state(d, true);
    update_tray(d, NIM_MODIFY);
}

void shutdown(Daemon& d) {
    if (d.mode == Mode::Attended) enter_mode(d, Mode::Unattended);
    save_state(d, false);
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Daemon& d = *g_d;
    if (msg == d.taskbar_created && msg) {
        update_tray(d, NIM_ADD);
        return 0;
    }
    switch (msg) {
        case WM_TIMER: evaluate(d); return 0;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK || wp == WTS_CONSOLE_DISCONNECT || wp == WTS_REMOTE_DISCONNECT) d.locked = true;
            if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT) d.locked = false;
            evaluate(d);
            return 0;
        case kMsgOverride: set_override(d, (Override)wp); return 0;
        case kTrayMsg:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) show_menu(d);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kCmdAuto: set_override(d, Override::Auto); break;
                case kCmdAttended: set_override(d, Override::Attended); break;
                case kCmdUnattended: set_override(d, Override::Unattended); break;
                case kCmdFolder: ShellExecuteW(nullptr, L"open", home_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
                case kCmdExit: DestroyWindow(h); break;
            }
            return 0;
        case WM_ENDSESSION:
            if (wp) shutdown(d);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LONG WINAPI on_crash(EXCEPTION_POINTERS*) {
    if (g_d) {
        for (auto& [top, h] : g_d->held)
            for (auto& m : h.members) resume_process(m.pid, m.create);
        if (g_d->cap.mask()) restore_cap_by_mask(g_d->cap.mask(), {});
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void prune_session_files() {
    std::wstring dir = path_in_home(L"sessions");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    uint64_t n = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    do {
        uint64_t t = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n - t > 7ull * 24 * 3600 * 10000000)
            DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

}  // namespace

int run_daemon() {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 2;

    static Daemon d;
    g_d = &d;
    d.cfg = load_config();
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);  // stay responsive under load
    recover_from_state(false);
    prune_session_files();
    SetUnhandledExceptionFilter(on_crash);

    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    d.total_ram = ms.ullTotalPhys;
    DWORD_PTR pm = 0, sm = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    d.ncpu = popcount64(sm);
    d.gpu.init();
    if (HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS)) CloseDesktop(desk);
    else d.locked = true;

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);
    d.hwnd = CreateWindowExW(0, kWindowClass, L"afk-governor", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                             wc.hInstance, nullptr);
    d.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    WTSRegisterSessionNotification(d.hwnd, NOTIFY_FOR_THIS_SESSION);
    update_tray(d, NIM_ADD);
    if (d.cfg.focus_guard)
        d.fg_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, on_foreground, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (d.cfg.mute) {
        static Scanner audio_scanner(d.cfg);  // used only on the audio thread
        d.audio.start([](DWORD pid) {
            std::vector<ProcInfo> procs;
            for (auto& g : audio_scanner.scan(&procs))
                if (g.pid == pid) return true;
            return false;
        });
    }
    log_line(L"started v%s: %d cpus, %.1f GB RAM, %.1f GB VRAM", kVersion, d.ncpu, d.total_ram / 1073741824.0,
             d.gpu.vram_total() / 1073741824.0);
    save_state(d, true);
    evaluate(d);
    SetTimer(d.hwnd, kTimerId, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    shutdown(d);
    d.audio.stop();
    if (d.fg_hook) UnhookWinEvent(d.fg_hook);
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = d.hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    WTSUnRegisterSessionNotification(d.hwnd);
    log_line(L"stopped");
    CloseHandle(mutex);
    return 0;
}

}  // namespace afk
