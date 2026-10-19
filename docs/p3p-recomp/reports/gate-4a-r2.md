# Gate 4A-R2 Audit Record

## Context
Second review of the threading specification. Addressed the 4 blockers: `$a0/$a1` thread entry contract, mathematical stack formulas, `$gp` initialization, and guest memory allocator API.

## Key Findings
1. Identified integer underflow risk in naive stack bounds formulas.
2. Verified that `$sp` and `$a1` maintain a 64-byte relationship (`sp = a1 - 64`).
3. Discovered that PPSSPP `$ra` points to stack RAM (`0x0BFFFAA0`), while Yakumo uses native stub `0x08800000`.
4. Identified that `GuestMemory` lacks an allocator, pointing to `Kernel::allocate_block` in `profiles/mhp3rd/`.

## Outcome
CONDITIONAL PASS. Proceeded to Gate 4A-R3 for final mathematical and architectural closure.
