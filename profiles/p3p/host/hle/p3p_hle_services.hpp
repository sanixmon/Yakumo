// Small platform services HLE for the P3P profile (Gate 5B):
// sceImpose, sceUtility, LoadExecForUser, ModuleMgrForUser (query only),
// and Kernel_Library (interrupt state, memcpy/memset, current thread id,
// lwmutex no-ops). Utility dialogs run a minimal status state machine
// (NONE -> INIT_STARTED -> RUNNING -> FINISHED, SHUTDOWN_STARTED -> NONE),
// mirroring PPSSPP's dialog states so polling loops terminate.
#pragma once

#include <cstdint>
#include <string>

namespace psprecomp {
class Runtime;
struct AllegrexContext;
}  // namespace psprecomp

namespace p3p {

class P3PKernel;

void services_reset_state();

// Returns true when the (library, NID) pair was handled. `kernel` may be
// null (interpreter tooling without a kernel attached); kernel-dependent
// queries then return their not-found result.
bool hle_services_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                           P3PKernel *kernel, const std::string &library, std::uint32_t nid);

}  // namespace p3p
