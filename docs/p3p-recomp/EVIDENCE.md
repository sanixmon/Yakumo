# EVIDENCE.md — P3P Recompilation Reproducible Evidence Archive

This document compiles all independently verifiable evidence, binary hashes, trace records, test results, and source code locations.

---

## 1. Binary & Source Revisions

| Component | Path / Location | Commit / Checksum | Verification Method |
|---|---|---|---|
| **Yakumo Repo** | `/home/ubuntu/recompile/yakumo_repo` | `5bacba315b5b151856adc80cd22b57ef6cf04308` | `git log -1` |
| **PPSSPP Reference**| `/home/ubuntu/recompile/ppsspp_repo` | `a5275067b62433375bbde12921e96329a795dff0` | `git log -1` |
| **pspdecrypt** | `/home/ubuntu/recompile/pspdecrypt` | `c156627db7634d395c380c0a9589130f603307fc` | `git log -1` |
| **P3P EBOOT.BIN** | `/home/ubuntu/recompile/p3p_sys/EBOOT.BIN` | SHA-256: `f2d4e66a3a373bde066921221e01bd217aa856ee78468a0185e237b66ddc0023` | `sha256sum` |
| **P3P Decrypted ELF**| `/home/ubuntu/recompile/p3p_sys/EBOOT_DECRYPTED.ELF`| SHA-256: `221b569fd8f01c8194bed47abcd69014251f518fa5b51ab3577f1d3f96522b26` | `sha256sum` |
| **P3P PARAM.SFO** | `/home/ubuntu/recompile/p3p_sys/PARAM.SFO` | SHA-256: `ee963a3aa833a85b12628cb8d160cd7a0362beb352bdb26e1468582c32edc388` | `sha256sum` |

---

## 2. Static Analysis Evidence (`psp_analyze`)

- **Report Path**: `/home/ubuntu/recompile/p3p_analysis/p3p_report.json`
- **Command**: `/home/ubuntu/recompile/yakumo_repo/out/framework/psp_analyze /home/ubuntu/recompile/p3p_sys/EBOOT_DECRYPTED.ELF /home/ubuntu/recompile/p3p_analysis/p3p_report.json`
- **Key Metrics**:
  - `load_base`: `0x08804000`
  - `entry_runtime`: `0x08804108`
  - `relocations`: `178,513` total (`invalid 0`, `unsupported 0`)
  - `module_name`: `p3p`
  - `module_gp`: `0x08C42A50`
  - `module_version`: `1.1`
  - `functions_auto`: `14,787` functions
  - `unique_code_labels`: `706,236` words
  - `unique_block_entries`: `161,686`
  - `unsupported_instructions`: `6` occurrences (0.00085%)
  - `truncated_functions`: `0`
  - `import_entries`: `221` stubs across `22` libraries

---

## 3. Disassembly Checkpoints (`0x08804108`..`0x08804264`)

