#include "model.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

namespace afk {

const wchar_t* mode_name(Mode m) { return m == Mode::Attended ? L"attended" : L"unattended"; }

static const char* mode_name_a(Mode m) { return m == Mode::Attended ? "attended" : "unattended"; }

const wchar_t* override_name(Override o) {
    switch (o) {
        case Override::Attended: return L"attended";
        case Override::Unattended: return L"unattended";
        default: return L"auto";
    }
}

bool parse_override(const std::wstring& s, Override* out) {
    if (s == L"auto") *out = Override::Auto;
    else if (s == L"attended") *out = Override::Attended;
    else if (s == L"unattended") *out = Override::Unattended;
    else return false;
    return true;
}

Mode decide_mode(Override ov, bool locked, uint64_t idle_ms, uint64_t idle_threshold_ms) {
    if (ov == Override::Attended) return Mode::Attended;
    if (ov == Override::Unattended) return Mode::Unattended;
    if (locked || idle_ms >= idle_threshold_ms) return Mode::Unattended;
    return Mode::Attended;
}

int popcount64(uint64_t m) {
    int n = 0;
    for (; m; m &= m - 1) ++n;
    return n;
}

uint64_t pick_cpu_mask(int ncpu, double cap, const std::vector<int>& avoid, std::mt19937_64& rng) {
    if (ncpu <= 0) return 0;
    if (ncpu > 64) ncpu = 64;
    int want = static_cast<int>(ncpu * cap);
    want = std::max(1, std::min(want, ncpu));
    std::set<int> avoided(avoid.begin(), avoid.end());
    std::vector<int> preferred, fallback;
    for (int i = 0; i < ncpu; ++i) (avoided.count(i) ? fallback : preferred).push_back(i);
    std::shuffle(preferred.begin(), preferred.end(), rng);
    std::shuffle(fallback.begin(), fallback.end(), rng);
    preferred.insert(preferred.end(), fallback.begin(), fallback.end());
    uint64_t mask = 0;
    for (int i = 0; i < want; ++i) mask |= 1ull << preferred[i];
    return mask;
}

std::vector<Governed> build_governed(const std::vector<ProcInfo>& procs) {
    std::map<uint32_t, size_t> by_pid;
    std::map<uint32_t, std::vector<size_t>> children;
    for (size_t i = 0; i < procs.size(); ++i) by_pid[procs[i].pid] = i;

    auto parent_of = [&](size_t i) -> long long {
        const ProcInfo& c = procs[i];
        if (c.ppid == c.pid) return -1;
        auto it = by_pid.find(c.ppid);
        if (it == by_pid.end()) return -1;
        const ProcInfo& p = procs[it->second];
        if (p.create && c.create && p.create > c.create) return -1;  // PID was reused
        return static_cast<long long>(it->second);
    };
    for (size_t i = 0; i < procs.size(); ++i) {
        long long p = parent_of(i);
        if (p >= 0) children[procs[p].pid].push_back(i);
    }

    std::vector<Governed> out;
    std::set<uint32_t> seen;
    for (size_t i = 0; i < procs.size(); ++i) {
        if (!procs[i].is_root) continue;
        bool nested = false;  // a root under another root belongs to the outer tree
        std::set<size_t> walked;
        for (long long p = parent_of(i); p >= 0 && walked.insert(p).second; p = parent_of(p))
            if (procs[p].is_root) { nested = true; break; }
        if (nested) continue;

        uint32_t root = procs[i].pid;
        std::vector<Governed> stack{{root, root, root}};
        while (!stack.empty()) {
            Governed g = stack.back();
            stack.pop_back();
            if (!seen.insert(g.pid).second) continue;
            out.push_back(g);
            auto it = children.find(g.pid);
            if (it == children.end()) continue;
            for (size_t ci : it->second) {
                uint32_t cp = procs[ci].pid;
                stack.push_back({cp, root, g.pid == root ? cp : g.top});
            }
        }
    }
    return out;
}

PolicyResult decide_policy(const std::vector<Group>& groups, const double caps[kResCount],
                           uint64_t now_ms, uint64_t min_hold_ms, double resume_ratio) {
    PolicyResult r;
    double total[kResCount] = {0, 0, 0};
    for (const Group& g : groups)
        if (!g.suspended)
            for (int k = 0; k < kResCount; ++k) total[k] += g.usage[k];

    std::set<uint32_t> chosen;
    for (int k = 0; k < kResCount; ++k) {
        if (caps[k] < 0) continue;
        while (total[k] > caps[k]) {
            const Group* best = nullptr;
            for (const Group& g : groups) {
                if (!g.suspendable || g.suspended || chosen.count(g.top) || g.usage[k] <= 0) continue;
                if (!best || g.usage[k] > best->usage[k]) best = &g;
            }
            if (!best) break;
            chosen.insert(best->top);
            r.suspend.push_back(best->top);
            for (int j = 0; j < kResCount; ++j) total[j] -= best->usage[j];
        }
    }
    if (!r.suspend.empty()) return r;

    std::vector<const Group*> held;
    for (const Group& g : groups)
        if (g.suspended) held.push_back(&g);
    std::sort(held.begin(), held.end(),
              [](const Group* a, const Group* b) { return a->suspended_at_ms < b->suspended_at_ms; });
    for (const Group* g : held) {
        if (now_ms - g->suspended_at_ms < min_hold_ms) continue;
        bool fits = true;
        for (int k = 0; k < kResCount; ++k)
            if (caps[k] >= 0 && total[k] + g->saved[k] > resume_ratio * caps[k]) fits = false;
        if (!fits) continue;
        r.resume.push_back(g->top);
        for (int k = 0; k < kResCount; ++k) total[k] += g->saved[k];
    }
    return r;
}

std::string serialize_state(const State& s) {
    std::ostringstream o;
    char mask[32];
    snprintf(mask, sizeof mask, "0x%llx", static_cast<unsigned long long>(s.cpu_mask));
    o << "running=" << (s.running ? 1 : 0) << "\n"
      << "mode=" << mode_name_a(s.mode) << "\n"
      << "override=" << (s.ov == Override::Auto ? "auto" : s.ov == Override::Attended ? "attended" : "unattended")
      << "\n"
      << "cpu_mask=" << mask << "\n"
      << "cpu_count=" << s.cpu_count << "\n"
      << "mem_cap=" << s.mem_cap << "\n"
      << "vram_cap=" << s.vram_cap << "\n"
      << "gpu_util_cap=" << s.gpu_util_cap << "\n"
      << "suspended=";
    for (size_t i = 0; i < s.suspended.size(); ++i)
        o << (i ? "," : "") << s.suspended[i].pid << ":" << s.suspended[i].create;
    o << "\n";
    return o.str();
}

State parse_state(const std::string& text) {
    State s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "running") s.running = v == "1";
        else if (k == "mode") s.mode = v == "attended" ? Mode::Attended : Mode::Unattended;
        else if (k == "override") s.ov = v == "attended" ? Override::Attended : v == "unattended" ? Override::Unattended : Override::Auto;
        else if (k == "cpu_mask") s.cpu_mask = strtoull(v.c_str(), nullptr, 0);
        else if (k == "cpu_count") s.cpu_count = atoi(v.c_str());
        else if (k == "mem_cap") s.mem_cap = strtoull(v.c_str(), nullptr, 10);
        else if (k == "vram_cap") s.vram_cap = strtoull(v.c_str(), nullptr, 10);
        else if (k == "gpu_util_cap") s.gpu_util_cap = atoi(v.c_str());
        else if (k == "suspended") {
            std::istringstream items(v);
            std::string item;
            while (std::getline(items, item, ',')) {
                size_t c = item.find(':');
                if (c == std::string::npos) continue;
                s.suspended.push_back({static_cast<uint32_t>(strtoul(item.substr(0, c).c_str(), nullptr, 10)),
                                       strtoull(item.substr(c + 1).c_str(), nullptr, 10)});
            }
        }
    }
    return s;
}

