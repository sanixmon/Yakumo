#!/usr/bin/env python3
"""
trace_diff.py - Oracle Trace Comparison Tool for PSPRecomp vs PPSSPP
Compares JSONL execution traces and identifies exact point of divergence.
Features:
- Streaming comparison (no full buffer in RAM)
- Distinguishes PC mismatch vs Register mismatch
- Handles trace length mismatch / early EOF cleanly
- Context window before divergence
"""

import sys
import json
import argparse
from typing import Dict, Any, Optional, List, Tuple

def parse_line(line: str) -> Optional[Dict[str, Any]]:
    line = line.strip()
    if not line or not line.startswith("{"):
        return None
    try:
        return json.loads(line)
    except json.JSONDecodeError:
        return None

def compare_traces(ref_path: str, test_path: str, max_mismatches: int = 5, context_window: int = 3) -> int:
    print("=" * 70)
    print("ORACLE TRACE COMPARISON (PPSSPP Reference vs Recompilation Test)")
    print("=" * 70)
    print(f"Reference Log : {ref_path}")
    print(f"Test Log      : {test_path}")

    tracked_regs = ["sp", "gp", "a0", "a1", "a2", "a3", "v0", "v1"]
    
    ref_count = 0
    test_count = 0
    matched_count = 0
    seq = 0
    
    first_divergence: Optional[Dict[str, Any]] = None
    mismatches: List[Dict[str, Any]] = []
    context_history: List[Tuple[Dict[str, Any], Dict[str, Any]]] = []

    with open(ref_path, "r", encoding="utf-8", errors="replace") as ref_f, \
         open(test_path, "r", encoding="utf-8", errors="replace") as test_f:

        while True:
            line_ref = ref_f.readline()
            line_test = test_f.readline()

            if not line_ref and not line_test:
                break # Both finished simultaneously

            if line_ref:
                ref_count += 1
            if line_test:
                test_count += 1

            # Early EOF condition
            if not line_ref or not line_test:
                if not first_divergence:
                    first_divergence = {
                        "sequence": seq + 1,
                        "type": "LENGTH_MISMATCH",
                        "reason": "Reference ended early" if not line_ref else "Test ended early",
                        "ref_line": line_ref.strip() if line_ref else "<EOF>",
                        "test_line": line_test.strip() if line_test else "<EOF>",
                        "context_before": list(context_history)
                    }
                # Drain remaining count to report accurate total lines
                if line_ref:
                    for _ in ref_f:
                        ref_count += 1
                if line_test:
                    for _ in test_f:
                        test_count += 1
                break

            rec_ref = parse_line(line_ref)
            rec_test = parse_line(line_test)

            if not rec_ref or not rec_test:
                continue

            seq += 1

            pc_ref = rec_ref.get("pc_guest", "").lower()
            pc_test = rec_test.get("pc_guest", "").lower()
            pc_match = (pc_ref == pc_test)

            reg_diffs = []
            for r in tracked_regs:
                vr = rec_ref.get(r, "").lower()
                vt = rec_test.get(r, "").lower()
                if vr and vt and vr != vt:
                    reg_diffs.append((r, vr, vt))

            state_match = pc_match and (len(reg_diffs) == 0)

            if state_match:
                matched_count += 1
                if len(context_history) >= context_window:
                    context_history.pop(0)
                context_history.append((rec_ref, rec_test))
            else:
                mismatch_entry = {
                    "sequence": seq,
                    "type": "STATE_MISMATCH" if pc_match else "CONTROL_FLOW_DIVERGENCE",
                    "pc_ref": pc_ref,
                    "pc_test": pc_test,
                    "pc_match": pc_match,
                    "reg_diffs": reg_diffs,
                    "ref_record": rec_ref,
                    "test_record": rec_test,
                    "context_before": list(context_history)
                }
                if not first_divergence:
                    first_divergence = mismatch_entry
                mismatches.append(mismatch_entry)
                if len(mismatches) >= max_mismatches:
                    # Count remaining lines in files before stopping
                    for _ in ref_f: ref_count += 1
                    for _ in test_f: test_count += 1
                    break

    print("\n--- SUMMARY OF EXECUTION ---")
    print(f"Total Reference Entries : {ref_count}")
    print(f"Total Test Entries      : {test_count}")
    print(f"Matched Synchronously   : {matched_count}")
    print(f"Detected Discrepancies  : {len(mismatches)}")

    if not first_divergence:
        print("\n[RESULT: PASS] Traces are 100% IDENTICAL across all compared entries!")
        return 0

    print("\n--- FIRST DIVERGENCE ANALYSIS ---")
    div_type = first_divergence.get("type")
    div_seq = first_divergence.get("sequence")
    print(f"Type     : {div_type}")
    print(f"Sequence : #{div_seq}")

    if div_type == "LENGTH_MISMATCH":
        print(f"Detail   : {first_divergence['reason']}")
        print(f"Ref Line : {first_divergence['ref_line']}")
        print(f"Test Line: {first_divergence['test_line']}")
    else:
        print(f"PC Guest Reference : {first_divergence['pc_ref']}")
        print(f"PC Guest Test      : {first_divergence['pc_test']}")
        if not first_divergence['pc_match']:
            print("-> Divergence: Control Flow (PC Misalignment)")
        if first_divergence['reg_diffs']:
            print("-> Divergence: Register State Mismatch:")
            for reg, vr, vt in first_divergence['reg_diffs']:
                print(f"   ${reg:4s} -> Expected (PPSSPP): {vr} | Actual (Recomp): {vt}")

    print("\n--- CONTEXT BEFORE DIVERGENCE (Last entries) ---")
    for r_rec, t_rec in first_divergence['context_before']:
        print(f"  Seq #{r_rec.get('sequence', '?')}: PC={r_rec.get('pc_guest')} SP={r_rec.get('sp')} a0={r_rec.get('a0')} v0={r_rec.get('v0')}")

    return 1

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Oracle Trace Diff Tool")
    parser.add_argument("reference", help="Reference log file (PPSSPP)")
    parser.add_argument("test", help="Test log file (Recomp)")
    parser.add_argument("--max-diffs", type=int, default=5, help="Stop after N mismatches")
    args = parser.parse_args()

    sys.exit(compare_traces(args.reference, args.test, max_mismatches=args.max_diffs))
