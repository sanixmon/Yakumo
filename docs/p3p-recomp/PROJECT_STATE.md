# PROJECT_STATE.md — Persona 3 Portable (P3P) Static Recompilation

> **Mandatory Entry Point**: Any AI agent or developer continuing this project MUST read this file first before inspecting or modifying any project files.

---

## 1. Project Objective & Explicit Non-Goals

### Objective
Develop a native static recompilation port of **Shin Megami Tensei: Persona 3 Portable** (PSP, `ULES-01523` / `ULUS-10512`) using the **TeamGDB/Yakumo** fork of `PSPRecomp`. Target runtime platforms: Linux/Steam Deck and Android (`arm64-v8a`), utilizing an offline MIPS Allegrex AOT recompiler, a clean HLE kernel, and native Vulkan rendering.

### Explicit Non-Goals
- **No Emulation Core Replacement**: Not building an interpreter/JIT-centric emulator.
- **No Scope Creep Before Baseline**: No renderer enhancements, texture packs, mod loaders, or audio synthesis before achieving clean, bit-level verified thread bootstrap.
- **No Copyrighted Asset Distribution**: Zero proprietary game files (ISOs, decrypted EBOOTs, textures, sounds) committed to Git. User must provide legally dumped media (*Bring Your Own Game*).
- **No Core Framework Pollution**: Zero game-specific hardcodes, memory addresses, or hooks in `src/` or `include/psprecomp/`. All game logic belongs strictly in `profiles/p3p/`.

---

## 2. Current Verified Repository State

