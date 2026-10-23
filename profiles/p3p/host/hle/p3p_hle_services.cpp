#include "hle/p3p_hle_services.hpp"

#include "kernel/p3p_kernel.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>

namespace p3p {
namespace {

// sceUtility NIDs.
constexpr std::uint32_t kNidUtilityLoadModule = 0x2A2B3DE0u;
constexpr std::uint32_t kNidUtilityUnloadModule = 0xE49BFE92u;
constexpr std::uint32_t kNidSavedataInitStart = 0x50C4CD57u;
constexpr std::uint32_t kNidSavedataGetStatus = 0x8874DBE0u;
constexpr std::uint32_t kNidSavedataUpdate = 0xD4B95FFBu;
constexpr std::uint32_t kNidSavedataShutdownStart = 0x9790B33Cu;
constexpr std::uint32_t kNidMsgDialogInitStart = 0x2AD8E239u;
constexpr std::uint32_t kNidMsgDialogGetStatus = 0x9A1C91D7u;
constexpr std::uint32_t kNidMsgDialogUpdate = 0x95FC253Bu;
constexpr std::uint32_t kNidMsgDialogShutdownStart = 0x67AF3428u;

// Utility dialog states (PPSSPP sceUtility dialog status values).
constexpr std::uint32_t kDialogNone = 0;
constexpr std::uint32_t kDialogInitStarted = 1;
constexpr std::uint32_t kDialogRunning = 2;
constexpr std::uint32_t kDialogFinished = 3;
constexpr std::uint32_t kDialogShutdownStarted = 4;

struct ServicesState {
    std::uint32_t savedata_dialog = kDialogNone;
    std::uint32_t msgdialog = kDialogNone;
    std::uint32_t intr_state = 1;  // interrupts start enabled
    std::int32_t exit_callback_uid = 0;
};

ServicesState g_state;

std::uint32_t dialog_update(std::uint32_t state) {
    switch (state) {
        case kDialogInitStarted: return kDialogRunning;
        case kDialogRunning: return kDialogFinished;
        case kDialogShutdownStarted: return kDialogNone;
        default: return state;
    }
}

}  // namespace

void services_reset_state() { g_state = ServicesState{}; }

bool hle_services_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                           P3PKernel *kernel, const std::string &library, std::uint32_t nid) {
    psprecomp::GuestMemory &memory = runtime.memory();
    const auto set_v0 = [&ctx](std::uint32_t value) { ctx.set_gpr(2, value); };

    if (library == "sceImpose") {
        if (nid == 0x36AA6E91u) {  // sceImposeSetLanguageMode
            set_v0(0);
            return true;
        }
        return false;
    }

    if (library == "sceUtility") {
        switch (nid) {
            case kNidUtilityLoadModule:
                std::fprintf(stderr, "[p3p] utility load module %s\n",
                             psprecomp::hex32(ctx.gpr[4]).c_str());
                set_v0(0);
                return true;
            case kNidUtilityUnloadModule:
                set_v0(0);
                return true;
            case kNidSavedataInitStart:
                g_state.savedata_dialog = kDialogInitStarted;
                set_v0(0);
                return true;
            case kNidSavedataGetStatus:
                set_v0(g_state.savedata_dialog);
                return true;
            case kNidSavedataUpdate:
                g_state.savedata_dialog = dialog_update(g_state.savedata_dialog);
                set_v0(0);
                return true;
            case kNidSavedataShutdownStart:
                g_state.savedata_dialog = kDialogShutdownStarted;
                set_v0(0);
                return true;
            case kNidMsgDialogInitStart:
                g_state.msgdialog = kDialogInitStarted;
                set_v0(0);
                return true;
            case kNidMsgDialogGetStatus:
                set_v0(g_state.msgdialog);
                return true;
            case kNidMsgDialogUpdate:
                g_state.msgdialog = dialog_update(g_state.msgdialog);
                set_v0(0);
                return true;
            case kNidMsgDialogShutdownStart:
                g_state.msgdialog = kDialogShutdownStarted;
                set_v0(0);
                return true;
            default:
                return false;
        }
    }

    if (library == "LoadExecForUser") {
        if (nid == 0x4AC57943u) {  // sceKernelRegisterExitCallback
            g_state.exit_callback_uid = static_cast<std::int32_t>(ctx.gpr[4]);
            set_v0(0);
            return true;
        }
        if (nid == 0x05572A5Fu) {  // sceKernelExitGame
            runtime.stop("p3p: sceKernelExitGame called by guest");
            return true;
        }
        return false;
    }

    if (library == "ModuleMgrForUser") {
        if (nid == 0xD8B73127u) {  // sceKernelGetModuleIdByAddress
            const std::int32_t uid = kernel != nullptr ? kernel->module_uid_at(ctx.gpr[4]) : 0;
            set_v0(uid != 0 ? static_cast<std::uint32_t>(uid) : kErrorNotFoundUidFunc);
            return true;
        }
        return false;  // LoadModule & friends stay stubs (later gate)
    }

    if (library == "Kernel_Library") {
        switch (nid) {
            case 0x092968F4u:  // sceKernelCpuSuspendIntr
                set_v0(g_state.intr_state);
                g_state.intr_state = 0;
                return true;
            case 0x5F10D406u: {  // sceKernelCpuResumeIntr
                const std::uint32_t prev = g_state.intr_state;
                g_state.intr_state = ctx.gpr[4];
                set_v0(prev);
                return true;
            }
            case 0x1839852Au: {  // sceKernelMemcpy
                const std::uint32_t dst = ctx.gpr[4];
                const std::uint32_t src = ctx.gpr[5];
                const std::uint32_t size = ctx.gpr[6];
                for (std::uint32_t i = 0; i < size; ++i)
                    memory.store8(dst + i, memory.load8(src + i));
                set_v0(dst);
                return true;
            }
            case 0xA089ECA4u: {  // sceKernelMemset
                const std::uint32_t dst = ctx.gpr[4];
                const auto value = static_cast<std::uint8_t>(ctx.gpr[5]);
                const std::uint32_t size = ctx.gpr[6];
                for (std::uint32_t i = 0; i < size; ++i)
                    memory.store8(dst + i, value);
                set_v0(dst);
                return true;
            }
            case 0x293B45B8u:  // sceKernelGetThreadId
                set_v0(kernel != nullptr ? static_cast<std::uint32_t>(kernel->current_uid()) : 0u);
                return true;
            case 0xBEA46419u:  // sceKernelLockLwMutex
            case 0x15B6446Bu:  // sceKernelUnlockLwMutex
                set_v0(0);
                return true;
            default:
                return false;
        }
    }

    return false;
}

}  // namespace p3p
