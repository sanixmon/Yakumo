# ROADMAP.md — P3P Static Recompilation Roadmap

This roadmap defines the strict phase-gate lifecycle for porting Persona 3 Portable to native x86_64 and Android arm64.

---

## Gate Overview & Status Ledger

```text
[Gate 0: Premise & Upstream Validation]       -> [PASS] (Verified 2026-10-19)
[Gate 1: Base Environment & Profile Setup]   -> [PASS SEBAGIAN] (Verified 2026-10-19)
[Gate 2: Binary Decryption & Analysis]       -> [PASS SEBAGIAN] (Verified 2026-10-19)
[Gate 3: Oracle Trace Infrastructure]        -> [PASS SEBAGIAN] (Verified 2026-10-19)
[Gate 3A: Trace Semantic Alignment]          -> [PASS] (Verified 2026-10-19)
[Gate 4A: Minimal Threading Specification]   -> [PASS / AUDIT COMPLETE] (Verified 2026-10-19)
[Gate 4B: Threading HLE Implementation]      -> [PASS] (Verified 2026-10-23)
[Gate 5: Headless CPK Mount & File I/O]      -> [NOT STARTED]
[Gate 6: 2D Graphics Engine to Vulkan]       -> [NOT STARTED]
[Gate 7: Audio Synthesis (CRI / SAS)]        -> [NOT STARTED]
[Gate 8: 3D Tartarus & Turn-Based Battle]    -> [NOT STARTED]
[Gate 9: Android APK Packaging & Testing]    -> [NOT STARTED]
```

---

## Detailed Gate Specifications

### Gate 0: Premise & Upstream Validation
- **Objective**: Verify existence, activity, and rendering capability of `jessicanataliagta/PSPRecomp` and `TeamGDB/Yakumo`. Check for duplicate projects.
- **Prerequisites**: Network access to GitHub repositories.
- **Authorized Scope**: Read-only repo inspection and commit logging.
- **Prohibited Work**: Any code modifications.
- **Acceptance Criteria**: Framework builds; at least one playable profile demonstrated; no active public P3P recomp project.
- **Current Status**: **PASS** (Commit `5bacba315b5b`, Yakumo playable, no P3P duplicate).

---

### Gate 1: Base Environment & Profile Setup
- **Objective**: Build clean host analyzer/recompiler tools; verify Android arm64 cross-compilation toolchain; scaffold clean `profiles/p3p/`.
- **Prerequisites**: CMake $\ge 3.20$, GCC $\ge 13$, Android NDK $\ge$ r28.
- **Authorized Scope**: Toolchain tests, creating empty `profiles/p3p/` directory tree, adding `.gitignore` rules.
- **Prohibited Work**: Committing game assets or modified core code.
- **Acceptance Criteria**: `psp_analyze` and `psp_recomp` compiled for host; `psprecomp_core` compiled for `arm64-v8a`; `profiles/p3p` builds clean static stub.
- **Current Status**: **PASS SEBAGIAN** (Tools and cross-compile verified with NDK r28; flagged MHP3rd debug address leaks in `src/runtime.cpp`).

---

### Gate 2: Binary Decryption & Static Analysis
- **Objective**: Decrypt user-provided `EBOOT.BIN` to `EBOOT_DECRYPTED.ELF`; run static analyzer; evaluate function count, VFPU usage, and NID coverage.
- **Prerequisites**: Legally dumped ISO from user's UMD media.
- **Authorized Scope**: Decryption via `pspdecrypt`; running `psp_analyze` and `dump_function`.
- **Prohibited Work**: Downloading commercial ISOs from third-party sites; committing decrypted ELFs.
- **Acceptance Criteria**: SHA-256 hashes recorded; analyzer generates `p3p_report.json`; VFPU and unknown instruction rates measured.
- **Current Status**: **PASS SEBAGIAN** (ELF decrypted cleanly, 14,787 functions, 0.02% VFPU, 136 of 221 NIDs implemented in base, 85 missing NIDs documented).

