# ARCHITECTURE.md — P3P Recompilation Architecture

This document specifies the technical design, execution model, memory contracts, and threading architecture for the Persona 3 Portable recompilation profile on Yakumo.

---

## 1. System Overview & Profile Isolation

The system follows a strict layered architecture:
```text
+-------------------------------------------------------------+
|                     P3P Host Profile                        |
|   (profiles/p3p/host/: Kernel, HLE, Renderer, Audio, Input) |
+-------------------------------------------------------------+
                              │ uses C++ public API
                              ▼
+-------------------------------------------------------------+
|                  Core Recompilation Engine                  |
|       (src/ & include/psprecomp/: Decoder, Runtime,         |
|         GuestMemory, Interpreter Fallback, ELF Loader)      |
+-------------------------------------------------------------+
```

### Boundary Invariant
- **`src/` and `include/psprecomp/`** are completely game-agnostic.
- **`profiles/p3p/`** contains all title-specific logic, memory limits, HLE registrations, and thread lifecycle management.

---

## 2. Execution Model & Direct Function Dispatch

### Ahead-of-Time (AOT) Translation
- The MIPS Allegrex executable (`EBOOT_DECRYPTED.ELF`) is statically analyzed by `psp_analyze`.
- Discovered functions are emitted as C++ translation units (`generated_unit_*.cpp`) by `psp_recomp`, grouped in 4,096-instruction blocks.
- Generated units link into the profile binary and register their guest entry addresses into `Runtime`.

### Runtime Dispatch Loop (`Runtime::run`)
```text
[Guest PC] -> lookup_generated_unit(PC) -> lookup_function(PC) -> RecompiledFunction
                                                                        │
                                                                        ▼
                                                             Executes Native C++ Body
```
1. **Direct Window Lookup (`0x08800000..0x08804000`)**: Reserved 16 KB below the load image. Functions registered here (like `kThreadExitStub = 0x08800000`) resolve in $O(1)$ via `direct_functions_` array probe.
2. **Dense AOT Unit Lookup**: If PC falls inside a generated unit span, direct pointer indexing locates the unit.
3. **Interpreter Fallback**: If an address is neither an AOT unit nor a registered host stub, `Runtime::run_interpreter_fallback` interprets guest bytes until the next registered dispatch target or import stub is reached.

---

## 3. Guest Memory Layout & Access Contracts

### Memory Map (P3P 32 MiB RAM Model)
```text
Address Range                  Size         Description
----------------------------------------------------------------------------------------
0x04000000 - 0x04200000        2 MiB        PSP VRAM (EDRAM) - Mirrored at 0x44..., 0x84...
0x08000000 - 0x08800000        8 MiB        PSP Kernel RAM Partition (Host scratch)
0x08800000 - 0x08804000        16 KiB       Direct Dispatch Window (kThreadExitStub @ 0x08800000)
0x08804000 - 0x08BB58F4        ~3.69 MiB    ELF Segment 0: Executable Code (.text)
0x08BB5900 - 0x08EC0F7C        ~3.05 MiB    ELF Segment 1: Read-Write Data, BSS, Stubs
0x08EC1000 - 0x0A800000        ~25.25 MiB   User Dynamic Allocation (Stacks, Heap, Partitions)
```

### Access Contracts (`GuestMemory`)
- **Canonical Address Translation**: Any pointer alias (cached `0x08...`, uncached `0x48...`, kernel `0x88...`) is normalized via:
  $$\text{canonical}(addr) = addr \ \& \ \text{0x1FFFFFFF}$$
- **Bounds Checking**: `contains(addr, length)` verifies `canonical(addr) >= 0x08000000` and `canonical(addr) + length <= 0x08000000 + 32\text{ MiB}`.
- **Safety**: `resolve(addr, length)` guarantees that if `contains` is true, memory loads (`load8/16/32`) and stores (`store8/16/32`) are safe direct vector accesses that cannot throw or fault.

---

## 4. Thread Context & Stack Specification

### Stack Layout (Top-Down)
When a thread of size `stack_size` (aligned to 256 bytes) is allocated with `stack_bottom` and `stack_top = stack_bottom + stack_size`:
```text
Alamat                       Komponen
----------------------------------------------------------------------------------------
stack_top                    Batas atas blok stack (eksklusif)
  - 256 byte (0x100)         Thread Control Block (TCB) -> $k0 menunjuk ke sini
tcb_address                  Batas bawah TCB
  - align_up(arglen, 16)     Buffer salinan argumen -> $a1 menunjuk ke sini (jika arglen > 0)
sp_arg                       Batas bawah buffer argumen
  - 64 byte (0x40)           PSP ABI Argument Home / Return Stub Frame
initial_sp                   Initial $sp saat thread mulai (0x0880421C)
  ... bebas ke bawah ...     Area pertumbuhan stack fungsi
stack_bottom                 Batas bawah blok stack (inklusif)
```

