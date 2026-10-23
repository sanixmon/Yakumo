#!/usr/bin/env python3
"""Gate 4B window comparison: PPSSPP vs recomp instruction traces.

Extracts the window from the first instruction at user_main entry
(0x0880421C) through the Call #5 jal at 0x08804260 (inclusive) from both
traces and compares:
  - the pc sequence (the recomp interpreter does not log branch delay
    slots, and the PPSSPP thread trampoline at 0x08000030 is outside the
    window; PPSSPP-only entries inside the window are reported, and any
    recomp-only entry or reordering fails the comparison)
  - a0, a2, a3, v0, v1, gp exactly
  - sp and a1 as offsets relative to their values at window entry
    (the two runs use different RAM models: 64 MiB vs 32 MiB,
    ARCHITECTURE.md section 6 documented divergence)
The thread UID ($v0 after CreateThread / $a0 of StartThread) is compared
outside this window via the calls trace; ADR-003 accepts any dynamic UID.
"""
import json
import sys

ENTRY = "0x0880421c"
END = "0x08804260"


def load_window(path):
    window = []
    started = False
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith("{"):
                continue
            ev = json.loads(line)
            if not started:
                if ev["pc_guest"] == ENTRY:
                    started = True
                else:
                    continue
            window.append(ev)
            if ev["pc_guest"] == END:
                break
    return window


def rel(value, base):
    return (int(value, 16) - int(base, 16)) & 0xFFFFFFFF


def main():
    ref = load_window(sys.argv[1])
    test = load_window(sys.argv[2])
    if not ref or not test:
        print(f"window missing: ref={len(ref)} test={len(test)}")
        return 1
    print(f"window: ref {len(ref)} instructions, recomp {len(test)} instructions")

    failures = 0
    ti = 0
    ref_sp0, ref_a1_0 = ref[0]["sp"], ref[0]["a1"]
    test_sp0, test_a1_0 = test[0]["sp"], test[0]["a1"]
    for ri, rev in enumerate(ref):
        if ti < len(test) and test[ti]["pc_guest"] == rev["pc_guest"]:
            tev = test[ti]
            ti += 1
            for reg in ("a0", "a2", "a3", "v0", "v1", "gp"):
                if rev.get(reg) != tev.get(reg):
                    print(f"MISMATCH pc={rev['pc_guest']} reg {reg}: ref={rev.get(reg)} recomp={tev.get(reg)}")
                    failures += 1
            if rel(rev["sp"], ref_sp0) != rel(tev["sp"], test_sp0):
                print(f"MISMATCH pc={rev['pc_guest']} sp offset: ref={rel(rev['sp'], ref_sp0):#x} recomp={rel(tev['sp'], test_sp0):#x}")
                failures += 1
            if rel(rev["a1"], ref_a1_0) != rel(tev["a1"], test_a1_0):
                print(f"MISMATCH pc={rev['pc_guest']} a1 offset: ref={rel(rev['a1'], ref_a1_0):#x} recomp={rel(tev['a1'], test_a1_0):#x}")
                failures += 1
        else:
            # PPSSPP logged an instruction the recomp interpreter does not
            # log separately (branch delay slot executed inside the branch).
            print(f"ref-only (delay slot): seq window[{ri}] pc={rev['pc_guest']}")
    if ti != len(test):
        print(f"recomp has {len(test) - ti} unmatched window instructions")
        failures += 1
    print("WINDOW RESULT:", "PASS" if failures == 0 else f"FAIL ({failures} mismatches)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
