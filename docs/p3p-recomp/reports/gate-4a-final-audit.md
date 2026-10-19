# Gate 4A Final Technical Audit Report

---

## 1. Stack Bounds Mathematics & Test Vectors

### Formal Invariants
- `stack_top = stack_bottom + stack_size`
- `TCB = stack_top - 256`
- `aligned_arglen = (arglen + 15) & ~15`
- `sp_arg = TCB - aligned_arglen`
- `initial_sp = sp_arg - 64 = stack_top - (aligned_arglen + 320)`

### Pre-Condition Validations (Guaranteed Overflow-Safe)
1. **Pre-Alignment Overflow Check**:
   If `arglen > 0xFFFFFFF0u`, reject with `SCE_KERNEL_ERROR_ILLEGAL_ADDR` (`0x800200D3`).
2. **Capacity Check**:
   If `stack_size < 320u` or `aligned_arglen > (stack_size - 320u)`, reject with `SCE_KERNEL_ERROR_NO_MEMORY` (`0x80020190`).
3. **Mathematical Guarantee**:
   $$\text{initial\_sp} = \text{stack\_bottom} + [\text{stack\_size} - (\text{aligned\_arglen} + 320)] \ge \text{stack\_bottom}$$

### Exact Boundary Table (stack_size = 0x40000 = 262,144 bytes, stack_bottom = 0x09000000)
- `arglen = 0`: `initial_sp = 0x0903FEC0`, `sp_arg = 0x0903FF00`, `$a0 = 0`, `$a1 = 0x00000000`, margin = 261,824 B $\rightarrow$ **VALID**
- `arglen = 27 (0x1B)`: `initial_sp = 0x0903FEA0`, `sp_arg = 0x0903FEE0`, `$a0 = 27`, `$a1 = 0x0903FEE0`, margin = 261,792 B $\rightarrow$ **VALID**
- `arglen = 261,824 (0x3FEC0)`: `initial_sp = 0x09000000`, `sp_arg = 0x09000040`, margin = 0 B $\rightarrow$ **VALID (Max Aligned)**
- `arglen = 261,809 (0x3FEB1)`: `initial_sp = 0x09000000`, `sp_arg = 0x09000040`, margin = 0 B $\rightarrow$ **VALID (Max Unaligned)**
- `arglen = 261,825 (0x3FEC1)`: Exceeds capacity by 16 bytes $\rightarrow$ **REJECTED** (`0x80020190`)
- `arglen = 0xFFFFFFFF`: Integer overflow $\rightarrow$ **REJECTED** (`0x800200D3`)

---

## 2. P3P Return-Stub Dispatch Verification

### Core Framework Mechanics (`src/runtime.cpp:680-705`)
- When `$ra = 0x08800000` is dispatched, `lookup_function(0x08800000)` performs a direct array lookup in `direct_functions_`.
- It directly invokes the bound host function pointer without executing guest memory bytes.

### Architectural Divergence ($ra)
- **PPSSPP**: `$ra = 0x0BFFFAA0` (stack RAM), where synthetic syscall `NID_THREADRETURN` is written.
- **Recomp**: `$ra = 0x08800000` (`kThreadExitStub`), invoking `p3p_native_thread_exit`.
- **Justification**: DEP/W^X compliance on modern OS; eliminates dynamic JIT on stack RAM.

---

## 3. Failure-Atomicity Matrix (`sceKernelStartThread`)

| Failure Condition | Error Code | Guest Memory | Context Registers | Thread Status | Ready Queue |
|---|---|---|---|---|---|
| `thid <= 0` | `0x80020197` | Unmodified | Unmodified | Dormant | Unmodified |
| Unknown `thid` | `0x80020198` | Unmodified | Unmodified | Unmodified | Unmodified |
| Status != Dormant | `0x800201A4` | Unmodified | Unmodified | Unmodified | Unmodified |
| `arglen > 0xFFFFFFF0` | `0x800200D3` | Unmodified | Unmodified | Dormant | Unmodified |
| `arglen > 0 && argp == 0` | `0x800200D3` | Unmodified | Unmodified | Dormant | Unmodified |
| `argp` unmapped | `0x800200D3` | Unmodified | Unmodified | Dormant | Unmodified |
| Stack capacity exceeded | `0x80020190` | Unmodified | Unmodified | Dormant | Unmodified |

---

## 4. Allocator Invariants & P3P Memory Map

### Memory Map
- **Executable Load Image**: `0x08804000` to `0x08EC0F7C`
- **Reserved Dispatch Window**: `0x08800000` to `0x08804000`
- **Dynamic Allocation Base (`user_start`)**: `0x08EC1000` (4 KB aligned after image end)
- **Available Free User RAM**: `0x0193F000` bytes (25.25 MiB)

### Seven Invariants Verified
1. No overlap between allocated blocks.
2. No overlap with reserved memory (`0x08800000..0x08EC1000`).
3. Alignment overflow prohibited.
4. Coalescing on `free_block` merges contiguous free blocks bidirectional.
5. Double-free rejected with `0x800201B6`.
6. Allocation failure leaves free ranges unmodified.
7. Allocator is thread-safe on single-threaded cooperative dispatcher.
