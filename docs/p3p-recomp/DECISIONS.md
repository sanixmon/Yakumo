# DECISIONS.md — P3P Recompilation Architecture Decision Records (ADR)

This document records all architectural decisions made for the Persona 3 Portable recompilation project.

---

### ADR-001: Adoption of TeamGDB/Yakumo Fork over Upstream PSPRecomp
- **Date**: 2026-10-19
- **Decision**: Build the P3P profile on top of the `TeamGDB/Yakumo` fork rather than upstream `jessicanataliagta/PSPRecomp`.
- **Rationale**: Upstream `PSPRecomp` (`profiles/vcs`) only supports Windows MSVC and DirectX 12. `Yakumo` provides an established, multiplatform Vulkan renderer, SDL3 windowing/input, Android NDK packaging, and 235 working HLE implementations.
- **Alternatives Considered**: Upstream `jessicanataliagta/PSPRecomp` (rejected due to Windows/DX12 lock-in); `sp00nznet/psprecomp` (rejected due to missing GPU/Vulkan subsystem).
- **Evidence**: `profiles/vcs/host/ge_gpu_backend_dx12.cpp` requires D3D12; `Yakumo` builds and passes test suites on Linux and cross-compiles to Android arm64.
- **Known Limitations**: Yakumo's core runtime contains some debug address leaks (`0x08...`) that must not be triggered.
- **Revisiting Conditions**: If upstream PSPRecomp adopts Vulkan and Android natively.

---

### ADR-002: Intentional Divergence of `$ra` via Native Return Stub (`kThreadExitStub`)
- **Date**: 2026-10-19
- **Decision**: Initialize thread `$ra` to registered virtual address `0x08800000` (`kThreadExitStub`) rather than pointing to the guest stack RAM as PPSSPP does.
- **Rationale**: On real hardware/emulators, synthetic machine code is written to RAM stack to handle `jr $ra`. In an AOT static recompilation environment, the guest RAM is non-executable data (W^X/DEP compliant). Registering `0x08800000` allows the dispatcher to invoke the native C++ exit handler in $O(1)$ without interpreting RAM.
- **Alternatives Considered**: Writing synthetic MIPS instructions to stack and interpreting them (rejected: violates W^X, adds interpreter overhead); Hardcoding return hook inside JAL lowering (rejected: breaks ABI calling convention).
- **Evidence**: PPSSPP trace Seq #68 shows `$ra = 0x0BFFFAA0`. Disassembly shows `user_main` never inspects `$ra` value, only preserving it on stack frame.
- **Known Limitations**: Trace diff tool must document `$ra` divergence as an intentional architectural difference rather than a failure.
- **Revisiting Conditions**: If a game dynamically compares `$ra` against stack bounds (P3P does not).

---

### ADR-003: Dynamic Monotonic UID Allocation (No Hardcoded IDs)
- **Date**: 2026-10-19
- **Decision**: Allocate kernel thread and block UIDs dynamically starting at 100 (`next_uid++`).
- **Rationale**: PPSSPP happened to return UID `0x114` during observation. Hardcoding `0x114` creates fragile, untestable code that breaks when multiple threads exist or allocation order changes.
- **Alternatives Considered**: Hardcoding `0x114` to force artificial trace match (rejected: dangerous anti-pattern).
- **Evidence**: PPSSPP kernel object map allocates UIDs dynamically via handle table (`Core/HLE/KernelObjects.h`).
- **Known Limitations**: Trace diff tool must accept any valid positive integer for `$a0`/`$v0` thread UIDs when comparing call #4.
- **Revisiting Conditions**: None; dynamic allocation is standard kernel behavior.

---

### ADR-004: Strict Profile Isolation for P3P Kernel and Allocator
- **Date**: 2026-10-19
- **Decision**: Place all P3P kernel structures, range allocators, and HLE implementations strictly under `profiles/p3p/host/kernel/` and `profiles/p3p/host/hle/`. Never modify `src/` or `include/psprecomp/`.
- **Rationale**: Keeps the core framework game-agnostic and maintains git hygiene. Prevents P3P-specific state from leaking into other titles or regression tests.
- **Alternatives Considered**: Modifying `src/runtime.cpp` directly (rejected: architectural violation).
- **Evidence**: PSPRecomp profile contract (`docs/PROFILE_GUIDE.md`).
- **Known Limitations**: Profile must instantiate its own kernel instance and bind stubs via `Runtime::register_function` and `Runtime::register_hle`.
- **Revisiting Conditions**: If a genuinely title-neutral framework bug is uncovered.

---

### ADR-005: Two-Phase Staging Buffer for Thread Arguments
- **Date**: 2026-10-19
- **Decision**: Use a two-phase staging buffer in host memory when copying thread arguments in `sceKernelStartThread`.
- **Rationale**: Guarantees failure atomicity. If argument read fails (e.g. invalid guest memory pointer), zero bytes of the target thread's stack are modified, and thread remains cleanly in `Dormant` state.
- **Alternatives Considered**: Direct byte-by-byte write with rollback on error (rejected: complex rollback, leaves dirty memory on exception).
- **Evidence**: Failure-atomicity matrix in Gate 4A-R3.
- **Known Limitations**: Incurs one transient host heap allocation (`std::vector<uint8_t>`) of at most 256 KB during thread start.
- **Revisiting Conditions**: None; arguments are typically small (27 bytes for P3P bootstrap).

---

