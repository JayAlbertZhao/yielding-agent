# yielding-agent

Keeps AI coding agents (Claude Code, Codex) from hogging your Windows PC while you are using it, and lets
them run flat out when you are away.

| | Attended (you are at the PC) | Unattended (you are away) |
|---|---|---|
| CPU | all agent sessions together pinned to 50% of logical CPUs, random set avoiding CPU 0/1; below-normal priority | unlimited |
| RAM | over 50% → the heaviest tool call is suspended and paged out, resumed when there is room | unlimited |
| GPU | over 75% utilization or VRAM → the heaviest tool call is suspended | unlimited |
| Windows | agent windows that grab focus without your input get minimized | — |
| Audio | agent-started programs start muted | same |
| Agents are told | the budget, and to keep programs in the background | not to block on questions; decide and report |

You are "away" after 30 minutes without keyboard/mouse input or when the screen is locked. Any input switches
back within a second. The tray icon (orange = limiting, gray = idle) can force either mode.

Nothing is ever killed. Suspended processes are resumed and CPU pinning is undone when you leave, when the
daemon exits, or, if the daemon crashed, at its next start or at the next agent hook call. A suspended tool
call also resumes once there is headroom.

## Install

Requirements: Windows 10/11 x64. No runtime dependencies. The binaries are not code-signed, so SmartScreen
may ask for confirmation on first run.

1. Download `yielding-agent-<version>-win-x64.zip` from [Releases](https://github.com/JayAlbertZhao/yielding-agent/releases)
   and unzip it anywhere.
2. In PowerShell, from the unzipped folder, install and start the daemon (it copies itself to
   `%LOCALAPPDATA%\Programs\yielding-agent`):

   ```powershell
   .\yielding-agent.exe install --autostart
   ```

   `--autostart` adds a shortcut to your Startup folder. Add `--codex` to also write Codex hooks to
   `~/.codex/hooks.json` (only if that file does not exist yet; otherwise the snippet is printed for you to
   merge). Codex asks you to review and trust new hooks once.
3. Claude Code hooks (mode context, blocking-question guard):

   ```bash
   claude plugin marketplace add JayAlbertZhao/yielding-agent
   claude plugin install yielding-agent@yielding-agent
   ```

   The plugin calls the installed exe through Git Bash (Claude Code's default hook shell on Windows). Set
   `YIELDING_AGENT_EXE` if you installed elsewhere.

## Use

The tray icon menu covers everyday use. From PowerShell:

```powershell
$ya = "$env:LOCALAPPDATA\Programs\yielding-agent\yielding-agent.exe"
& $ya status
& $ya mode attended    # or unattended / auto
& $ya stop             # lifts all limits
& $ya start
```

Config: `%LOCALAPPDATA%\yielding-agent\config.ini` (written with defaults on first run; restart the daemon after
editing). Log: `yielding-agent.log` in the same folder.

| Key | Default | Meaning |
|---|---|---|
| `idle_minutes` | 30 | minutes without input before unattended |
| `cpu_cap` / `mem_cap` | 0.5 | attended budget as fraction of logical CPUs / physical RAM |
| `gpu_util_cap` / `vram_cap` | 0.75 | attended GPU utilization / dedicated VRAM budget |
| `avoid_cpus` | 0,1 | logical CPUs left out of the pinning set while others are available |
| `roots` | claude.exe,codex.exe | agent processes; everything they start is governed |
| `exclude_paths` | \WindowsApps\ | roots under these paths are ignored (desktop app UI) |
| `suspend_tops` | bash.exe,sh.exe,cmd.exe,powershell.exe,pwsh.exe,codex-command-runner.exe | only tool-call trees started by these may be suspended (MCP servers and the agents themselves never are) |
| `mute` / `focus_guard` | 1 | mute agent audio / minimize focus-grabbing agent windows |
| `focus_grace_ms` | 1000 | focus changes this soon after your input count as yours |

## How it works

One small native daemon (`yielding-agentw.exe`, ~0.1% CPU) with a tray icon.

- **Presence**: `GetLastInputInfo` polling (1 s) plus session lock notifications.
- **Governed set**: agent roots plus all descendants, from a process snapshot every 2 s while attended
  (parent links checked against creation times to survive PID reuse).
- **CPU**: process affinity + priority. Children inherit both, so new processes are capped from birth.
- **RAM / GPU**: measured lazily. System-wide numbers come first (memory status; NVML, or the PDH adapter
  memory counter on non-NVIDIA GPUs); per-process numbers (working set, PDH GPU counters) are read only when
  the system-wide value exceeds a budget or something is suspended. Without NVML there is no cheap
  system-wide utilization, so per-process utilization is sampled every 10 s instead. Over budget, the heaviest tool-call tree is suspended (`NtSuspendProcess`) and its working set
  trimmed; it resumes FIFO after at least 30 s once its usage fits under 90% of the budget.
- **Focus**: `EVENT_SYSTEM_FOREGROUND` hook; only governed windows that took focus without recent input.
- **Audio**: WASAPI session notifications; each agent audio session is muted once (unmute it in the volume
  mixer and it stays unmuted).
- **Hooks**: `yielding-agent hook <event>` reads the daemon's state file and prints hook JSON. Sessions get the
  mode at SessionStart, and again after a prompt or a tool call (PostToolUse) whenever it changed since they
  were last told, since leaving the PC does not send a prompt. If you leave while Claude is writing without
  tool calls, its Stop hook blocks the end of that turn once so it can re-decide with the new mode (Claude Code
  shows this as "Stop hook error"). `AskUserQuestion` is denied while unattended. Codex gets SessionStart,
  UserPromptSubmit and PostToolUse context. Each hook call costs about 60 ms, mostly Git Bash startup.

## Limitations

- Suspension does not free VRAM by itself; Windows may page a suspended process's VRAM out only under pressure.
- A suspended foreground tool call can still hit the agent's own command timeout.
- Up to 64 logical CPUs (one processor group). GPU budgets use the first NVIDIA GPU via NVML, otherwise all
  adapters via PDH counters.
- Input injected by automation (including agent computer use) counts as presence.
- Windows hosted by Windows Terminal belong to the terminal, not to the agent, and are not focus-guarded.
- Audio devices connected after the daemon started are not watched until it restarts.

## Uninstall

```powershell
& "$env:LOCALAPPDATA\Programs\yielding-agent\yielding-agent.exe" uninstall
```

Then `claude plugin uninstall yielding-agent@yielding-agent` and delete the two folders it prints.

## Build

Visual Studio 2022 (C++ workload) and CMake 3.20+:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\test_model.exe
```

## License

MIT