Dumped via `/home/ubuntu/recompile/yakumo_repo/out/framework/dump_function`:
```text
0x08804108  27BDFFE0  addiu $sp, $sp, -32        # module_start: alokasi stack 32 byte
0x0880410C  3C020602  lui   $v0, 0x0602          # $v0 = 0x06020000 (SDK 6.20)
0x08804114  00809821  addu  $s3, $a0, $zero      # $s3 = argc (27)
0x08804120  00A09021  addu  $s2, $a1, $zero      # $s2 = argv pointer
0x08804130  AFBF0010  sw    $ra, 16($sp)
0x08804134  0E2DFF03  jal   0x08B7FC0C           # Call #1: sceKernelSetCompiledSdkVersion600_602
0x08804138  26110000  addiu $s0, $s1, 0          # [delay slot]
0x0880413C  3C020003  lui   $v0, 0x0003          # $v0 = 0x00030000
0x08804140  0E2DFF01  jal   0x08B7FC04           # Call #2: sceKernelSetCompilerVersion
0x08804144  34440306  ori   $a0, $v0, 0x0306     # [delay slot] $a0 = 0x00030306
...
0x088041D0  3C110880  lui   $s1, 0x0880          # $s1 = 0x08800000
0x088041D4  2625421C  addiu $a1, $s1, 0x421C     # $a1 = 0x0880421C (ENTRY THREAD BARU)
0x088041D8  0E2DFF2D  jal   0x08B7FCB4           # Call #3: sceKernelCreateThread
0x088041DC  00004821  addu  $t1, $zero, $zero    # [delay slot]
0x088041E0  00402021  addu  $a0, $v0, $zero      # $a0 = $v0 (UID thread yang dibuat)
0x088041E4  02602821  addu  $a1, $s3, $zero      # $a1 = $s3 (argc diteruskan)
0x088041E8  0E2DFF1D  jal   0x08B7FC74           # Call #4: sceKernelStartThread
0x088041EC  02403021  addu  $a2, $s2, $zero      # [delay slot] $a2 = $s2 (argv diteruskan)
0x088041F0  8FBF0010  lw    $ra, 16($sp)         # Pulihkan $ra (kThreadExitStub)
0x08804208  03E00008  jr    $ra                  # Loader thread SELESAI, keluar ke exit stub!
0x0880420C  27BD0020  addiu $sp, $sp, 32         # [delay slot]

=== ENTRY THREAD BARU (user_main) ===
0x0880421C  3C030000  lui   $v1, 0x0000          # Entry point thread user_main
0x08804220  27BDFC00  addiu $sp, $sp, -1024      # Alokasi stack frame 1024 byte
0x08804224  24660000  addiu $a2, $v1, 0          # $a2 = 0
0x08804228  AFB503F4  sw    $s5, 1012($sp)
0x0880422C  27B50060  addiu $s5, $sp, 96
0x08804230  AFB303EC  sw    $s3, 1004($sp)
0x08804234  00809821  addu  $s3, $a0, $zero      # $s3 = $a0 (arglen)
0x08804238  AFB203E8  sw    $s2, 1000($sp)
0x0880423C  00A09021  addu  $s2, $a1, $zero      # $s2 = $a1 (argp)
0x08804240  AFBF03F8  sw    $ra, 1016($sp)       # Simpan $ra
0x08804244  AFB403F0  sw    $s4, 1008($sp)
0x08804248  AFB103E4  sw    $s1, 996($sp)
0x0880424C  10C00004  beq   $a2, $zero, +4       # Percabangan ke 0x08804260!
0x08804250  AFB003E0  sw    $s0, 992($sp)        # [delay slot]
0x08804260  0E2D39A8  jal   0x08B4E6A0           # Call #5: Pemanggilan game init pertama!
```

---

## 4. Emulator Oracle Trace Evidence (`PPSSPPHeadless`)

- **Trace File**: `/home/ubuntu/recompile/traces/ppsspp_p3p_trace.jsonl` (Instruction-level, 1,395,523 lines)
- **Call Trace File**: `/home/ubuntu/recompile/traces/ppsspp_p3p_calls.jsonl` (Call-level, 457,259 lines)
- **Sequence #68 (First instruction of `user_main` at `0x0880421C`):**
```json
{
  "sequence": 68,
  "pc_guest": "0x0880421c",
  "sp": "0x0bfffaa0",
  "a0": "0x0000001b",
  "a1": "0x0bfffae0",
  "a2": "0xdeadbeef",
  "a3": "0xdeadbeef",
  "v0": "0x00000000",
  "v1": "0xdeadbeef",
  "event": "instruction"
}
```
- **Sequence #82 (Call #5 at `0x08804260`):**
```json
{
  "sequence": 82,
  "pc_guest": "0x08804260",
  "sp": "0x0bfff6a0",
  "a0": "0x0000001b",
  "a1": "0x0bfffae0",
  "a2": "0x00000000",
  "a3": "0xdeadbeef",
  "v0": "0x00000000",
  "v1": "0x00000000",
  "event": "call"
}
```

---

## 5. Trace Diff Tool Validation (`trace_diff.py`)

- **Tool Path**: `/home/ubuntu/recompile/yakumo_repo/tools/trace/trace_diff.py`
- **Validation Results**:
  1. Identical Logs: Exit code `0` (`PASS`)
  2. Register Mismatch Only: Exit code `1` (`STATE_MISMATCH`, isolates `$a0`)
  3. Control Flow PC Mismatch Only: Exit code `1` (`CONTROL_FLOW_DIVERGENCE`, isolates PC)
  4. Early EOF / Length Mismatch: Exit code `1` (`LENGTH_MISMATCH`, drains remaining lines)

