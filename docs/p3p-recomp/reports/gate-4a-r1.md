# Gate 4A-R1 Audit Record

## Context
Initial review of the minimal threading specification. Identified abstraction leak in `src/runtime.cpp` (MHP3rd hardcoded addresses), discrepancies in `$v0` at Call #3, and highlighted that the entry point `0x0880421C` differs from the observed first JAL at `0x08804260`.

## Key Findings
1. MHP3rd debug addresses in `src/runtime.cpp` are guarded by environment variables (`PSPRECOMP_HEAP_DIAG`, etc.).
2. Entry point `0x0880421C` is the true entry point of `user_main`; `0x08804260` is the first JAL instruction inside `user_main`.
3. Call #3 `sceKernelCreateThread` returned 0 on stub vs 0x114 on PPSSPP.

## Outcome
NEEDS REVISION. Proceeded to Gate 4A-R2.