- **Repository Root**: `/home/ubuntu/recompile/yakumo_repo`
- **Active Git Branch**: `feature/p3p-recomp` (fork: `sanixmon/Yakumo`)
- **Upstream HEAD Commit**: `5bacba315b5b151856adc80cd22b57ef6cf04308` (Merge pull request #341 from TeamGDB/sound-settings)
- **Local Working-Tree Status**: Clean — all P3P work committed on `feature/p3p-recomp` and pushed to the fork (`sanixmon/Yakumo`).
- **AOT Corpus (2026-10-23)**: Codegen complete — `profiles/p3p/generated/` (gitignored): 224 units + registry, 14,787 functions, 706,236 instructions, 221 import wrappers. Corpus compiles 225/225 objects with 0 errors via target `p3p_generated` in build tree `out/p3p-build` (Ninja, `PSPRECOMP_PROFILE=p3p`, `PSPRECOMP_GENERATED_JOBS=1`). The old `out/p3p` tree is STALE (its source path `/tmp/recomp_workspace` no longer exists) — do not use it.
- **Host Wiring Plan (2026-10-23)**: `docs/p3p-recomp/HOST_WIRING_PLAN.md`.
- **Gate 5B (2026-10-23)**: Early services implemented and unit-verified (`test_p3p_services` 57/57; regressions 44/44 + 128/128; `verify_boot.sh` PASS). PSP EABI argument convention established (ADR-007). Boot now passes the pre-I/O wall, completes heap + UMD/audio init, and settles into the game's CRI frame loop; CRI's loader thread never runs under the exit-only scheduler — proposed as Gate 5C (see GATES.md / EVIDENCE.md).
- **Gate 5C (2026-10-23)**: Cooperative scheduling implemented (ADR-008) — kernel `yield_current`/`block_current`/`sleep_current` on a fake clock with idle time-skip, shared priority picker with the exit hand-off; DelayThread, vblank waits and blocking audio outputs are timed sleeps; WaitEventFlag blocks for real (saved context re-dispatches the call on resume, poll form returns EVF_COND); mutexes are real objects with PPSSPP semantics and unlock hand-off; UMD reports an inserted, ready disc. Verified: `test_p3p_yield` 83/83, `test_p3p_services` 65/65, regressions 44/44 + 128/128, `verify_boot.sh` PASS. Boot now runs all CRI threads (user_main, SceWaveMain, 3x CriThread, CRI ADX Audio/File/Wave out) and serves the first file I/O: opens + lseek/read on `umd0.cpk` and `umd1.cpk`. **Gate 5's exit criterion is closed by this.** Next: Gate 6 (2D graphics to Vulkan) — requires explicit user authorization.

---

## 3. Current Gate & Milestone Status

- **Latest Completed Gate**: **Gate 5B (Early Kernel & Display Services)** — `VERDICT: PASS` (2026-10-23)
- **Current Gate Status**: **Gate 5 (Headless CPK Mount & File I/O) — IMPLEMENTED 2026-10-23; its boot-exit criterion is blocked by CRI worker-thread starvation, proposed as Gate 5C (awaiting authorization)**
- **Gate 4B Results (2026-10-23)**: AOT executable host `p3p` boots the ELF; `p3p_kernel` (RangeAllocator + thread lifecycle) and `p3p_hle_threadman` (Create/Start) implemented per the Gate 4A specification. Unit tests 128/128 pass. Oracle window `0x0880421C`–`0x08804260` matches PPSSPP (see EVIDENCE.md Gate 4B section; `profiles/p3p/scripts/verify_boot.sh` reproduces the full loop). Boot proceeds past Call #5 deep into game init and stops at a guest null dereference caused by a return-0 stub — Gate 5+ HLE territory, not a Gate 4B defect.
- **Gate 5 Outcome (2026-10-23)**: Media supplied by the user and verified byte-identical to the analyzed dump (EBOOT.BIN sha256 match); IoFileMgrForUser fully implemented and unit-verified (44/44). Boot reaches no I/O call yet: it stops at the pre-I/O wall (guest null deref, `recomp_unit_0165`, `0x08A9B7D8`) behind return-0 stubs for early services (sceKernelAllocPartitionMemory, sceGeEdramGetAddr, sceDisplayGetFrameBuf, sceKernelCreateCallback/CreateEventFlag, sceKernelGetModuleIdByAddress).
- **Recommended Next Step**: Gate 5C — cooperative thread yield (DelayThread and blocking waits hand off to the highest-priority Ready thread) so CRI's loader thread runs and boot reaches the first `sceIoOpen`, which Gate 5 has ready. Requires explicit user authorization.
- **Explicitly Prohibited in Next Step**:
  - NO changes to `src/` or `include/psprecomp/`.
  - NO renderer or audio work before its gate.
  - NO implementing full `ThreadMan` sync primitives beyond Gate 5B's authorized minimums (callbacks + event flags with UIDs; semaphores/mutexes beyond creation remain deferred).

---

## 4. Key Verified Facts vs Unverified Claims

| Parameter | Value | Status | Evidence Source |
|---|---|---|---|
| Decrypted ELF SHA-256 | `221b569fd8f01c8194bed47abcd69014251f518fa5b51ab3577f1d3f96522b26` | **VERIFIED** | `sha256sum /home/ubuntu/recompile/p3p_sys/EBOOT_DECRYPTED.ELF` |
| Encrypted EBOOT SHA-256 | `f2d4e66a3a373bde066921221e01bd217aa856ee78468a0185e237b66ddc0023` | **VERIFIED** | `sha256sum /home/ubuntu/recompile/p3p_sys/EBOOT.BIN` |
| Module Entry (`module_start`) | `0x08804108` | **VERIFIED** | ELF Header `e_entry` + `p3p_report.json` |
| Thread Entry (`user_main`) | `0x0880421C` | **VERIFIED** | Arg `$a1` in Call #3 + PPSSPP Seq #68 |
| First Game Init JAL | `0x08804260` | **VERIFIED** | Disassembly + PPSSPP Seq #82 |
| Observed P3P Boot Arglen | `27` (`0x1B`) bytes | **VERIFIED** | PPSSPP trace Seq #68 register `$a0` |
| P3P Module GP | `0x08C42A50` | **VERIFIED** | `p3p_report.json` ModuleInfo `gp` field |
| User RAM Allocation Start | `0x08EC1000` | **VERIFIED** | `align_up(load_image_end 0x08EC0F7C, 0x1000)` |
| Return Stub Contract ($ra) | `0x08800000` (`kThreadExitStub`) | **DESIGN DECISION** | Intentional AOT divergence from stack syscall |
| 85 Missing HLE NIDs | 61.5% implemented in Yakumo base | **UNVERIFIED AT RUNTIME** | Static name match; runtime behavior untested |

---

## 5. Files & Artifacts Checklist

Before resuming or modifying any code, verify existence of:
- [x] Decrypted ELF: `/home/ubuntu/recompile/p3p_sys/EBOOT_DECRYPTED.ELF`
- [x] Reference Emulator: `/home/ubuntu/recompile/ppsspp_repo/build/PPSSPPHeadless`
- [x] Recomp Runner: `/home/ubuntu/recompile/yakumo_repo/out/p3p/profiles/p3p/p3p_trace_runner`
- [x] Oracle Diff Tool: `/home/ubuntu/recompile/yakumo_repo/tools/trace/trace_diff.py`
- [x] Reference Trace: `/home/ubuntu/recompile/traces/ppsspp_p3p_trace.jsonl` and `/home/ubuntu/recompile/traces/ppsspp_p3p_calls.jsonl`
- [x] Analysis Data: `/home/ubuntu/recompile/p3p_analysis/p3p_report.json`
- [x] Generated AOT Corpus: `/home/ubuntu/recompile/yakumo_repo/profiles/p3p/generated/` (224 units + registry; gitignored, regenerable)
- [x] Corpus Build Tree: `/home/ubuntu/recompile/yakumo_repo/out/p3p-build/` (225/225 objects; `out/p3p` is stale — do not use)
- [x] AOT Executable Host: `/home/ubuntu/recompile/yakumo_repo/out/p3p-build/profiles/p3p/p3p` (plus `test_p3p_threading`; reproduce with `profiles/p3p/scripts/verify_boot.sh`)

---

## 6. Exact Resume Instructions for Next AI Session

To continue the project in a new session, run:
```bash
# 1. Enter repository
cd /home/ubuntu/recompile/yakumo_repo

# 2. Check git cleanliness
git status

# 3. Read project truth
cat docs/p3p-recomp/PROJECT_STATE.md

# 4. Review active gate specifications
cat docs/p3p-recomp/GATES.md
cat docs/p3p-recomp/reports/gate-4a-final-audit.md

# 5. Execute only authorized tasks for Gate 4B
```

---

## 7. Reusable Workflow Skill Assessment

A dedicated skill (e.g., `skill://psp-static-recomp-workflow`) is **RECOMMENDED FOR FUTURE SESSIONS** once Gate 4B passes and HLE development becomes iterative.
- **Value**: Automates pre-flight git checks, prevents accidental edits to `src/`, enforces JSONL trace diff verification on every build, and blocks premature gate progression.
- **Current Posture**: Keep repository markdown files (`docs/p3p-recomp/*`) as the primary ground truth. Do not generate or install a skill until the Gate 4B implementation baseline is reproducible.

---

## 8. Session-Handoff Protocol

### Mandatory Start-of-Session Procedure
1. **Read `PROJECT_STATE.md`**: Establish current context, scope, and rules.
2. **Inspect Git status and HEAD**: Run `git status` and `git log -1`.
3. **Read the active gate and ADRs**: Inspect `docs/p3p-recomp/GATES.md` and `docs/p3p-recomp/DECISIONS.md`.
4. **Verify artifacts exist**: Confirm required ELFs, tools, and traces from Section 5.
5. **Identify the single authorized task**: Do not expand scope.
6. **State blockers before editing**: If any prerequisite is missing, halt and report.

### Mandatory End-of-Session Procedure
1. **Summarize completed work**: State exactly what was analyzed or built.
2. **List changed paths**: Run `git status -s` and list all touched files.
3. **Record tests executed and raw results**: Include command and output.
4. **Record untested behavior**: Note what remains unverified.
5. **Update project state**: Reflect status changes in `PROJECT_STATE.md`, `GATES.md`, and `EVIDENCE.md`.
6. **Document next authorized task**: State clearly what the next session should do.
7. **Leave clean working tree**: Ensure no orphaned or intermediate artifacts linger uncommitted or untracked.
