# GATE5D_PLAN.md — Gate 5D: Real PRX Module Loading

Status: AUTHORIZED by user 2026-10-23. Discovered during Gate 6 WP1
verification: past the first CPK opens, the boot spins in a
LoadModule → StartModule → UnloadModule retry loop on
`USRDIR/module/libsuppreacc.prx` because ModuleMgr loading is still
the Gate 5-era fake (no image placed, no registry entry). The same
folder carries `libccc.prx` (CRI text encoding) and `libfont.prx`
(the font library), so module loading is a real subsystem, and it
blocks Gate 6 WP2 (frames are downstream of a boot that proceeds).

## Recon

- PRX format: ET_SCE_RELEXE (ELF type 0xFFA0), segments linked from
  vaddr 0, `.rel.*` sections of type SHT_PRXREL (0x700000A0) with
  8-byte entries, `.rodata.sceModuleInfo` (52 bytes: attr, version,
  name[28], gp, ent/stub ranges — vaddr-relative), also reachable via
  a PT phdr of type 0x700000A0. libsuppreacc.prx: 19 KB, text
  ~0x34DC + small data/bss.
- The kernel registry is main-module-only (`register_main_module` +
  `module_uid_at`); the allocator already serves arbitrary named
  blocks (`alloc_partition_memory` / `block_head_address` /
  `free_partition_memory`).
- Imported ModuleMgr surface: LoadModule, StartModule, StopModule,
  UnloadModule, GetModuleId, GetModuleIdByAddress,
  StopUnloadSelfModuleWithStatus. No FindModuleByName.

## Scope (this gate)

1. **Kernel module registry** generalized: uid → {name, base, size,
   gp, module_start, started, partition block}; `module_uid_at`
   answers for loaded modules too; unregister frees the block.
2. **PRX loader** (`host/kernel/p3p_module_loader`): parse the ELF,
   allocate one partition block for the image span, zero + copy
   segments, apply relocations (R_MIPS_32, R_MIPS_26, HI16/LO16
   pairs; other types counted and logged), parse the module info
   and the syslib export to recover `module_start`, register.
3. **ModuleMgr HLE**: LoadModule reads the file through the profile
   VFS (`Runtime::translate_path`) and loads for real (a second load
   of an already-loaded name returns the existing uid); StartModule
   marks the module started and reports the entry it *would* run;
   Stop/Unload complete the lifecycle; GetModuleId answers the main
   module. Unknown uids → SCE_KERNEL_ERROR_UNKNOWN_MODULE (0x8002012E).
4. **Explicit non-goal for this gate:** executing PRX code
   (module_start or exports). StartModule does not run the entry yet;
   the boot evidence after this gate decides whether execution is
   required, and if so whether it comes from a recompiled corpus
   extension or the interpreter. The loader records `module_start`
   precisely so that step is a wiring change, not a redesign.

## Exit criteria

- Unit tests (`test_p3p_modules`) green: synthetic PRX fixture —
  segment placement + bss zeroing, R_MIPS_32/26/HI16/LO16 math,
  module info + module_start recovery, registry queries, unload.
- All prior suites + verify_boot green.
- Boot: the libsuppreacc retry loop breaks (module loads + starts);
  the boot's next state is observed and recorded.
