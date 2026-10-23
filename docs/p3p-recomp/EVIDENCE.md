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


---

## Gate 5C — Cooperative Scheduling & Sync Semantics (2026-10-23)

### What was built

- **Kernel scheduling** (`p3p_kernel`): `ThreadStatus::Waiting`, per-thread
  wait records (event flag / mutex), `yield_current` / `block_current` /
  `sleep_current` over a shared priority picker (the exit hand-off's rule:
  highest priority, then lowest UID), a fake clock (`now_us_`) with idle
  time-skip to the next timed wake (also applied on the exit path).
- **Timed sleeps as scheduling points**: `sceKernelDelayThread` sleeps for
  the delay; sceDisplay vblank waits (`WaitVblankStart`, `WaitVblank`,
  `WaitVblankStartMultiCB`) sleep to the next frame boundary; the sceAudio
  blocking outputs (`p3p_hle_audio`: OutputBlocking, OutputPannedBlocking,
  Output2OutputBlocking, SRCOutputBlocking) sleep to the next frame
  boundary as a drain model. Channel calls return real values
  (ChReserve returns the channel, `-1` → 0; Output2 rest sample 0).
- **Blocking waits**: `sceKernelWaitEventFlag` blocks for real — the saved
  context stays on the call and re-dispatches on resume; satisfied waits
  complete with the result pattern and WAITCLEAR modes; a zero-timeout
  poll returns `SCE_KERNEL_ERROR_EVF_COND`; `sceKernelSetEventFlag` /
  delete wake satisfied waiters. When no thread can run, the previous
  headless approximation (log once, report success) is kept as a fallback.
- **Mutexes** (PPSSPP `sceKernelMutex.cpp` semantics): create (initial
  count > 0 = created locked by the creator; recursive only with attr
  `0x200`, priority wake order with `0x100`), lock (blocking, poll
  `WAIT_TIMEOUT`, `ALREADY_LOCKED` for a non-recursive owner relock),
  unlock with direct ownership hand-off to the best waiter, delete waking
  its waiters. Reference error codes are used throughout.
- **UMD** (`p3p_hle_umd`): the Gate 5 stub returned 0 for every UMD call,
  which for `sceUmdCheckMedium` means "no disc in the drive". Now:
  CheckMedium = 1, GetDriveStat = `PRESENT|READY|READABLE` (0x32),
  WaitDriveStat/CB complete when the requested state already holds,
  Activate validates mode + `disc0:` name, GetErrorStat = 0, callback
  register/unregister recorded.

### How the blocker chain was found

1. With yield-only scheduling, the boot's threads switched but two
   frame-paced threads (`CRI ADX Audio` waiting vblank, `CRI Wave out`
   blocking on audio output) ping-ponged and `CRI ADX File` starved —
   hence timed sleeps: a blocked thread is not Ready until its time.
2. With timed sleeps all threads rotated, but still no file was requested.
   A diff of import coverage against the inherited PPSSPP instruction
   trace (which reaches `sceIoOpen` at sequence 83246) showed exactly six
   imports PPSSPP executes that the boot never did; the load-bearing ones
   were the mutex family: `sceKernelCreateMutex` was a return-0 stub, so
   CRI's `CriCsPsp` critical sections never received a UID, no thread ever
   locked/unlocked, and the event-flag traffic that precedes the first
   open never happened.
3. With real mutexes, the CRI thread pool (`CriThread` ×3) spawns, the
   `CRI ADX File` thread issues the opens, and CPK parsing begins
   (`sceIoLseek` + `sceIoRead` follow the opens).

### Verification

- `test_p3p_yield`: **83 checks, 0 failures** — delay yield/resume, solo
  delay, event-flag block/wake/re-dispatch + poll, audio/vblank yield and
  sleep state, the CRI starvation scenario (audio thread vblank-sleeps,
  wave thread audio-sleeps, lower-priority file thread runs), mutex
  ownership/recursion errors/hand-off round trip, poll WAIT_TIMEOUT.
- `test_p3p_services`: **65 checks, 0 failures** (57 Gate 5B checks + 8
  UMD checks). Regressions: `test_p3p_io` **44/44**,
  `test_p3p_threading` **128/128**, `scripts/verify_boot.sh` **OK**
  (Gate 4B oracle window untouched).