---

## 6. Source Inspections

- `src/guest_memory.cpp:115-130`: `GuestMemory::contains` and `resolve` boundary checks.
- `src/runtime.cpp:680-705`: Direct function dispatch lookup for virtual stubs.
- `src/runtime.cpp:775-1032`: Hardcoded MHP3rd debug address leaks (`0x08...`) flagged.
- `profiles/mhp3rd/host/kernel/kernel.cpp:134-185`: TCB layout, stack filling, loader thread start.
- `profiles/mhp3rd/host/kernel/kernel.cpp:758-835`: RangeAllocator `allocate_block` / `free_block`.

---

## Gate 4B Implementation Evidence (2026-10-23)

### Implementation (all under `profiles/p3p/`, ADR-004 respected; no `src/` changes)
- `host/kernel/p3p_kernel.hpp` & `.cpp`: coalescing `RangeAllocator` (bottom-up/top-down) + `P3PKernel` thread lifecycle + cooperative scheduler hand-off.
- `host/hle/p3p_hle_threadman.hpp` & `.cpp`: `ThreadManForUser` NIDs `0x446D8DE6` (CreateThread) and `0xF475845D` (StartThread).
- `host/p3p_profile.hpp` & `.cpp`: import wiring (HLE bindings for the AOT wrappers, stub-address functions for the interpreter, diagnostic return-0 stubs that log once per NID), exit stub at `0x08800000`.
- `host/main.cpp`: executable `p3p` linking the `p3p_generated` AOT corpus; `--interpret` mode for oracle tracing.
- `tests/test_p3p_threading.cpp`; `scripts/verify_boot.sh`; `scripts/window_diff.py`.

### Reference-kernel fidelity (verified against `ppsspp_repo/Core/HLE/sceKernelThread.cpp`)
- `PSPThread::FillStack`: stack filled `0xFF` at create; UID written at stack bottom; `$k0` TCB fields: UID at `+0xC0`, stack base at `+0xC8`, `0xFFFFFFFF` at `+0xF8`/`+0xFC`.
- `__KernelStartThread`: `$sp -= aligned arglen`, arguments copied, `$sp -= 64`; `$fp` = initial `$sp`.
- Registers the kernel does not set are architecturally undefined; the PPSSPP oracle observes them as the Gate 3 boot poison `0xDEADBEEF`, except `$v0`, which is observed `0` at thread entry. The P3P kernel mirrors the observed values.
- Boot argument is 27 bytes: `/PSP_GAME/SYSDIR/EBOOT.BIN` plus its NUL terminator. (`tools/p3p_trace_runner` used a 32-byte `disc0:/...` string; corrected in `host/main.cpp`.)

