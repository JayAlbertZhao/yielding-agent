// CLI entry. yielding-agent.exe (console) and yielding-agentw.exe (windowless daemon) share this file.
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <string>

#include "platform.h"

namespace yielding {
int run_daemon();
}

using namespace yielding;

namespace {

const wchar_t kRepo[] = L"JayAlbertZhao/yielding-agent";

void out(const std::string& s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), (DWORD)s.size(), &w, nullptr);
}
void outw(const std::wstring& s) { out(utf8(s)); }

std::wstring install_dir() {
    wchar_t buf[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Programs\\yielding-agent";
}

bool spawn_daemon(const std::wstring& dir) {
    std::wstring exe = dir + L"\\yielding-agentw.exe";
    std::wstring cmd = L"\"" + exe + L"\" run";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    // Break away from the caller's job (e.g. an agent session) so closing it does not kill us.
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB,
                             nullptr, dir.c_str(), &si, &pi);
    if (!ok) ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, dir.c_str(), &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (int i = 0; i < 30 && !daemon_alive(); ++i) Sleep(100);
    return daemon_alive();
}

int cmd_start() {
    if (daemon_alive()) return out("already running\n"), 0;
    if (!spawn_daemon(exe_dir())) return out("failed to start daemon\n"), 1;
    out("started\n");
    return 0;
}

bool stop_daemon() {
    HWND w = FindWindowW(kWindowClass, nullptr);
    if (w) PostMessageW(w, WM_CLOSE, 0, 0);
    for (int i = 0; i < 100 && daemon_alive(); ++i) Sleep(100);
    return !daemon_alive();
}

int cmd_stop() {
    if (!daemon_alive()) return out("not running\n"), 0;
    if (!stop_daemon()) return out("daemon did not stop within 10 s\n"), 1;
    out("stopped\n");
    return 0;
}

int cmd_mode(const std::wstring& arg) {
    Override o;
    if (!parse_override(arg, &o)) return out("usage: yielding-agent mode auto|attended|unattended\n"), 1;
    HWND w = FindWindowW(kWindowClass, nullptr);
    if (!w) return out("daemon not running\n"), 1;
    PostMessageW(w, kMsgOverride, (WPARAM)o, 0);
    outw(std::wstring(L"override set: ") + override_name(o) + L"\n");
    return 0;
}

int cmd_status() {
    bool alive = daemon_alive();
    State s = read_state();
    char line[256];
    snprintf(line, sizeof line, "daemon: %s\nmode: %s (override %s)\n", alive ? "running" : "not running",
             alive && s.running ? utf8(mode_name(s.mode)).c_str() : "-", utf8(override_name(s.ov)).c_str());
    out(line);
    if (alive && s.mode == Mode::Attended) {
        snprintf(line, sizeof line, "cpu mask: 0x%llx (%d cpus)\nsuspended processes: %zu\n",
                 (unsigned long long)s.cpu_mask, s.cpu_count, s.suspended.size());
        out(line);
    }
    outw(L"home: " + home_dir() + L"\n");
    return 0;
}

std::string read_stdin() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::string data;
    DWORD type = in ? GetFileType(in) : FILE_TYPE_UNKNOWN;
    if (type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK) return data;
    char buf[8192];
    DWORD n;
    while (ReadFile(in, buf, sizeof buf, &n, nullptr) && n) data.append(buf, n);
    return data;
}

int cmd_hook(const std::wstring& arg) {
    HookEvent ev;
    std::string input = read_stdin();  // always drain so the caller never blocks on a full pipe
    if (!parse_hook_event(arg, &ev)) return 0;
    State s;
    if (daemon_alive()) {
        s = read_state();
    } else {
        State stale = read_state();
        if (stale.running || !stale.suspended.empty()) recover_from_state(true);
    }
    std::string sid;
    for (char c : json_string_field(input, "session_id"))
        if (isalnum((unsigned char)c) || c == '-' || c == '_') sid += c;
    std::wstring sess_file = sid.empty() ? L"" : path_in_home(L"sessions") + L"\\" + wide(sid);
    std::string last;
    if (!sess_file.empty()) read_file(sess_file, &last);
    std::string o = hook_output(ev, s, last, json_true_field(input, "stop_hook_active"));
    out(o);
    if (!o.empty() && !sess_file.empty()) {  // every output tells the session the current mode
        CreateDirectoryW(path_in_home(L"sessions").c_str(), nullptr);
        write_file_atomic(sess_file, s.running ? utf8(mode_name(s.mode)) : "off");
    }
    return 0;
}

std::wstring fwd_slashes(std::wstring p) {
    for (auto& c : p)
        if (c == L'\\') c = L'/';
    return p;
}

// The 8.3 short form keeps the command free of spaces, so it runs the same under cmd, PowerShell
// and bash whichever shell Codex uses.
std::string codex_exe() {
    std::wstring exe = install_dir() + L"\\yielding-agent.exe";
    wchar_t buf[MAX_PATH];
    DWORD n = GetShortPathNameW(exe.c_str(), buf, MAX_PATH);
    return utf8(fwd_slashes(n && n < MAX_PATH ? std::wstring(buf) : exe));
}

std::wstring codex_hooks_path() {
    wchar_t buf[MAX_PATH];
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    return std::wstring(buf) + L"\\.codex\\hooks.json";
}

std::wstring startup_link() {
    PWSTR dir = nullptr;
    SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &dir);
    std::wstring p = std::wstring(dir) + L"\\yielding-agent.lnk";
    CoTaskMemFree(dir);
    return p;
}

