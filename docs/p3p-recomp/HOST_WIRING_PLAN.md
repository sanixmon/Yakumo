# HOST_WIRING_PLAN.md — Executable Host Wiring Plan (P3P)

> **Status**: PREPARED 2026-10-23. Planning document only — this plan authorizes **no** implementation by itself.
> **Prerequisites read**: `PROJECT_STATE.md`, `GATES.md`, `ROADMAP.md`, `DECISIONS.md`, `ARCHITECTURE.md`, `reports/gate-4a-final-audit.md`.

---

## 1. Purpose

Define exactly how the P3P profile gets a runnable AOT executable host: which targets to add, which files to create, in what order the runtime is wired at boot, and how the result is verified against the PPSSPP oracle. The executable host is the vehicle that Gate 4B (Threading HLE Implementation) executes on; Gate 4B implementation itself still requires explicit user authorization per `GATES.md`.

## 2. Verified Starting Point (2026-10-23)

| Fact | Value | Evidence |
|---|---|---|
| Codegen | Complete: `psp_recomp --auto`, 749 s, 0 errors / 0 warnings | `~/recompile/codegen_p3p.log`, `profiles/p3p/generated/auto_codegen_report.json` |
| Corpus size | 224 units + `generated_registry.cpp` + `recomp_units.hpp` (224 MB source, gitignored) | `profiles/p3p/generated/` |
| Corpus content | 14,787 functions, 706,236 instructions, 913,517 blocks, 221 import wrappers, 741 indirect call sites, 6 unsupported-instruction occurrences | `~/recompile/p3p_analysis/p3p_report.json` |
| Corpus compiles | **225/225 objects, 0 errors** (618 MB objects) | Target `p3p_generated`, build log `~/recompile/build_p3p_generated.log` |
| Build tree | `out/p3p-build` (Ninja, `-DPSPRECOMP_PROFILE=p3p -DPSPRECOMP_GENERATED_JOBS=1`) | `out/p3p-build/CMakeCache.txt` |
| Stale tree warning | `out/p3p` is **STALE** — its cache points at `/tmp/recomp_workspace` (deleted). Do not build there. | `out/p3p/CMakeCache.txt` |
| Boot prototype | `profiles/p3p/tools/p3p_trace_runner.cpp` boots the ELF via interpreter + return-0 import stubs; register setup equalized with PPSSPP | Gate 3 / 3A reports |
| Registry contract | `generated_registry.cpp` registers every AOT unit and all 221 import wrappers (`register_function` at stub addresses); wrappers route to `Runtime::invoke_import(library, nid, ctx)` | `profiles/p3p/generated/generated_registry.cpp` |
| Runtime API | `Runtime(ram_size)`, `Elf32Image::load_and_relocate`, `register_function`, `register_hle`, `register_generated_unit`, `run(entry, max_dispatches)`, `stop_reason()` | `include/psprecomp/runtime.hpp` |
| Reference host | `profiles/mhp3rd/host/main.cpp` + `mhp3rd_profile.cpp` + `host/kernel/`, `host/hle/` | MHP3rd profile |

Key boot constants (verified, `PROJECT_STATE.md` §4): entry `module_start = 0x08804108`, `user_main = 0x0880421C`, first game init JAL target call site `0x08804260`, `$gp = 0x08C42A50`, user partition `0x08EC1000..0x0A800000`, `$ra` exit stub `0x08800000` (ADR-002), boot arglen `27`, boot path `disc0:/PSP_GAME/SYSDIR/EBOOT.BIN`.

## 3. Target Layout (ADR-004: everything under `profiles/p3p/`)

| File | Role | Gate |
|---|---|---|
| `host/main.cpp` | Executable entry: arg parsing, boot sequence (§4), stop reporting. Mirrors `profiles/mhp3rd/host/main.cpp` role. | WP1 |
| `host/p3p_profile.cpp` | Extend existing stub: profile init = register AOT units, exit stub, kernel, HLE bindings. | WP1/WP2 |
| `host/kernel/p3p_kernel.hpp` / `.cpp` | RangeAllocator (ADR-006), `P3PThread` lifecycle, cooperative scheduler hand-off (ARCHITECTURE §5). | **Gate 4B** |
| `host/hle/p3p_hle_threadman.cpp` | `ThreadManForUser` NIDs `0x446D8DE6` (CreateThread), `0xF475845D` (StartThread). | **Gate 4B** |
| `tests/test_p3p_threading.cpp` | Unit tests: allocator invariants, lifecycle, failure atomicity (Gate 4A-R3 matrix). | **Gate 4B** |
| `scripts/verify_boot.sh` | One-command build + run + `trace_diff.py` oracle check for repeatable verification. | WP3 |
| `CMakeLists.txt` | New targets: static libs `p3p_kernel`, `p3p_hle_threadman`; executable target (§7 decision 2) linking `$<TARGET_OBJECTS:p3p_generated>` + `psprecomp_core`. `p3p_generated` already added 2026-10-23. | WP1 |

No changes to `src/` or `include/psprecomp/` anywhere in this plan.

## 4. Boot Sequence Wiring