### Test results (real terminal output)
- `test_p3p_threading`: **128 checks, 0 failures** (Gate 4A boundary table, allocator invariants incl. coalescing + double-free, start failure-atomicity matrix, lifecycle + scheduler hand-off, HLE cores).
- Window diff (`profiles/p3p/scripts/window_diff.py` vs `traces/ppsspp_p3p_trace.jsonl`): `window: ref 15 instructions, recomp 14 instructions` — the single ref-only line is the branch delay slot at `0x08804250`, which the recomp interpreter executes inside the branch without a separate log line; all compared registers match (`pc`, `a0`, `a2`, `a3`, `v0`, `v1`, `gp` exact; `sp`/`a1` stack-relative offsets exact). **WINDOW RESULT: PASS.** The window's last instruction is the Call #5 `jal` at `0x08804260` on both runtimes, and the following instruction is `0x08B4E6A0` on both.
- AOT boot: `relocated 178513 entries`, `registered 221 import bindings`; loader → user_main hand-off observed live (later crash dump shows `$sp = 0x09FEF8B0` inside user_main's stack block, `$k0 = 0x09FEFF00` = its TCB, `$fp = 0x09FEFEA0` = initial `$sp`). Execution proceeds deep into game init (sceGe/sceDisplay import stubs) and stops at `Guest memory access outside PSP RAM/EDRAM at 0x00000000` from `recomp_unit_0165` (guest_pc `0x08A9B7D8`) — a guest null dereference downstream of a return-0 stub, Gate 5+ scope.
- Full verification loop: `profiles/p3p/scripts/verify_boot.sh` exits 0.

### Known simplifications / divergences (documented; ADR-002, ADR-003, ARCHITECTURE §6)
- `$ra` exit stub at `0x08800000` instead of a stack-resident syscall stub (ADR-002); PPSSPP executes two trampoline instructions (`0x08000030`/`0x08000034`) during the switch that have no recomp counterpart.
- Dynamic UIDs from 100 instead of PPSSPP's observed `0x114` (ADR-003).
- 32 MiB RAM model: stack bases differ from the 64 MiB PPSSPP trace by design; every stack-relative offset matches.
- User partition clamped to the RAM end `0x0A000000` (`0x113F000` bytes), not the `0x0A800000` end stated in ARCHITECTURE.md — that figure assumes the 64 MiB model. Flagged for a future documentation correction.
- `sceKernelStartThread` does not immediately switch to a strictly higher-priority thread (PPSSPP does). The boot path is unaffected (equal priorities; switch happens at loader exit per ARCHITECTURE §5). Deferred beyond Gate 4B.

## Gate 5 — IoFileMgr File I/O (2026-10-23)

**Status: IMPLEMENTED and unit-verified.** Boot integration: the boot does not reach any IoFileMgr call yet — it stops at the same pre-I/O wall as Gate 4B (see "Boot evidence"). The plan's boot exit criterion ("first `sceIoOpen` served") is pending an earlier-services gate; this is not an I/O defect.

### Media (verified 2026-10-23)

- User-supplied dump `~/recompile/p3p_media/ULES-01523.iso` (1,321,893,888 bytes, kept outside git). PARAM.SFO DISC_ID = `ULES01523`.
- EBOOT.BIN extracted from the ISO: sha256 `f2d4e66a3a373bde066921221e01bd217aa856ee78468a0185e237b66ddc0023` — byte-identical to the analyzed dump of PROJECT_STATE section 4. The media matches the recompiled corpus exactly.
- Extracted with 7z into `profiles/p3p/game/` (gitignored): `PSP_GAME/USRDIR/` with `umd0.cpk` (553,718,280 B), `umd1.cpk` (338,630,752 B), `MEMST.CPK` (269,667,104 B), `module/{libccc,libfont,libsuppreacc}.prx`, `sound/`.

### Implementation

- `profiles/p3p/host/hle/p3p_hle_io.{hpp,cpp}`: all 20 IoFileMgrForUser NIDs (names fully resolved, incl. `0x71B19E77` = sceIoLseekAsync and `0xFF5940B6` = sceIoCloseAsync, per PPSSPP `Core/HLE/sceIo.cpp`). Registered through a dispatch branch in `p3p_profile.cpp`; `main.cpp` gained `--game-root` (`Runtime::set_game_root`).
- Semantics: fd/dir handle tables in the profile; `disc0:` read-only, `ms0:` writable under the same single game root; case-insensitive path fallback after `translate_path`; `SceIoStat`/`SceIoDirEnt` filled per PPSSPP layouts; PPSSPP error codes (ErrorCodes.h + errno mapping); the async family completes synchronously, results consumed via Poll/Wait/WaitCB; CPK parsing stays in guest code (the host serves bytes only).

### Unit Verification (2026-10-23)

- `profiles/p3p/scripts/verify_io.sh`: build clean.
- `test_p3p_io`: **44 checks, 0 failures** — open/read/seek (SET/CUR/END, 64-bit)/getstat, traversal rejection, case-insensitive open, directory walk (`.`/`..`/entries), async open/read/close cycle with NO_ASYNC on re-poll, ms0 create/write/rename/read-back, disc0 write open rejected with `0x8001001E`.
- `test_p3p_threading` regression: **128 checks, 0 failures**.

### Boot Evidence (AOT, `--game-root profiles/p3p/game`, 50M dispatches)

- **Zero IoFileMgr calls occur before the stop.** The stop is identical to the Gate 4B wall: guest memory access at `0x00000000`, `recomp_unit_0165`, guest_pc `0x08A9B7D8`.
- The 22 stub imports fired before the stop, all resolved by name against PPSSPP tables: sceKernelSetCompiledSdkVersion600_602, sceKernelSetCompilerVersion, **sceKernelAllocPartitionMemory**, **sceKernelGetModuleIdByAddress**, sceKernelCpuSuspendIntr, sceKernelCpuResumeIntr, sceImposeSetLanguageMode, **sceKernelCreateCallback**, sceKernelRegisterExitCallback, sceUtilityLoadModule, sceKernelChangeCurrentThreadAttr, **sceGeEdramGetAddr**, **sceKernelCreateEventFlag**, sceGeSetCallback, sceGeListEnQueue, sceGeListSync, sceGeDrawSync, sceGeListUpdateStallAddr, sceDisplaySetMode, sceDisplayWaitVblankStartMultiCB, **sceDisplayGetFrameBuf**, sceDisplaySetFrameBuf.
- Load-bearing return-0 stubs (semantically wrong as 0): `sceKernelAllocPartitionMemory` (block UID), `sceGeEdramGetAddr` (must return the EDRAM address `0x04000000`), `sceDisplayGetFrameBuf` (out-parameters never written), `sceKernelCreateCallback` / `sceKernelCreateEventFlag` (UIDs).
- **CORRECTION:** `ModuleMgrForUser::0xD8B73127` is `sceKernelGetModuleIdByAddress`, not `sceKernelLoadModule` as earlier documents (Gate 4B notes, GATE5_PLAN) assumed. No LoadModule call occurs on the boot path up to the wall; the only module-related call seen is `sceUtilityLoadModule`. The plan's WP3 module-loading concern is not on the current critical path.

### Verdict

Gate 5's deliverable — the mounted game tree and a complete IoFileMgrForUser — is in place and verified in isolation. Boot progression past init now depends on minimal implementations of the early services listed above (SysMem partition allocation can reuse the kernel's existing RangeAllocator). Recommended as the next gate's scope.

