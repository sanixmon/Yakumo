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
| **Gate 4B** | Threading HLE Implementation | **READY FOR AUTHORIZATION** | Pending User Review | Awaiting explicit approval to begin code implementation |
| **Gate 5** | Headless CPK Mount & File I/O | **NOT STARTED** | — | Prerequisite: Gate 4B PASS |
| **Gate 6** | 2D Graphics Engine to Vulkan | **NOT STARTED** | — | Prerequisite: Gate 5 PASS |
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
- **Status**: **READY FOR AUTHORIZATION** (NOT STARTED / LOCKED)
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
- **Outstanding Blockers**: Awaiting user's explicit command to begin implementation.