### Boot Evidence (AOT, `--game-root profiles/p3p/game`)

- Threads created and scheduled: `user_main` (102), `SceWaveMain` (108),
  `CriThread` ×3 (111/114/117), `CRI ADX Audio` (128), `CRI ADX File`
  (129), `CRI Wave out` (130); context switches rotate across all of them.
- CRI mutexes created with real UIDs (`CriCsPsp`, attr `0x100`); UMD
  activated for `disc0:`.
- **First file I/O served**:
  `[p3p-io] open 'disc0:/PSP_GAME/USRDIR/umd0.cpk' flags=0x00004001 -> fd 3`
  and the same for `umd1.cpk` (fd 4), followed by `sceIoLseek` /
  `sceIoRead` as CRI parses the archives.

### Verdict

**Gate 5C: PASS.** CRI's worker threads execute under the cooperative
scheduler, and the boot reaches and serves the first `sceIoOpen` — which
also closes **Gate 5's** exit criterion. Remaining stubs observed in the
boot (`sceKernelLoadModule`, `sceKernelCreateLwMutex` — whose success
return is 0 anyway, `sceKernelChangeThreadPriority`,
`sceKernelGetThreadCurrentPriority`) are not load-bearing for this
criterion and belong to later gates' scope.


---

## Gate 6 WP1 — Display-List Executor (2026-10-23)

Plan: `GATE6_PLAN.md` (authorized 2026-10-23). Recon: P3P's GE import
surface is just the nine `sceGe_user` calls plus `sceDisplay`; there are
no `sceFont` imports (text is texture-based). The core framework has no
renderer; MHP3rd's ~17k-line GPU stack is game-tuned and was used as an
architecture reference only (notably its address convention).

### What was built

- `profiles/p3p/host/gpu/p3p_ge_executor.{hpp,cpp}`: walks a display
  list in guest memory (u32 commands, opcode in the top byte),
  maintaining the GE register file plus resolved address registers.
  Address rule (per the working in-repo implementation,
  `profiles/mhp3rd/host/gpu/ge_state.cpp`): `BASE` supplies address
  bits 24-27 via `(data & 0x000F0000) << 8`, `OFFSETADDR = data << 8`,
  `relative(arg) = base + offset + arg`; `ORIGIN` re-anchors the offset
  at its own pc. Handles PRIM (recorded as a draw call with a full
  register snapshot), TRANSFERSTART (recorded as a transfer), JUMP/
  BJUMP/CALL/RET/END control flow (32-deep call stack, base/offset
  saved per frame), SIGNAL (delivered to a handler), FINISH, and the
  stall address (checked before each command). A run is capped at
  2^20 commands; out-of-memory fetches end the run as an error.
- HLE wiring (`p3p_hle_gedisplay`): `sceGeListEnQueue`/`EnQueueHead`
  now execute the list synchronously on the shared GE machine and keep
  a per-list record (status, resume pc, draws, stats); `sceGeListSync`
  answers from the record (0 done / 3 stalled / 2 other);
  `sceGeListUpdateStallAddr` resumes a stalled list;
  `sceGeDrawSync` reports busy while any list is stalled;
  `sceGeSetCallback` captures the guest finish/signal callback UIDs —
  SIGNAL notifies the signal callback, a finished list notifies the
  finish callback with the list id. The first 12 list runs log a
  one-line summary.

### Verification

- `test_p3p_ge`: **19 checks, 0 failures** — state/address resolution,
  sprite draw decode + register snapshot, JUMP skip, CALL/RET nesting,
  stall + resume, SIGNAL delivery, transfer recording, ORIGIN
  anchoring, bare RET, runaway cap. Regressions: services 65/65,
  yield 83/83, threading 128/128, io 44/44, `verify_boot.sh` **OK**.