bool parse_hook_event(const std::wstring& s, HookEvent* out) {
    if (s == L"session-start") *out = HookEvent::SessionStart;
    else if (s == L"user-prompt") *out = HookEvent::UserPrompt;
    else if (s == L"pre-tool-ask") *out = HookEvent::PreToolAsk;
    else return false;
    return true;
}

static std::string gb(uint64_t bytes) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

std::string context_text(const State& s) {
    if (!s.running) return "";
    if (s.mode == Mode::Attended) {
        return "[afk-governor] The user is AT the computer (attended mode). Combined budget for all agent "
               "sessions: CPU " + std::to_string(s.cpu_count) + " logical cores (enforced by CPU pinning), RAM " +
               gb(s.mem_cap) + ", GPU " + std::to_string(s.gpu_util_cap) + "% utilization and " + gb(s.vram_cap) +
               " VRAM. Size parallelism (--jobs, -j, worker counts) to fit; tool processes over budget are "
               "suspended until headroom returns. Run programs in the background: never bring windows to the "
               "foreground, keep audio muted.";
    }
    return "[afk-governor] The user is AWAY (unattended mode). No resource limits. Do not block on questions "
           "or buttons: decide non-blocking matters yourself following the user's guidelines and list them in "
           "your final report; stop and wait only for a decision that truly blocks all remaining work.";
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

std::string hook_output(HookEvent ev, const State& s, const std::string& last_mode) {
    if (!s.running) {  // tell sessions that were given a budget that it no longer applies
        if (ev != HookEvent::UserPrompt || (last_mode != "attended" && last_mode != "unattended")) return "";
        return "{\"hookSpecificOutput\":{\"hookEventName\":\"UserPromptSubmit\",\"additionalContext\":\"" +
               json_escape("[afk-governor] The governor is no longer running; earlier resource budgets and "
                           "away-mode instructions no longer apply.") +
               "\"}}";
    }
    switch (ev) {
        case HookEvent::SessionStart:
        case HookEvent::UserPrompt: {
            if (ev == HookEvent::UserPrompt && last_mode == mode_name_a(s.mode)) return "";
            const char* name = ev == HookEvent::SessionStart ? "SessionStart" : "UserPromptSubmit";
            return std::string("{\"hookSpecificOutput\":{\"hookEventName\":\"") + name +
                   "\",\"additionalContext\":\"" + json_escape(context_text(s)) + "\"}}";
        }
        case HookEvent::PreToolAsk:
            if (s.mode != Mode::Unattended) return "";
            return "{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"permissionDecision\":\"deny\","
                   "\"permissionDecisionReason\":\"" +
                   json_escape("[afk-governor] The user is away (unattended mode) and cannot answer now. Decide "
                               "this yourself following the user's guidelines and note the decision in your final "
                               "report. Only if the question truly blocks all remaining work, end your turn with "
                               "the question in plain text.") +
                   "\"}}";
    }
    return "";
}

std::string json_string_field(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return "";
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || json[p] != '"') return "";
    std::string out;
    for (++p; p < json.size() && json[p] != '"'; ++p) {
        if (json[p] == '\\' && p + 1 < json.size()) ++p;
        out += json[p];
    }
    return out;
}

std::string codex_hooks_json(const std::string& exe) {
    std::string cmd = exe.find(' ') == std::string::npos ? exe : "\"" + exe + "\"";
    auto entry = [&](const char* ev) {
        return "[{\"hooks\":[{\"type\":\"command\",\"command\":\"" + json_escape(cmd + " hook " + ev) + "\"}]}]";
    };
    return "{\n  \"hooks\": {\n    \"SessionStart\": " + entry("session-start") +
           ",\n    \"UserPromptSubmit\": " + entry("user-prompt") + "\n  }\n}\n";
}

}  // namespace afk