### ADR-006: Self-Contained Range Coalescing Allocator (`RangeAllocator`)
- **Date**: 2026-10-19
- **Decision**: Implement a clean `FreeRange` coalescing allocator inside `profiles/p3p/host/kernel/p3p_kernel.cpp` rather than linking to MHP3rd's internal kernel.
- **Rationale**: Avoids linking to MHP3rd-specific code. Provides verified top-down stack allocation (Type 1) and bottom-up partition allocation (Type 0) with automatic coalescing.
- **Alternatives Considered**: Simple bump allocator without free (rejected: leaks memory on thread destruction); Sharing MHP3rd kernel directly (rejected: cross-profile code dependency).
- **Evidence**: Verified with 7 formal invariants in Gate 4A-R3 Python verification suite.
- **Known Limitations**: Allocator operates on guest user partition (`0x08EC1000..0x0A800000`).
- **Revisiting Conditions**: If P3P requires complex memory partition attributes (`MPID`).


### ADR-007: PSP EABI Argument Convention for HLE Dispatch (a0-a3, t0-t3, stack at sp+0)

- **Context**: Gate 5B's first boot showed `sceKernelAllocPartitionMemory('UserSbrk', 16 MiB, PSP_SMEM_LowAligned)` rejected: the alignment, read from `$sp+16` under an o32 layout assumption (the same assumption Gate 4B used for CreateThread's `attr` and Gate 5 for Lseek's `whence`), was stack garbage (`0xFFFFFFFF` — the kernel's own 0xFF stack fill). Disassembling the caller from guest memory shows the alignment (4096) is loaded into `$t0` in the jal delay slot, and PPSSPP's instruction trace shows the identical call succeeding (returns UID `0x115`). PPSSPP's HLE layer documents why (`Core/HLE/HLE.cpp`, `ReadVarArgWord`): psp-gcc builds for the MIPS EABI — the first eight arguments travel in `a0-a3` and `t0-t3`, and the rest go on the stack starting at `sp+0`.
- **Decision**: All profile HLE argument reads follow the EABI layout: parameters 1-4 in `$a0-$a3`, parameters 5-8 in `$t0-$t3`, parameters 9+ on the stack at `$sp+0`. The three o32 reads (AllocPartitionMemory address, CreateThread attr, IoLseek whence) were corrected, and their unit tests now drive those arguments through `$t0`.
- **Consequences**: Gate 4B's CreateThread attr read was a latent bug — it consumed whatever the caller's stack slot happened to hold (a usable value by luck). It is fixed, and the Gate 4B oracle window still verifies PASS (`scripts/verify_boot.sh`). Any future HLE function with 5+ parameters must source parameters 5-8 from `$t0-$t3`; an `$sp+16` argument read in HLE code is a defect.


### ADR-008: Cooperative Scheduling with Timed Sleeps, Re-Dispatch Blocking Waits, and Mutex Hand-Off (Gate 5C)

**Context.** The Gate 4B scheduler only switched threads at thread exit. That
starved every worker the game creates: CRI performs CPK loading on its own
threads while `user_main` pumps server functions per frame, so no `sceIoOpen`
was ever issued. A naive yield-on-wait model was then shown to starve workers
a second way: with instant (headless) completion, a high-priority thread that
"blocks" becomes Ready again immediately and two such threads ping-pong
forever while lower-priority threads never run. On hardware the blocked
thread is *asleep for a duration* (until the next vblank, until its samples
drain, until its delay expires) and is not Ready in between.

**Decision.**
- The kernel gains three scheduling primitives beside the exit hand-off, all
  sharing one picker (highest priority, then lowest UID): `yield_current`
  (voluntary, stays Ready), `block_current` (untimed wait), and
  `sleep_current(duration)` (timed wait on a kernel fake clock). When no
  thread is Ready, the clock idle-skips to the earliest timed wake and due
  sleepers become Ready; the same skip applies on the exit path.
- Scheduling points: `sceKernelDelayThread` (sleeps for the delay), the
  sceDisplay vblank waits (sleep to the next 16667us frame boundary), and the
  sceAudio blocking outputs (sleep to the next frame boundary as the drain
  model). Each presets its return value and resume address ($ra) before
  sleeping, so the saved context continues after the call.
- Blocking waits (WaitEventFlag, LockMutex) save the thread's context
  *pointing at the call*; the call is re-dispatched when the thread is
  scheduled again and re-evaluates its condition. Event-flag waiters are
  re-readied by set/delete; mutex unlock instead hands ownership directly to
  the best waiter (FIFO, or priority order with attr 0x100), marking the
  thread so its re-dispatched lock completes through a hand-off branch in
  `lock_mutex` rather than acquiring twice.
- Time is virtual: the fake clock advances only via sleeps and idle skips.
  Nothing couples guest progress to wall-clock time, keeping runs
  deterministic. Non-poll timeouts on event-flag/mutex waits are not yet
  modelled (a timed-out wait behaves as wait-forever until satisfied);
  poll forms (zero timeout) return the reference error codes.

**Consequences.** All CRI threads (file, audio, wave-out, generic CriThread
pool) now receive CPU in priority order and the boot serves real file I/O
(both CPK archives opened and read). The single-live-context invariant of
Gate 4B is unchanged: switches still happen only inside an HLE dispatch or
the exit hand-off, and the import wrapper's pc guard (ADR-007 era) makes a
handler-switched context safe. `verify_boot.sh` (Gate 4B oracle window) is
unaffected and still passes.