- Boot: the game's real lists decode. List 0 @0x08BB40D4 executes 212
  commands (a state/setup list, no draws yet); list 1 is enqueued
  already stalled (stall == list start, the standard "fill then
  release" pattern) and a later `sceGeListUpdateStallAddr` runs its 29
  commands to completion.

### Discovered blocker for WP2 (not Gate 6 scope)

Past the CPK opens, the boot spins in a tight
LoadModule(`disc0:/PSP_GAME/USRDIR/module/libsuppreacc.prx`) →
StartModule → UnloadModule retry loop (~1.8M iterations in 25 s;
the fake-UID counter in `p3p_hle_services` is the tally). ModuleMgr
loading is still the Gate 5-era fake: no image is placed, no registry
entry exists, so the game's post-load verification can never pass.
Rendering lists with actual draws are downstream of this. Proposed
follow-up: **Gate 5D — real module loading** (parse + place the PRX
image, kernel module registry so GetModuleIdByAddress/GetModuleId
answer for it, StartModule semantics; PRX code execution strategy —
recompiled corpus extension vs interpreter — decided from what the
game actually calls).


---

## Gate 5D — Real PRX Module Loading (2026-10-23)

Plan: `GATE5D_PLAN.md` (authorized 2026-10-23; discovered during
Gate 6 WP1 verification). The boot was spinning in a
LoadModule/StartModule/UnloadModule loop on
`USRDIR/module/libsuppreacc.prx` because ModuleMgr was the Gate 5-era
fake.

### What was built

- **Kernel module registry** (`p3p_kernel`): uid → {name, base, size,
  gp, module_start, started, partition block}; `module_uid_at`
  answers for loaded modules as well as the main module; unregister
  frees the image block.
- **PRX loader** (`host/kernel/p3p_module_loader`): parses
  ET_SCE_RELEXE files, allocates one partition block for the image
  span, zeroes + copies PT_LOAD segments (bss included), and applies
  SHT_PRXREL relocations — R_MIPS_32, R_MIPS_26, and HI16/LO16 pairs
  (the LO16 computes the combined addend and patches all pending
  HI16s); other types are counted, not applied. Module info comes
  from `.rodata.sceModuleInfo`; `module_start` is recovered from the
  syslib export using the real table layout (16-byte entries
  {name, version, attribute, size(words), varCount, funcCount,
  resident}; resident table = [func nids][var nids][func addrs]
  [var addrs] — PPSSPP `sceKernelModule.cpp`). A module may
  legitimately have no module_start: libsuppreacc's syslib exports
  zero functions (two variables: module info + SDK version).
- **ModuleMgr HLE**: LoadModule reads the file via
  `Runtime::translate_path` and loads for real (re-loading a live
  name returns the existing uid); StartModule marks started and
  **returns the module uid**; Stop/Unload/GetModuleId complete the
  lifecycle; unknown uids → SCE_KERNEL_ERROR_UNKNOWN_MODULE
  (0x8002012E). PRX code is not executed (the gate's explicit
  non-goal).

### The decisive detail

The game wrapper (generated unit 0, guest ~0x08807E60) treats
StartModule's return as: v0 > 0 = success, otherwise unload + retry.
PPSSPP's `__KernelStartModule` always returns positive (the start
thread's id, or the module id when there is no entry to run).
Returning 0 from the headless model cost ~1.8M fake loads before
this gate and ~18K real ones during it — the loader itself was never
the problem.

### Verification

- `test_p3p_modules`: **20 checks, 0 failures** (synthetic PRX:
  placement, bss, relocation math, registry lifecycle, truncated
  input). An early version of the fixture shared the loader's wrong
  24-byte export-entry assumption and passed vacuously; both were
  corrected against PPSSPP's layout and the real PRX.
- Regressions: services 65/65, ge 19/19, yield 83/83,
  threading 128/128, io 44/44, `verify_boot.sh` **OK**.
- Boot: `libsuppreacc.prx` loads once ('scesupPreAcc_library'
  @0x09EC1000, 410 relocations applied, 0 skipped) and starts once.
  The game then calls the module's library (`scesupPreAcc::0x86DEBD66`),
  spawns 'sas thread' + 'SDK WRAP', and begins sceSasCore
  initialization (Gate 7 territory), settling into its thread
  rotation. The road to Gate 6 WP2 (draws/frame dump) is open.


---

## Gate 6 WP2 — Software Rasterizer + Frame Dump (2026-10-23)

Plan: `GATE6_PLAN.md` WP2. The rasterizer back end for the WP1
executor, plus the discoveries WP2's boot verification forced.

### What was built

- **`host/gpu/p3p_rasterizer`**: consumes recorded draw calls and
  renders into guest memory. Through-mode only (transformed draws
  counted + skipped; 3D is Gate 8): sprites, triangles, strips, fans,
  points; vertex decode for the GE layouts (tc u8/u16/float, color
  565/5551/4444/8888, position s8/s16/float; through texcoords carry
  5 fractional bits, through s16 positions and OFFSETX/Y are 12.4);
  texturing with nearest sampling — direct formats, CLUT4/8/16 with
  the CLUTFORMAT shift/mask/start semantics, PSP swizzle (16-byte x
  8-row blocks); alpha blending (GE factor/equation encoding incl.
  fixed colors); alpha test; VRAM block transfers (PPSSPP register
  semantics). Wired into the GE HLE: each list run rasterizes its new
  draws immediately, using the display pixel format as the target
  format (the GE has no framebuffer-format register).
- **`host/gpu/p3p_frame_dump`**: dependency-free PNG writer (zlib
  stored blocks) + framebuffer conversion. The host flag
  `--dump-frame FILE` dumps at exit AND every 240 vblanks mid-run —
  a headless boot that never exits by budget still produces frames.
- **Diagnostics that earned their keep**: `kernel.dump_thread_states`
  (per-thread status/wait target/saved pc) + a live-CPU register line
  at exit, both in the host. They cracked the two stalls below.
- `test_p3p_rasterizer`: **16 checks, 0 failures** (sprite fill,
  CLUT8 sampling, transfer strides, triangle coverage, alpha blend
  math, non-through skip). Boot regressions all green; verify_boot OK.

### Boot findings (the frame is still black — and we know exactly why)

1. **The "settle" was a stubbed syscall.** `sceKernelSleepThread`
   (0x9ACE131E) was a return-0 stub; the 'SDK WRAP' thread's main
   loop is work + SleepThread, so it spun at full speed and starved
   the cooperative scheduler (guest clock frozen at t=28.7ms). Fixed
   properly in this WP: kernel ThreadSleep waits + wakeup counts,
   `sceKernelWakeupThread` (0xD59EAD2F). SDK WRAP now sleeps, the
   clock advances, user_main runs.
2. **The boot then STOPS (not hangs)** at guest t≈534ms:
   `Unsupported Allegrex instruction 0x66818102 at 0x08B5737C:
   vfpu1 not lowered yet`. The corpus contains a handful of
   unsupported traps (`rt.unsupported`) in units 0212/0214/0216/0217:
   the VFPU pair 0x66818102/0x66808003 at several sites, VFPU4-group
   0xD03Cxxxx words, and 0x000001CD — the original analysis counted
   6 unsupported occurrences. The init path user_main runs into
   (via the helper at 0x08B57150) needs them.
3. The display framebuffer is therefore still untouched VRAM
   (480x272, one color) — the dump pipeline itself is verified.

### Proposed next step (needs authorization)

**Interpreter fallback for trap-bearing functions**: the runtime
already falls back to its interpreter for unresolved addresses
(`run_interpreter_fallback`). If the profile skips registering the
few AOT functions that contain `rt.unsupported` traps, calls into
them execute interpreted (slow, but these are init-path functions)
and the AOT boot proceeds. Identification: map trap addresses to
function entries via the analysis function table. Fallback if the
interpreter also lacks the encodings: lower the specific VFPU ops.


---

## Gate 6 WP2b — Interpreter Fallback + Rasterizer Correctness (2026-10-23)

The step that took the boot from "stops at 534ms" to "renders
continuously". Two halves: getting execution PAST the unlowered
instructions, and fixing the rasterizer semantics that real display
lists then exposed.

### Interpreter fallback (profile-side, framework mechanisms only)

- The profile installs `psprecomp::set_runtime_unsupported_hook` and
  enables the interpreter fallback. On a trap, the hook registers a
  bridge function (`p3p_interpret_bridge`) at the trap address with a
  non-"recomp_unit_" name — which POISONS the containing 16 KiB
  generated unit (the framework's own mechanism, the same one that
  keeps import-stub buckets dispatchable). Outer dispatch then
  resolves the trap pc exactly, to the bridge, and the bridge runs
  `psprecomp::interpret_allegrex` until control reaches registered
  code again. Everything else in the bucket keeps running AOT via
  exact dispatch. Earlier dead ends, for the record: hook-alone
  (dispatch re-enters the unit and re-traps forever), unregistering
  the block (the unit table ignores the function map).
- **Neither the code generator nor the interpreter implements the
  three instructions the boot path needs** (both stop on them):
  0x66818102 and 0x66808003 (VFPU0-group sub-op 5) at
  0x08B5737C/0x08B57380, and 0xD03C8080 (VFPU4-group) at
  0x08B57528 — all inside one init function reached from 0x08B57150.
  As a labeled PROBE (not a fix), the bridge currently SKIPS those
  instructions (pc += 4, result register keeps a stale value) and
  interprets onward. Only those three sites fire on the whole
  boot-to-render path. Real semantics (candidate: implement in the
  bridge, profile-side) are the next step.
- The interpreter is otherwise load-bearing and healthy: stats at
  exit (instructions/entries + entry profile) are printed by main.

### Rasterizer semantics fixes (all validated against PPSSPP)

The WP2 synthetic tests passed with several wrong conventions
because the fixture shared them; real lists corrected all of these:

1. **CLUT address high bits**: `(CLUTADDRHIGH & 0xFF0000) << 8`
   (bits 16-23, like every other GE address register), not the low
   byte shifted to bit 24.
2. **GE address aliasing**: GE register addresses for VRAM targets
   arrive VRAM-relative (fbp as an offset, width-register address
   bits zero) while GuestMemory maps EDRAM only at 0x04000000.
   The rasterizer now translates any GE address inside the 2 MiB
   VRAM span by +0x04000000 (framebuffer, texture, CLUT, transfers).
3. **Vertex color formats**: the vtype color field is
   0-3 = none, 4 = 565, 5 = 5551, 6 = 4444, 7 = 8888 (PPSSPP's
   colsize table {0,0,0,0,2,2,2,4}), not 1-4. Vertex stride aligns
   to the widest field alignment (color8888 + pos16 = 12 bytes).
4. **Through positions** are raw screen pixels — no drawing-offset
   subtraction, no 12.4 scaling (that belongs to the transform
   path). The fixture had compensated with pre-scaled vertices.
5. **ALPHATEST layout**: func bits 0-2, ref bits 8-15, mask bits
   16-23; the comparison runs on masked values. (The game runs
   GREATER/0/0xFF: alpha-0 texels discarded.)
6. **BLENDMODE layout + factor numbering**: src factor bits 0-3,
   dst factor bits 4-7, equation bits 8-10; factors are the
   GEBlendSrc/DstFactor enums (0 = opposite color, 2 = SRCALPHA,
   10 = fixed, ...). The game's standard blend is 0x32 =
   SRCALPHA / INVSRCALPHA / ADD. Fixed the ABSDIFF equation too.
   The WP2 blend test's register value was rewritten to the
   hardware encoding (0x32) — it had been built to the wrong layout.

Also: the GE HLE rasterize/execute calls are exception-guarded (a
wild guest address logs and skips the list, never kills the host),
and the frame dump reports VRAM nonzero-word counts + raster totals
— the diagnostic that proved the pipeline end-to-end.

### Boot result

- Display lists flow continuously; both framebuffers are fully
  written (138,752 nonzero words each in a 512x272 dump window),
  ~41.6M pixels rasterized in a 40M-dispatch run. The visible frame
  at this boot phase is a near-black fade frame (opaque black with
  a soft bottom edge) — the game's actual content at this point.
- Tests: rasterizer 16/16 (recalibrated fixtures), ge 19, modules
  20, io 44, services 65, yield 83, threading 128; verify_boot OK.
- **Next frontier**: after the render phase is established the boot
  dies on a guest null-struct dereference in unit 0173 at
  0x08AB8B60 (a0 = 0, a1 = 0x08BA888C, ra = 0x08AB9004) — either a
  stub returning 0 where an object is expected, or fallout from the
  skipped VFPU results. Then: real implementations of the three
  skipped instructions.
