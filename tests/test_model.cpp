#include <cstdio>
#include <set>

#include "model.h"

using namespace yielding;

static int g_fail = 0;
#define CHECK(c)                                                   \
    do {                                                           \
        if (!(c)) {                                                \
            ++g_fail;                                              \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
        }                                                          \
    } while (0)

static void test_mode() {
    const uint64_t T = 30 * 60000;
    CHECK(decide_mode(Override::Auto, false, 0, T) == Mode::Attended);
    CHECK(decide_mode(Override::Auto, false, T - 1, T) == Mode::Attended);
    CHECK(decide_mode(Override::Auto, false, T, T) == Mode::Unattended);
    CHECK(decide_mode(Override::Auto, true, 0, T) == Mode::Unattended);
    CHECK(decide_mode(Override::Attended, true, T * 2, T) == Mode::Attended);
    CHECK(decide_mode(Override::Unattended, false, 0, T) == Mode::Unattended);
    Override o;
    CHECK(parse_override(L"attended", &o) && o == Override::Attended);
    CHECK(!parse_override(L"bogus", &o));
}

static void test_cpu_mask() {
    std::mt19937_64 rng(42);
    std::set<uint64_t> distinct;
    uint64_t seen = 0;
    for (int i = 0; i < 200; ++i) {
        uint64_t m = pick_cpu_mask(32, 0.5, {0, 1}, rng);
        CHECK(popcount64(m) == 16);
        CHECK((m & 0x3) == 0);
        CHECK((m >> 32) == 0);
        distinct.insert(m);
        seen |= m;
    }
    CHECK(distinct.size() > 100);          // random, not a fixed pattern
    CHECK(seen == 0xFFFFFFFCull);         // every non-avoided cpu gets used
    CHECK(popcount64(pick_cpu_mask(8, 0.01, {}, rng)) == 1);  // at least one cpu
    uint64_t m = pick_cpu_mask(4, 0.5, {0, 1, 2}, rng);       // too few preferred: fall back
    CHECK(popcount64(m) == 2 && (m & 0x8));
    CHECK(pick_cpu_mask(0, 0.5, {}, rng) == 0);
}

static ProcInfo P(uint32_t pid, uint32_t ppid, uint64_t create, bool root = false, const wchar_t* name = L"x.exe") {
    ProcInfo p;
    p.pid = pid;
    p.ppid = ppid;
    p.create = create;
    p.is_root = root;
    p.name = name;
    return p;
}

static void test_tree() {
    std::vector<ProcInfo> procs = {
        P(1, 0, 1),         // explorer
        P(10, 1, 10, true), // claude root
        P(11, 10, 11),      // bash tool call
        P(12, 11, 12),      // python under bash
        P(13, 10, 13),      // mcp server
        P(20, 1, 20),       // unrelated
        P(30, 10, 5),       // ppid 10 reused: created before the root -> not governed
        P(31, 30, 31),      // child of the stale process -> not governed
        P(40, 11, 40, true),// nested claude under a tool call -> part of the outer tree
        P(41, 40, 41),
    };
    std::map<uint32_t, Governed> g;
    for (auto& x : build_governed(procs)) g[x.pid] = x;
    CHECK(g.size() == 6);
    CHECK(g.count(10) && g[10].top == 10 && g[10].root == 10);
    CHECK(g.count(11) && g[11].top == 11);
    CHECK(g.count(12) && g[12].top == 11);
    CHECK(g.count(13) && g[13].top == 13);
    CHECK(!g.count(20) && !g.count(30) && !g.count(31));
    CHECK(g.count(40) && g[40].top == 11 && g[40].root == 10);
    CHECK(g.count(41) && g[41].top == 11);
    // A loop in parent links must not hang.
    std::vector<ProcInfo> loop = {P(50, 51, 0, true), P(51, 50, 0, true)};
    build_governed(loop);
}

static Group G(uint32_t top, bool susp, double mem, bool suspended = false, uint64_t at = 0, double saved = 0) {
    Group g;
    g.top = top;
    g.suspendable = susp;
    g.suspended = suspended;
    g.suspended_at_ms = at;
    g.usage[kMem] = suspended ? 0 : mem;
    g.saved[kMem] = saved;
    return g;
}

static void test_policy() {
    double caps[kResCount] = {100, -1, -1};
    auto r = decide_policy({G(1, true, 70), G(2, true, 50), G(3, false, 10)}, caps, 0, 30000, 0.9);
    CHECK(r.suspend.size() == 1 && r.suspend[0] == 1 && r.resume.empty());

    r = decide_policy({G(1, true, 60), G(2, true, 50), G(3, false, 60)}, caps, 0, 30000, 0.9);
    CHECK(r.suspend.size() == 2);  // 170 -> 110 -> 60

    r = decide_policy({G(3, false, 150)}, caps, 0, 30000, 0.9);
    CHECK(r.suspend.empty());  // nothing suspendable

    // Resume needs the hold time and room under 90% of the cap.
    r = decide_policy({G(1, true, 0, true, 0, 70), G(2, true, 50)}, caps, 60000, 30000, 0.9);
    CHECK(r.resume.empty());
    r = decide_policy({G(1, true, 0, true, 0, 70), G(2, true, 10)}, caps, 60000, 30000, 0.9);
    CHECK(r.resume.size() == 1 && r.resume[0] == 1);
    r = decide_policy({G(1, true, 0, true, 50000, 70), G(2, true, 10)}, caps, 60000, 30000, 0.9);
    CHECK(r.resume.empty());
    // FIFO and cumulative: only the first fits.
    r = decide_policy({G(1, true, 0, true, 0, 50), G(2, true, 0, true, 10, 50)}, caps, 60000, 30000, 0.9);
    CHECK(r.resume.size() == 1 && r.resume[0] == 1);

    double off[kResCount] = {-1, -1, -1};
    r = decide_policy({G(1, true, 1e12)}, off, 0, 30000, 0.9);
    CHECK(r.suspend.empty());
}

