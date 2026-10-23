#pragma once

// P3P profile wiring for the AOT executable host (see HOST_WIRING_PLAN.md).

#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>

namespace p3p {

class P3PKernel;

// Wires a runtime for the P3P executable:
//  - HLE bindings: the real ThreadMan implementations when `kernel` is
//    non-null; a diagnostic return-0 stub for every other ELF import. The
//    stubs log once per library/NID because Runtime::invoke_import stops the
//    runtime on unbound imports, and silently swallowing a syscall would
//    hide exactly the evidence later gates need.
//  - In interpreter mode (`aot` == false) each import stub address is also
//    bound to a native function that performs the import and returns to $ra,
//    mirroring tools/p3p_trace_runner (the generated AOT registry provides
//    the equivalent wrappers when `aot` == true).
//  - The thread-exit stub at P3PKernel::kExitStubAddress (ADR-002): hands
//    control to the kernel scheduler, or stops the runtime when no Ready
//    thread remains or no kernel is attached.
void init_profile(psprecomp::Runtime &runtime, const psprecomp::Elf32Image &elf,
                  std::uint32_t load_base, P3PKernel *kernel, bool aot);

}  // namespace p3p