---

### Gate 3 & Gate 3A: Oracle Trace Infrastructure & Semantic Alignment
- **Objective**: Build `PPSSPPHeadless` on host; attach non-intrusive JSONL tracing hooks to emulator and recomp runner; verify diff tool; isolate first divergence.
- **Prerequisites**: Compiled `PPSSPPHeadless`; decrypted ELF; equalized initial CPU state.
- **Authorized Scope**: Adding trace hooks gated by environment variables; writing `trace_diff.py`; comparing initial 1000 dispatches.
- **Prohibited Work**: Implementing new HLE functions to force artificial pass.
- **Acceptance Criteria**: Diff tool detects synthetic and real divergence; first 4 calls match 100%; divergence at Call #5 diagnosed via disassembly.
- **Current Status**: **PASS** (Disassembly proves Call #3 `sceKernelCreateThread` returns UID 0 on stub vs 0x114 on PPSSPP, causing Call #5 control flow split).

---

### Gate 4A: Minimal Threading Specification (R1, R2, R3)
- **Objective**: Mathematically formulate stack layout; verify `$sp` and `$a1` relationships; document `$ra` architectural divergence; define atomic failure matrix; specify P3P RangeAllocator.
- **Prerequisites**: Gate 3A completion; disassembly of `0x08804108..0x08804260`.
- **Authorized Scope**: Audit and specification documentation ONLY.
- **Prohibited Work**: Any source code modifications or HLE implementation.
- **Acceptance Criteria**: Non-overlapping stack bounds mathematically proven; integer overflow checks specified; allocator invariants verified; all blockers resolved.
- **Current Status**: **PASS / SPECIFICATION FINALIZED** (`reports/gate-4a-final-audit.md`).

---

### Gate 4B: Threading HLE Implementation
- **Objective**: Implement minimal `p3p_kernel` and `ThreadManForUser` HLE stubs (`sceKernelCreateThread` and `sceKernelStartThread`) in `profiles/p3p/`. Verify execution reaches `user_main` at `0x0880421C` and calls `0x08804260`.
- **Prerequisites**: Explicit user authorization of Gate 4A specification.
- **Authorized Scope**:
  1. `profiles/p3p/host/kernel/p3p_kernel.hpp` & `.cpp`
  2. `profiles/p3p/host/hle/p3p_hle_threadman.cpp`
  3. `profiles/p3p/tests/test_p3p_threading.cpp`
- **Prohibited Work**:
  - Modifying `src/`, `include/psprecomp/`, or `profiles/mhp3rd/`.
  - Implementing semaphores, mutexes, event flags, callbacks.
  - Advancing PC directly to `0x08804260` without executing `0x0880421C..0x0880425C`.
- **Acceptance Criteria**:
  1. Unit tests pass for lifecycle and parameter validation errors.
  2. Recomp trace matches PPSSPP instruction-by-instruction from `0x0880421C` to `0x08804260`.
  3. Call #5 (`jal 0x08B4E6A0`) dispatched identically on both runtimes.
- **Current Status**: **PASS** (Implemented and verified 2026-10-23: unit tests 128/128, oracle window 0x0880421C–0x08804260 match, Call #5 dispatched identically. See EVIDENCE.md Gate 4B section.)

---

### Future Gates (Summary)
- **Gate 5 (File I/O)**: Virtual disc mounting, reading `umd0.cpk`, implementing `IoFileMgrForUser`.
- **Gate 6 (2D Engine)**: Display list handling, font rasterization, 2D UI presentation via Vulkan.
- **Gate 7 (Audio)**: CRI ADX streaming, `sceSasCore` ADPCM and reverb synthesis.
- **Gate 8 (3D Engine)**: Tartarus 3D geometry, GMO model transform, camera controls.
- **Gate 9 (Android Packaging)**: Building APK, touch controls, validation on real ARM64 phone.
