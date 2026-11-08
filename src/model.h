// Pure logic: no Win32 calls, unit-tested in tests/test_model.cpp.
#pragma once
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace afk {

enum class Mode { Attended, Unattended };
enum class Override { Auto, Attended, Unattended };

const wchar_t* mode_name(Mode m);
const wchar_t* override_name(Override o);
bool parse_override(const std::wstring& s, Override* out);

// Presence: forced override wins; otherwise lock or long idle means away.
Mode decide_mode(Override ov, bool locked, uint64_t idle_ms, uint64_t idle_threshold_ms);

// Affinity mask of floor(ncpu * cap) logical CPUs (at least 1), chosen at random,
// preferring CPUs outside `avoid`. ncpu <= 64.
uint64_t pick_cpu_mask(int ncpu, double cap, const std::vector<int>& avoid, std::mt19937_64& rng);
int popcount64(uint64_t m);

struct ProcInfo {
    uint32_t pid = 0;
    uint32_t ppid = 0;
    std::wstring name;     // lower-case image name, e.g. L"bash.exe"
    uint64_t create = 0;   // FILETIME as integer; 0 = unknown
    bool is_root = false;  // decided by caller (name + path rules)
};

struct Governed {
    uint32_t pid;
    uint32_t root;  // pid of the agent root (claude.exe / codex.exe)
    uint32_t top;   // direct child of root that this pid descends from; == pid for roots' children, == root for roots
};

// Roots plus all descendants. A child is accepted only if it was created no earlier than its
// parent (guards against PID reuse). Unknown create times (0) are accepted.
std::vector<Governed> build_governed(const std::vector<ProcInfo>& procs);

// ---- Suspension policy (memory / VRAM / GPU utilization) ----
enum Res { kMem = 0, kVram = 1, kGpu = 2, kResCount = 3 };

struct Group {
    uint32_t top = 0;
    bool suspendable = false;  // tool-call shells only
    bool suspended = false;
    uint64_t suspended_at_ms = 0;
    double usage[kResCount] = {0, 0, 0};       // current (0 for suspended groups' gpu)
    double saved[kResCount] = {0, 0, 0};       // usage captured at suspension
};

struct PolicyResult {
    std::vector<uint32_t> suspend;
    std::vector<uint32_t> resume;
};

// caps[r] < 0 disables resource r. Running groups over a cap get suspended heaviest-first;
// suspended groups resume FIFO once held >= min_hold_ms and their saved usage fits under
// resume_ratio * cap on every resource.
PolicyResult decide_policy(const std::vector<Group>& groups, const double caps[kResCount],
                           uint64_t now_ms, uint64_t min_hold_ms, double resume_ratio);

// ---- Persisted state (key=value lines) ----
struct SuspendedEntry {
    uint32_t pid = 0;
    uint64_t create = 0;
};

struct State {
    bool running = false;  // daemon alive and governing
    Mode mode = Mode::Unattended;
    Override ov = Override::Auto;
    uint64_t cpu_mask = 0;  // active cap mask (0 = none)
    int cpu_count = 0;
    uint64_t mem_cap = 0, vram_cap = 0;
    int gpu_util_cap = 0;
    std::vector<SuspendedEntry> suspended;
};

std::string serialize_state(const State& s);
State parse_state(const std::string& text);

// ---- Hook output ----
enum class HookEvent { SessionStart, UserPrompt, PreToolAsk };
bool parse_hook_event(const std::wstring& s, HookEvent* out);

// Context line describing the current mode, or "" when the daemon is not running.
std::string context_text(const State& s);
// Full JSON for stdout; "" means print nothing. `last_mode` is what this session was told
// before ("" if never); UserPrompt only re-injects on change.
std::string hook_output(HookEvent ev, const State& s, const std::string& last_mode);
std::string json_escape(const std::string& s);
// Extracts a top-level string field from a hook's stdin JSON (best effort).
std::string json_string_field(const std::string& json, const std::string& key);

// Codex ~/.codex/hooks.json content invoking `exe` (forward-slash path).
std::string codex_hooks_json(const std::string& exe);

}  // namespace afk