bool create_shortcut(const std::wstring& link, const std::wstring& target, const std::wstring& args) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* sl = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) {
        sl->SetPath(target.c_str());
        sl->SetArguments(args.c_str());
        sl->SetWorkingDirectory(target.substr(0, target.find_last_of(L'\\')).c_str());
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
            ok = SUCCEEDED(pf->Save(link.c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
    }
    CoUninitialize();
    return ok;
}

int cmd_install(int argc, wchar_t** argv) {
    bool autostart = false, codex = false, start = true;
    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--autostart") autostart = true;
        else if (a == L"--codex") codex = true;
        else if (a == L"--no-start") start = false;
        else return outw(L"unknown option: " + a + L"\n"), 1;
    }
    std::wstring dest = install_dir(), src = exe_dir();
    if (lower(src) != lower(dest)) {
        if (daemon_alive() && !stop_daemon()) return out("could not stop the running daemon\n"), 1;
        CreateDirectoryW((dest.substr(0, dest.find_last_of(L'\\'))).c_str(), nullptr);
        CreateDirectoryW(dest.c_str(), nullptr);
        for (const wchar_t* f : {L"yielding-agent.exe", L"yielding-agentw.exe"})
            if (!CopyFileW((src + L"\\" + f).c_str(), (dest + L"\\" + f).c_str(), FALSE))
                return outw(L"copy failed: " + std::wstring(f) + L"\n"), 1;
    }
    outw(L"installed to " + dest + L"\n");
    load_config();
    outw(L"config: " + path_in_home(L"config.ini") + L"\n");

    if (autostart) {
        bool ok = create_shortcut(startup_link(), dest + L"\\yielding-agentw.exe", L"run");
        outw(ok ? L"autostart: " + startup_link() + L"\n" : L"autostart: failed to create shortcut\n");
    }
    if (codex) {
        std::wstring hp = codex_hooks_path();
        std::string existing, mine = codex_hooks_json(codex_exe());
        if (!read_file(hp, &existing)) {
            write_file_atomic(hp, mine);
            outw(L"codex: wrote " + hp + L" (review and trust the hooks in Codex once)\n");
        } else if (existing.find("yielding-agent") != std::string::npos) {
            outw(L"codex: " + hp + L" already references yielding-agent\n");
        } else {
            outw(L"codex: " + hp + L" exists; merge these hooks into it manually:\n");
            out(mine);
        }
    }
    if (start) out(spawn_daemon(dest) ? "daemon started\n" : "failed to start daemon\n");
    outw(std::wstring(L"\nClaude Code plugin (hooks):\n  claude plugin marketplace add ") + kRepo +
         L"\n  claude plugin install yielding-agent@yielding-agent\n");
    return 0;
}

int cmd_uninstall() {
    if (daemon_alive()) stop_daemon();
    if (DeleteFileW(startup_link().c_str())) out("autostart removed\n");
    std::wstring hp = codex_hooks_path();
    std::string existing;
    if (read_file(hp, &existing)) {
        if (existing == codex_hooks_json(codex_exe())) {
            DeleteFileW(hp.c_str());
            out("codex hooks removed\n");
        } else if (existing.find("yielding-agent") != std::string::npos) {
            outw(L"codex: remove the yielding-agent entries from " + hp + L" by hand\n");
        }
    }
    outw(L"Remaining manual steps:\n  claude plugin uninstall yielding-agent@yielding-agent\n  delete " + install_dir() +
         L"\n  delete " + home_dir() + L"\n");
    return 0;
}

const char kHelp[] =
    "yielding-agent - keeps AI agent sessions from hogging the PC while you use it\n\n"
    "usage: yielding-agent <command>\n"
    "  start                 start the background daemon\n"
    "  stop                  stop it (lifts all limits, resumes suspended processes)\n"
    "  status                show daemon state\n"
    "  mode auto|attended|unattended   override presence detection\n"
    "  install [--autostart] [--codex] [--no-start]\n"
    "  uninstall\n"
    "  restore               undo limits left behind by a crashed daemon\n"
    "  hook <session-start|user-prompt|pre-tool-ask>   agent hook entry (reads JSON on stdin)\n"
    "  run                   run the daemon in this process\n"
    "  version\n";

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring cmd = argc > 1 ? argv[1] : L"";
#ifdef YIELDING_GUI
    if (cmd.empty()) cmd = L"run";
#endif
    if (cmd == L"run") {
        int r = run_daemon();
        if (r == 2) out("already running\n");
        return r;
    }
    if (cmd == L"start") return cmd_start();
    if (cmd == L"stop") return cmd_stop();
    if (cmd == L"status") return cmd_status();
    if (cmd == L"mode") return cmd_mode(argc > 2 ? argv[2] : L"");
    if (cmd == L"hook") return cmd_hook(argc > 2 ? argv[2] : L"");
    if (cmd == L"install") return cmd_install(argc, argv);
    if (cmd == L"uninstall") return cmd_uninstall();
    if (cmd == L"restore") {
        if (daemon_alive()) return out("daemon is running; use stop instead\n"), 1;
        recover_from_state(false);
        return out("done\n"), 0;
    }
    if (cmd == L"version") return outw(std::wstring(kVersion) + L"\n"), 0;
    out(kHelp);
    return cmd.empty() || cmd == L"help" || cmd == L"--help" ? 0 : 1;
}