### Stack Formulas (Proven Invariants)
$$\text{tcb\_address} = \text{stack\_top} - 256$$
$$\text{aligned\_arglen} = (\text{arglen} + 15) \ \& \ \sim 15$$
$$\text{sp\_arg} = \text{tcb\_address} - \text{aligned\_arglen}$$
$$\text{initial\_sp} = \text{sp\_arg} - 64 = \text{stack\_top} - (\text{aligned\_arglen} + 320)$$
$$\text{Syarat Kapasitas:} \quad \mathbf{\text{aligned\_arglen} \le \text{stack\_size} - 320} \iff \mathbf{\text{initial\_sp} \ge \text{stack\_bottom}}$$

### Zero-Length Argument Handling
If `arglen == 0`:
- `aligned_arglen = 0`
- `initial_sp = stack_top - 320`
- Register **`$a0 = 0`**
- Register **`$a1 = 0x00000000` (`NULL`)** (Preserves reference contract; does not point to stack).

---

## 5. Thread Lifecycle & Cooperative Scheduler Hand-off

### State Machine
```text
[Unallocated] ──(allocate_block)──> [Dormant] ──(start_thread)──> [Ready]
                                                                     │
                                                                 (switch_to)
                                                                     ▼
[Deleted/Reclaimed] <──(delete_thread)── [Dead] <──(exit_thread)── [Running]
```

### The Bootstrap Scheduler Hand-off Sequence
1. **Loader Launch**: Initial thread `module_start` (`0x08804108`) is started with `$ra = kThreadExitStub` (`0x08800000`).
2. **Thread Creation**: `module_start` calls `sceKernelCreateThread`:
   - Allocates stack block via `allocate_block("stack:user_main", 1, 0x40000)`.
   - Initializes TCB and thread context with `$pc = 0x0880421C`, `$gp = 0x08C42A50`.
   - Thread enters `Dormant` state. Returns allocated UID.
3. **Thread Start**: `module_start` calls `sceKernelStartThread`:
   - Validates parameters and stack capacity.
   - Stages and copies arguments to stack; sets `$a0 = arglen`, `$a1 = sp_arg`, `$sp = initial_sp`, `$ra = kThreadExitStub`.
   - Thread enters `Ready` state. Returns `0`.
4. **Loader Exit**:
   - `module_start` runs its final 5 instructions (`0x088041F0..0x08804204`) restoring registers.
   - `0x08804208: jr $ra` jumps to `kThreadExitStub` (`0x08800000`).
5. **Context Switch**:
   - Dispatcher calls native `p3p_native_thread_exit`.
   - Loader thread is marked `Dead` and deleted.
   - Scheduler picks `user_main` (highest priority `Ready` thread).
   - `switch_to` copies `user_main->context` into `Runtime::cpu_`.
6. **Execution of `user_main`**:
   - Dispatcher continues next cycle with `cpu_.pc = 0x0880421C`.
   - Executes instructions sequentially from `0x0880421C` to `0x08804260`.

---

## 6. Architectural Divergences: PPSSPP vs Recomp

| Area | PPSSPP (Emulator Reference) | Yakumo Recomp (Native Port) | Rationale |
|---|---|---|---|
| **Return Stub ($ra)** | `$ra = sp` (Stack pointer `0x0BFFFAA0`) | `$ra = 0x08800000` (`kThreadExitStub`) | **Intentional Divergence**: W^X/DEP safety; stack is non-executable RAM; recomp dispatcher intercepts registered native addresses in $O(1)$. |
| **Stack Memory Base** | `0x0B...` (64 MiB model stack top `0x0BFFFC00`) | `0x09...` (32 MiB model user partition top) | **Intentional Divergence**: P3P uses standard 32 MiB partition; relative offsets (`initial_sp = sp_arg - 64`) are identical. |
| **Return Code** | Writes machine code `syscall NID_THREADRETURN` to stack | Invokes native C++ handler `p3p_native_thread_exit` | Avoids JIT overhead; purely compiled host C++. |
| **UID Allocation** | Emulation kernel object map (observed `0x114`) | Dynamic monotonic counter (`100, 101, ...`) | Prevents hardcoded UID dependencies; portable across test configurations. |

---

## 7. Implementation Boundary: Existing vs Proposed

- **Existing in Base Framework**:
  - `src/runtime.cpp`: Dispatch loop, `lookup_function`, pre/post dispatch hooks.
  - `src/guest_memory.cpp`: `GuestMemory` physical vector, `contains()`, `resolve()`.
  - `profiles/mhp3rd/host/kernel/`: Historical MHP3rd reference kernel.
- **Proposed for P3P (Gate 4B)**:
  - `profiles/p3p/host/kernel/p3p_kernel.hpp` & `.cpp`: Self-contained P3P RangeAllocator, `P3PThread`, scheduler.
  - `profiles/p3p/host/hle/p3p_hle_threadman.cpp`: NID bindings for `0x446D8DE6` and `0xF475845D`.
  - `profiles/p3p/tests/test_p3p_threading.cpp`: Unit test suite.