## Gate 5B — Early Kernel & Display Services (2026-10-23)

### Scope & Authorization

Authorized by the user ("kerjakan sesuai ard") following Gate 5's verdict. Scope: the minimal early services the boot path hits before any file I/O — SysMem partition allocation, ThreadMan callbacks/event flags, Ge/Display state with truthful return values, and minimum ModuleMgr/Kernel_Library/Utility/Impose behaviour. All code lives in `profiles/p3p/` (ADR-004); object UIDs come from the kernel's single monotonic counter (ADR-003).

### Implementation

- **Kernel** (`p3p_kernel.{hpp,cpp}`): `RangeAllocator::allocate_at` (PSP_SMEM_Addr); partition-block registry (`alloc/free_partition_memory`, `block_head_address`) over the existing coalescing allocator; callback registry (create/delete/notify/check for the current thread); event-flag registry (create/delete/set/clear/pattern); `change_current_thread_attr`; main-module span registry backing `sceKernelGetModuleIdByAddress`.
- **`p3p_hle_sysmem.cpp`**: AllocPartitionMemory (all five PSP_SMEM types), FreePartitionMemory, GetBlockHeadAddr, sceKernelPrintf (logged), SDK/compiler version setters.
- **`p3p_hle_threadman.cpp`** (extended): CreateCallback/DeleteCallback/CheckCallback, CreateEventFlag/Set/Clear/Delete, WaitEventFlag(+CB) with PSP wait/clear modes (an unsatisfied wait is logged and completed non-blockingly — headless approximation), ChangeCurrentThreadAttr, GetSystemTimeLow/DelayThread over a fake clock that always advances, ExitThread via the kernel hand-off.
- **`p3p_hle_gedisplay.cpp`**: sceGe EdramGetAddr (`0x04000000`)/EdramGetSize (2 MiB), list & callback ids, ListSync/DrawSync report completion; sceDisplay mode/framebuf record, GetFrameBuf writes the recorded values, vblank waits advance the vcount, GetFramePerSec returns 59.94.
- **`p3p_hle_services.cpp`**: sceImposeSetLanguageMode; sceUtility Load/UnloadModule + a savedata/msgdialog status state machine (NONE→INIT_STARTED→RUNNING→FINISHED, SHUTDOWN_STARTED→NONE); LoadExec RegisterExitCallback/ExitGame; ModuleMgr GetModuleIdByAddress only (LoadModule family stays stubbed — not on the boot path, per Gate 5's correction); Kernel_Library CpuSuspendIntr/CpuResumeIntr state, Memcpy/Memset through guest memory, GetThreadId, LwMutex no-ops.

### The EABI Correction (ADR-007)

The gate's first boot rejected `AllocPartitionMemory('UserSbrk', 16 MiB, LowAligned)`: the alignment was read from `$sp+16` (o32 assumption) and contained stack garbage. Evidence chain: guest-memory disassembly of the caller shows the alignment (4096) loaded into `$t0` in the jal delay slot (`0x08B4E704`); PPSSPP's instruction trace shows the identical call succeeding (UID `0x115`); PPSSPP's HLE source documents the MIPS EABI argument layout (ADR-007). Three reads were corrected to `$t0` — AllocPartitionMemory's address, **CreateThread's attr (a latent Gate 4B bug: it had consumed a coincidentally usable stack value)**, and IoLseek's whence — and the three unit tests now pass those arguments in `$t0`. After the fix `verify_boot.sh` still reports the Gate 4B oracle window PASS.

### Verification

- `test_p3p_services`: **57 checks, 0 failures** — partition alloc (Low/High/Addr, head alignment, NO_MEMORY, UNKNOWN_UID on re-free), printf, callback UID + notify/check, event-flag set/clear/wait incl. WAITCLEARPATTERN, thread attr mutation, fake-clock advance, Ge EDRAM constants + list ids, Display framebuf record round-trip + vcount, ModuleMgr in/out-of-image, Kernel_Library memcpy/memset/intr state/GetThreadId, Utility dialog state machine.
- Regressions: `test_p3p_io` **44/44**, `test_p3p_threading` **128/128**, `scripts/verify_boot.sh` **OK**.

### Boot Evidence (AOT, `--game-root profiles/p3p/game`)

- `sceKernelAllocPartitionMemory('UserSbrk', 0x01000000)` → **UID 103, head `0x08EC1000`**. The Gate 4B/5 null-dereference wall (`0x08A9B7D8`) is gone.
- Init proceeds through: `sceUtilityLoadModule(0x300/0x302)`, the UMD sequence (`sceUmdRegisterUMDCallBack`, `sceUmdCheckMedium`, `sceUmdWaitDriveStat`, `sceUmdActivate`), ThreadMan `CreateMutex`/`CreateLwMutex`/`GetThreadCurrentPriority`/`ChangeThreadPriority`, and audio setup (`sceAudioChReserve`, `sceAudioChangeChannelConfig`, `sceAudioSetChannelDataLen`) — all still return-0 stubs, none load-bearing yet.
- Execution then settles into an indefinite frame cycle and issues **no further imports and no `sceIoOpen`**. Diagnosis from a 37,028,985-instruction interpreter oracle trace: the pc distribution is identical 10M instructions apart; the cycle is ~900 instructions through engine/CRI utility functions (`0x08B756xx`–`0x08B761xx`, called from `0x08B485xx`/`0x08B779xx`/`0x08B7Bxxx`) on a single thread (sp `0x09FEF8xx`); a string referenced in the cycle (`0x08B95810`) is the CRIWARE banner (`CRI Base/PSP Ver.2.06`). The loop is the game pumping CRI server functions each frame; CRI performs CPK loading asynchronously on **its own ThreadMan thread**, which the Gate 4B exit-only cooperative scheduler never runs while `user_main` loops. Loads therefore never start and no thread ever issues `sceIoOpen`.

### Verdict

**Gate 5B: PASS.** Every service in scope is implemented and unit-verified; boot advances past the entire pre-I/O wall into engine init. Reaching the first `sceIoOpen` is no longer an HLE-value problem but a scheduler-coverage one — proposed as **Gate 5C (cooperative thread yield)**, awaiting authorization.