1. Parse args: ELF path, `--max-dispatches N`, oracle trace via existing `YAKUMO_ORACLE_TRACE` env gate.
2. `Elf32Image::from_file(elf)`; construct `Runtime(32 MiB)`; `elf.load_and_relocate(memory, 0x08804000)` (178,513 relocations).
3. Register the AOT corpus via the generated registry aggregate — AOT becomes the primary dispatch; the interpreter remains the fallback for unregistered PCs (ARCHITECTURE §2).
4. Register `kThreadExitStub` at `0x08800000` → native `p3p_native_thread_exit` (direct-window dispatch, ADR-002).
5. Register HLE bindings: real ThreadMan implementations (WP2); every other import gets a **diagnostic stub** that logs `library::NID` + PC once and returns 0 — an evolution of the trace runner's silent stubs, so the first genuinely missing NID is visible in logs instead of failing silently (85 of 221 NIDs are base-unverified, Gate 2).
6. Instantiate the P3P kernel; create the loader thread context using the trace runner's verified setup: `$sp = 0x09FFFEA0`, boot path written at `0x09FFFE0`, `$a0 = 27`, `$a1 = path addr`, `$gp = module gp`, `$ra = 0x08800000`, remaining GPRs poisoned `0xDEADBEEF` for oracle parity.
7. `runtime.run(0x08804108, budget)`: `module_start` executes as AOT; CreateThread/StartThread hand-off per ARCHITECTURE §5 moves execution to `user_main` at `0x0880421C`.
8. Stop conditions (each must produce a named `stop_reason`): dispatch budget exhausted; exit stub reached with no `Ready` thread (clean boot termination); unsupported instruction hit (report PC + encoding); oracle divergence in test mode.

## 5. Work Packages

### WP1 — Build wiring & stub boot (plumbing, no gate semantics)
- CMake targets, `main.cpp`, profile init, registry + stub registration (§4 steps 1–6 with return-0 ThreadMan stubs).
- **Exit criteria**: executable links; boots the ELF under AOT; execution reaches Call #3 (`sceKernelCreateThread`) and diverges exactly as diagnosed in Gate 3A (stub returns UID 0). Any earlier divergence = wiring bug, fix before WP2.

### WP2 — Gate 4B kernel + ThreadMan (**requires explicit user authorization**)
- Implement per Gate 4A-R3 spec: `p3p_kernel` (RangeAllocator + thread lifecycle), `p3p_hle_threadman` (Create/Start only), `test_p3p_threading`.
- Honors ADR-002 (exit stub), ADR-003 (dynamic UIDs from 100), ADR-005 (two-phase arg staging), ADR-006 (self-contained allocator).
- **Exit criteria** (= Gate 4B acceptance in `GATES.md`): unit tests 100% pass; recomp trace matches PPSSPP instruction-by-instruction `0x0880421C → 0x08804260` with 0 divergences in `trace_diff.py`; Call #5 (`jal 0x08B4E6A0`) dispatched identically on both runtimes.

### WP3 — Verification loop script
- `scripts/verify_boot.sh`: incremental build (Ninja) → run AOT executable with oracle trace → `trace_diff.py` vs `~/recompile/traces/ppsspp_p3p_trace.jsonl` → print first divergence or PASS summary. Reused by every later gate.

### Explicitly deferred (not this plan)
- Full ThreadMan (semaphores, mutexes, event flags, callbacks) — later gates.
- `IoFileMgrForUser` / CPK mount — Gate 5. The three loadable modules in `~/recompile/p3p_modules/` (`libccc.prx`, `libfont.prx`, `libsuppreacc.prx`) load via ModuleMgr and belong to Gate 5+ scope.
- GE/display list, Vulkan — Gate 6; SAS/CRI audio — Gate 7.
- Dispatch-target pre-registration for the 741 indirect call sites: codegen ran **without** `--dispatch-target` (MHP3rd passes one for its known dispatcher). AOT + interpreter fallback is correct-first; only revisit if traces show fallback hotspots.

## 6. Risks & Constraints

- **VPS build resources**: 2 CPU / 3.7 GB RAM. Generated units compile serially (`GENERATED_JOBS=1`) — a unit can exceed 1 GB at high optimization; the current build uses an empty build type (no `-O`) and is safe. Full corpus build took ~50 min; keep Ninja incremental, never touch generated sources.
- **Link step**: linking 618 MB of objects into one executable is untested on this VPS; if the default linker OOMs, switch to `lld` (decision 3 below).
- **Unsupported instructions**: 6 occurrences in the corpus (see analysis report). AOT routes them to `Runtime::unsupported` / interpreter fallback; their PCs must be recorded the first time execution reaches one — not pre-solved here.
- **NID coverage**: 136/221 statically matched in base (Gate 2), runtime behavior unverified. The diagnostic stub (§4.5) is the planned discovery mechanism; do not bulk-implement HLE ahead of trace evidence (gate discipline).
- **Known core wart**: MHP3rd debug address leaks in `src/runtime.cpp` (Gate 1 flag) — P3P addresses near `0x0880421C` must not trip them; if they do, document in `EVIDENCE.md` rather than patching `src/` (ADR-004).

## 7. Decisions Needed From User

1. **Authorization scope**: authorize WP1 only, or WP1 + Gate 4B (WP2) together?
2. **Executable target name**: `p3p` (parallel to MHP3rd's `Yakumo`) vs `p3p_runner`.
3. **Linker fallback**: if the default linker OOMs on the VPS, is switching the profile target to `lld` pre-approved?

## 8. Evidence & Bookkeeping

- On completion of each WP: record commands + raw results in `EVIDENCE.md`, update `PROJECT_STATE.md` §3 and `GATES.md` per the session-handoff protocol, and leave the working tree clean (commit + push to `fork/feature/p3p-recomp`).
