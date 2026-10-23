#include "hle/p3p_hle_services.hpp"

#include "kernel/p3p_kernel.hpp"
#include "kernel/p3p_module_loader.hpp"

#include <cstdio>
#include <filesystem>
#include <vector>
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
        if (nid == 0x977DE386u) {  // sceKernelLoadModule (Gate 5D: real)
            std::string path;
            for (std::uint32_t i = 0; i < 128u && memory.contains(ctx.gpr[4] + i, 1u); ++i) {
                const char c = static_cast<char>(memory.load8(ctx.gpr[4] + i));
                if (c == '\0')
                    break;
                path += c;
            }
            if (kernel == nullptr) {
                set_v0(kErrorUnknownModule);
                return true;
            }
            std::vector<std::uint8_t> bytes;
            const std::filesystem::path host_path = runtime.translate_path(path);
            if (!host_path.empty()) {
                if (FILE *file = std::fopen(host_path.string().c_str(), "rb")) {
                    std::fseek(file, 0, SEEK_END);
                    const long size = std::ftell(file);
                    std::fseek(file, 0, SEEK_SET);
                    if (size > 0) {
                        bytes.resize(static_cast<std::size_t>(size));
                        if (std::fread(bytes.data(), 1u, bytes.size(), file) != bytes.size())
                            bytes.clear();
                    }
                    std::fclose(file);
                }
            }
            if (bytes.empty()) {
                std::fprintf(stderr, "[p3p] load module '%s': file not readable\n", path.c_str());
                set_v0(kErrorUnknownModule);
                return true;
            }
            const PrxLoadResult res = load_prx_module(memory, *kernel, bytes);
            if (!res.ok) {
                std::fprintf(stderr, "[p3p] load module '%s' failed: %s\n", path.c_str(),
                             res.error.c_str());
                set_v0(kErrorUnknownModule);
                return true;
            }
            // Loading an already-loaded module again returns its uid
            // (the freshly placed copy is dropped).
            if (const std::int32_t existing = kernel->find_module_by_name(res.name);
                existing != 0 && existing != res.uid) {
                kernel->unregister_module(res.uid);
                std::fprintf(stderr, "[p3p] load module '%s': '%s' already loaded as uid %d\n",
                             path.c_str(), res.name.c_str(), existing);
                set_v0(static_cast<std::uint32_t>(existing));
                return true;
            }
            std::fprintf(stderr,
                         "[p3p] load module '%s' -> '%s' uid %d @%08X size %u gp %08X "
                         "module_start %08X relocs %u applied, %u skipped\n",
                         path.c_str(), res.name.c_str(), res.uid, res.base, res.size, res.gp,
                         res.module_start, res.relocations_applied, res.relocations_skipped);
            set_v0(static_cast<std::uint32_t>(res.uid));
            return true;
        }
        if (nid == 0x50F0C1ECu) {  // sceKernelStartModule
            const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const P3PKernel::ModuleRecord *rec =
                kernel != nullptr ? kernel->find_module(uid) : nullptr;
            if (rec == nullptr) {
                set_v0(kErrorUnknownModule);
                return true;
            }
            kernel->set_module_started(uid, true);
            std::fprintf(stderr,
                         "[p3p] start module '%s' uid %d (module_start %08X NOT executed "
                         "yet -- Gate 5D scope)\n",
                         rec->name.c_str(), uid, rec->start_entry);
            // sceKernelStartModule returns a POSITIVE value on success:
            // the start thread's id, or the module id itself when the
            // module has no entry to run (PPSSPP __KernelStartModule).
            // The game checks v0 > 0 and unloads + retries otherwise --
            // returning 0 here was the libsuppreacc retry loop.
            set_v0(static_cast<std::uint32_t>(uid));
            return true;
        }
        if (nid == 0xD1FF982Au) {  // sceKernelStopModule
            const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
            set_v0(kernel != nullptr && kernel->set_module_started(uid, false)
                       ? 0u
                       : kErrorUnknownModule);
            return true;
        }
        if (nid == 0x2E0911AAu) {  // sceKernelUnloadModule
            const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
            set_v0(kernel != nullptr && kernel->unregister_module(uid) ? 0u
                                                                       : kErrorUnknownModule);
            return true;
        }
        if (nid == 0xF0A26395u) {  // sceKernelGetModuleId (the caller's module)
            const std::int32_t uid = kernel != nullptr ? kernel->main_module_uid() : 0;
            set_v0(uid != 0 ? static_cast<std::uint32_t>(uid) : kErrorUnknownModule);
            return true;
        }
        return false;  // StopUnloadSelfModuleWithStatus stays a stub
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
