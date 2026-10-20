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
