# GATES.md — Project Gate Ledger

This document tracks authorization, entry criteria, exit criteria, and status for each phase gate of the Persona 3 Portable recompilation project.

---

## Gate Ledger Summary

| Gate | Name | Status | Authorized By | Outstanding Blockers |
|---|---|---|---|---|
| **Gate 0** | Premise & Upstream Validation | **PASS** | User | None |
| **Gate 1** | Base Environment & Profile Setup | **PASS SEBAGIAN** | User | MHP3rd debug address leaks in `src/runtime.cpp` flagged |
| **Gate 2** | Binary Decryption & Static Analysis | **PASS SEBAGIAN** | User | 85 unverified runtime HLE NIDs |
| **Gate 3** | Oracle Trace Infrastructure | **PASS SEBAGIAN** | User | Pre-Seq #5 state divergence observed |
| **Gate 3A** | Trace Semantic Alignment | **PASS** | User | None (Divergence proven at Call #3 UID return) |
| **Gate 4A** | Minimal Threading Specification (R1-R3) | **PASS / AUDIT COMPLETE** | User | None (All 4 blockers mathematically resolved) |
| **Gate 4B** | Threading HLE Implementation | **PASS** | User (2026-10-23) | None |
| **Gate 5** | Headless CPK Mount & File I/O | **PASS** | User (2026-10-23) | Prerequisite: Gate 4B PASS. IoFileMgr implemented + unit-verified 44/44 (2026-10-23). The boot exit criterion (first `sceIoOpen` served) was blocked upstream by scheduler coverage and stub-era UMD/mutex semantics; it was satisfied 2026-10-23 under Gate 5C: boot opens and reads `disc0:/PSP_GAME/USRDIR/umd0.cpk` and `umd1.cpk` |
| **Gate 5B** | Early Kernel & Display Services | **PASS** | User (2026-10-23) | None. SysMem partition blocks over the kernel allocator, callback/event-flag UIDs, Ge/Display state with real return values, ModuleMgr/Kernel_Library/Utility minimums; 57/57 new unit checks, regressions green, verify_boot PASS. EABI argument convention recorded as ADR-007 |
| **Gate 5C** | Cooperative Scheduling & Sync Semantics (CRI Worker Progress) | **PASS** | User (2026-10-23) | Kernel yield/block plus timed sleeps on a fake clock (DelayThread, vblank waits, audio output drains) with idle time-skip; blocking WaitEventFlag via save-and-re-dispatch; real mutexes (PPSSPP semantics, unlock hand-off, priority attr); UMD medium semantics (CheckMedium = 1, drive state PRESENT\|READY\|READABLE); sceAudio blocking outputs as scheduling points. `test_p3p_yield` 83/83, services 65/65, regressions 44/44 + 128/128, verify_boot PASS. Boot: CRI threads execute and the first `sceIoOpen`/`sceIoLseek`/`sceIoRead` on both CPK archives are served (EVIDENCE Gate 5C; design recorded as ADR-008) |
| **Gate 6** | 2D Graphics Engine to Vulkan | **IN PROGRESS** (WP1 done) | User (2026-10-23) | Plan: GATE6_PLAN.md. WP1 (display-list executor) DONE: lists really execute (state, stall/resume, signals, draw recording), `test_p3p_ge` 19/19, boot lists decode. WP2 (software 2D rasterizer + frame dump) is blocked upstream: the boot now spins in a LoadModule/StartModule/UnloadModule retry loop (~1.8M iterations) on `USRDIR/module/libsuppreacc.prx` — PRX loading is still fake (Gate 5-era ModuleMgr). Proposed: a Gate 5D mini-gate for real module loading before WP2 |
| **Gate 7** | Audio Synthesis (CRI / SAS) | **NOT STARTED** | — | Prerequisite: Gate 6 PASS |
| **Gate 8** | 3D Tartarus & Battle Logic | **NOT STARTED** | — | Prerequisite: Gate 7 PASS |
| **Gate 9** | Android APK Packaging & Testing | **NOT STARTED** | — | Prerequisite: Gate 8 PASS |

---

## Detailed Gate Audits

### Gate 4A: Minimal Threading Specification (R1, R2, R3)
- **Status**: **PASS / SPECIFICATION FINALIZED**
- **Authorized By**: User (Instructions on Gate 4A-R1, R2, R3)
- **Entry Criteria**: Gate 3A PASS; disassembly of `module_start` and `user_main` available.
- **Exit Criteria**:
  1. Stack layout formulas mathematically proven non-overlapping.
  2. Pre-alignment integer overflow protection specified.
  3. Contradiction between PPSSPP stack `$ra` and Recomp native stub documented as intentional architectural divergence.
  4. Failure-atomicity matrix for `sceKernelStartThread` fully defined.
  5. P3P RangeAllocator invariants formally proven.
- **Evidence**: `reports/gate-4a-final-audit.md`
- **Blockers**: None.

---

### Gate 4B: Threading HLE Implementation
- **Status**: **PASS** (Implemented and verified 2026-10-23; see EVIDENCE.md, Gate 4B section)
- **Authorized Scope**:
  - `profiles/p3p/host/kernel/p3p_kernel.hpp` & `.cpp`
  - `profiles/p3p/host/hle/p3p_hle_threadman.cpp`
  - `profiles/p3p/tests/test_p3p_threading.cpp`
- **Entry Criteria**: Explicit user authorization of Gate 4A-R3 specification.
- **Exit Criteria**:
  1. `test_p3p_threading` unit tests pass 100%.
  2. Trace diff against PPSSPP verifies 100% MATCH from `0x0880421C` to `0x08804260`.
  3. Call #5 (`0x08804260`) dispatched identically on both runtimes.
- **Evidence Required**: Real terminal test logs, `trace_diff.py` output with 0 divergences up to Call #5.
- **Outstanding Blockers**: None.
