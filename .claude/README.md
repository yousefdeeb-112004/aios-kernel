# `.claude/` — Claude Code configuration for AIOS

What lives here and how it's used:

| Path                         | Type          | How it triggers                                    |
|------------------------------|---------------|----------------------------------------------------|
| `../CLAUDE.md`               | Project memory| Auto-loaded into context every session (repo root) |
| `settings.local.json`        | Settings      | Local permissions (git-ignore this if you commit)  |
| `commands/build.md`          | Slash command | Type `/build`                                      |
| `commands/run.md`            | Slash command | Type `/run` — headless QEMU boot test              |
| `commands/screenshot.md`     | Slash command | Type `/screenshot [cmd]` — capture VGA as PNG      |
| `commands/debug.md`          | Slash command | Type `/debug <symptom>` — routes to the debugger   |
| `agents/kernel-debugger.md`  | Subagent      | Claude delegates crash diagnosis to it             |
| `skills/add-shell-command/`  | Skill         | Auto-invoked when adding a shell command           |
| `skills/add-syscall/`        | Skill         | Auto-invoked when adding a syscall                 |
| `skills/add-driver/`         | Skill         | Auto-invoked when adding a device driver           |
| `scripts/aios-screenshot.sh` | Helper        | Boot headless, type a cmd, screendump VGA → PNG    |
| `scripts/ppm2png.py`         | Helper        | stdlib PPM→PNG converter (no deps); used above     |

Notes:
- **CLAUDE.md is intentionally at the repo root**, not in here — that's where Claude
  Code auto-loads it from. Everything else stays under `.claude/`.
- **Skills** are matched by their `description:` — Claude pulls one in on its own when
  your request fits. **Commands** are things *you* type with a leading `/`.
- Edit any `.md` to tweak behavior; add a new `commands/<x>.md` for a new `/x`, or a
  new `skills/<x>/SKILL.md` for new procedural know-how.