static std::string H(HookEvent ev, const State& s, const std::string& last, bool stop = false, bool cu = false) {
    HookInput in;
    in.last_mode = last;
    in.stop_hook_active = stop;
    in.computer_use_approved = cu;
    return hook_output(ev, s, in);
}

static void test_state_and_hooks() {
    State s;
    s.running = true;
    s.mode = Mode::Attended;
    s.ov = Override::Unattended;
    s.cpu_mask = 0xFFFC0000ull;
    s.cpu_count = 14;
    s.mem_cap = 34359738368ull;
    s.vram_cap = 25769803776ull;
    s.gpu_util_cap = 75;
    s.suspended = {{123, 456}, {789, 1011}};
    State t = parse_state(serialize_state(s));
    CHECK(t.running && t.mode == Mode::Attended && t.ov == Override::Unattended);
    CHECK(t.cpu_mask == s.cpu_mask && t.cpu_count == 14 && t.mem_cap == s.mem_cap && t.vram_cap == s.vram_cap);
    CHECK(t.gpu_util_cap == 75 && t.suspended.size() == 2 && t.suspended[1].pid == 789 && t.suspended[1].create == 1011);
    CHECK(!parse_state("").running);

    CHECK(H(HookEvent::SessionStart, State{}, "") == "");
    CHECK(H(HookEvent::UserPrompt, State{}, "") == "");
    CHECK(H(HookEvent::UserPrompt, State{}, "off") == "");
    CHECK(H(HookEvent::UserPrompt, State{}, "attended").find("no longer running") != std::string::npos);
    CHECK(H(HookEvent::PreToolAsk, State{}, "unattended") == "");
    std::string o = H(HookEvent::SessionStart, s, "");
    CHECK(o.find("\"hookEventName\":\"SessionStart\"") != std::string::npos);
    CHECK(o.find("AT the computer") != std::string::npos && o.find("14 logical cores") != std::string::npos);
    CHECK(o.find("32.0 GB") != std::string::npos);
    CHECK(H(HookEvent::UserPrompt, s, "attended") == "");
    CHECK(H(HookEvent::UserPrompt, s, "unattended").find("UserPromptSubmit") != std::string::npos);
    CHECK(H(HookEvent::PreToolAsk, s, "") == "");
    s.mode = Mode::Unattended;
    CHECK(H(HookEvent::PreToolAsk, s, "").find("\"permissionDecision\":\"deny\"") != std::string::npos);
    CHECK(H(HookEvent::SessionStart, s, "").find("AWAY") != std::string::npos);
    // Mid-turn: PostTool injects only on change; Stop blocks once when away and not yet told.
    CHECK(H(HookEvent::PostTool, s, "unattended") == "");
    CHECK(H(HookEvent::PostTool, s, "attended").find("\"hookEventName\":\"PostToolUse\"") != std::string::npos);
    CHECK(H(HookEvent::Stop, s, "attended").find("\"decision\":\"block\"") != std::string::npos);
    CHECK(H(HookEvent::Stop, s, "attended", true) == "");
    CHECK(H(HookEvent::Stop, s, "unattended") == "");
    CHECK(H(HookEvent::PostTool, State{}, "unattended").find("no longer running") != std::string::npos);
    s.mode = Mode::Attended;
    CHECK(H(HookEvent::Stop, s, "") == "");
    s.mode = Mode::Unattended;
    // Computer use: attended asks every time; unattended inherits a session's earlier approval.
    s.mode = Mode::Attended;
    CHECK(H(HookEvent::PreToolComputer, s, "", false, true).find("\"permissionDecision\":\"ask\"") != std::string::npos);
    s.mode = Mode::Unattended;
    CHECK(H(HookEvent::PreToolComputer, s, "", false, true).find("\"permissionDecision\":\"allow\"") != std::string::npos);
    CHECK(H(HookEvent::PreToolComputer, s, "", false, false).find("\"permissionDecision\":\"ask\"") != std::string::npos);
    CHECK(H(HookEvent::PreToolComputer, State{}, "", false, false) == "");  // no governor: no policy
    CHECK(is_computer_use_tool("mcp__computer-use__left_click"));
    CHECK(is_computer_use_tool("mcp__remote-devices__computer_batch"));
    CHECK(!is_computer_use_tool("mcp__claude-in-chrome__computer"));
    CHECK(json_true_field("{\"stop_hook_active\": true}", "stop_hook_active"));
    CHECK(!json_true_field("{\"stop_hook_active\":false}", "stop_hook_active"));
    CHECK(!json_true_field("{}", "stop_hook_active"));

    CHECK(json_escape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
    CHECK(json_string_field("{\"x\":1, \"session_id\" : \"ab\\\"c\"}", "session_id") == "ab\"c");
    CHECK(json_string_field("{}", "session_id") == "");
    HookEvent ev;
    CHECK(parse_hook_event(L"pre-tool-ask", &ev) && ev == HookEvent::PreToolAsk);

    std::string c = codex_hooks_json("C:/Users/A B/yielding-agent.exe");
    CHECK(c.find("\\\"C:/Users/A B/yielding-agent.exe\\\" hook session-start") != std::string::npos);
    CHECK(codex_hooks_json("C:/x/yielding-agent.exe").find("\"C:/x/yielding-agent.exe hook user-prompt\"") != std::string::npos);
}

int main() {
    test_mode();
    test_cpu_mask();
    test_tree();
    test_policy();
    test_state_and_hooks();
    std::printf(g_fail ? "%d check(s) failed\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
