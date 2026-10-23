# GATE5_PLAN.md — Gate 5 Plan: Headless CPK Mount & File I/O (P3P)

> **Status**: EXECUTED 2026-10-23 (WP0–WP2 + WP4; WP3 superseded by evidence). Implementation complete and unit-verified; boot exit blocked by pre-I/O services — see EVIDENCE.md Gate 5 section. Note: §2/§5's `sceKernelLoadModule` label for NID `0xD8B73127` was wrong; it is `sceKernelGetModuleIdByAddress` (PPSSPP table), and no LoadModule call occurs on the boot path.
> **Prerequisites read**: `PROJECT_STATE.md`, `GATES.md`, `ROADMAP.md`, `DECISIONS.md`, `ARCHITECTURE.md`, `HOST_WIRING_PLAN.md`, Gate 4B evidence.

---

## 1. Purpose

Define how the P3P profile gains file I/O: a mounted virtual disc backed by the user's own game dump, an `IoFileMgrForUser` implementation in the profile, and the verification loop that proves boot progresses past the current init wall. Scope per `ROADMAP.md`: *"Virtual disc mounting, reading `umd0.cpk`, implementing `IoFileMgrForUser`."*

## 2. Verified Starting Point (2026-10-23)

| Fact | Value | Evidence |
|---|---|---|
| Gate 4B | PASS; AOT host boots to user_main and deep into game init | EVIDENCE.md, Gate 4B section |
| Current boot wall | Guest null dereference at `0x08A9B7D8` (recomp_unit_0165) downstream of return-0 stubs; the init path is blocked on kernel services, file I/O among the first | AOT boot log 2026-10-23 |
| Core path facility | `Runtime::set_game_root()` + `Runtime::translate_path()`: strips the `device:` prefix, rejects `..` traversal, joins under the game root | `src/runtime.cpp:517-541` |
| Media layout convention | `profiles/<profile>/game/` is gitignored (framework-wide) | `.gitignore:43` |
| P3P IoFileMgr imports | **20 NIDs** (table §3) | `profiles/p3p/generated/generated_registry.cpp` |
| Reference implementation | MHP3rd `host/hle/hle_io.cpp` (488 lines): Open/Close/Read/Write/Lseek/Getstat/Dopen/Dclose/Rename/Devctl over an fd table, registered by name via its profile helper | `profiles/mhp3rd/host/hle/hle_io.cpp` |
| Game file layout (from the EBOOT's own string table) | `disc0:/PSP_GAME/USRDIR/` with `DATA/`, `SOUND/`, `MEMST.CPK`, `%sumd0.cpk`, `%sumd1.cpk`, `module/libsuppreacc.prx`; savedata path `ms0:/PSP/SAVEDATA/ULES01523-INST00/MEMST.CPK`; raw-sector format string `disc0:/sce_lbn0x%x_size0x%x` | `strings EBOOT_DECRYPTED.ELF` |
| ModuleMgr imports | 7 NIDs; boot calls one in early init (observed stub `ModuleMgrForUser::0xD8B73127`, sceKernelLoadModule) | generated registry; AOT boot log |
| Extraction tooling | `7z` present on the VPS (no xorriso / pycdlib) | `which 7z` |
| Oracle | PPSSPP calls trace: 457,259 calls (`traces/ppsspp_p3p_calls.jsonl`); `PPSSPPHeadless` build present for regenerating reference behaviour with media mounted | `~/recompile/traces/`, `~/recompile/ppsspp_repo/build/` |
| **Media gap** | **No ISO and no CPK files exist anywhere on the VPS.** `~/recompile/p3p_sys/` holds only SYSDIR files from the user's dump. | filesystem audit 2026-10-23 |

**Media requirement (hard prerequisite, per PROJECT_STATE non-goals — Bring Your Own Game):** the user places **his own dumped ISO** of ULES-01523 at `~/recompile/p3p_media/` (outside git). The dump must match the analyzed EBOOT: patched/undub variants alter data files and risk subtle mismatches against the recompiled corpus. No game files are ever committed.

## 3. The I/O Surface (20 IoFileMgrForUser NIDs)

Resolved via `src/nid_registry.cpp` and the PPSSPP HLE tables:

| NID | Name | NID | Name |
|---|---|---|---|
| 0x109F50BC | sceIoOpen | 0xB29DDF9C | sceIoDopen |
| 0x6A638D83 | sceIoRead | 0xE3EB004C | sceIoDread |
| 0x810C4BC3 | sceIoClose | 0xEB092469 | sceIoDclose |
| 0x27EB27B8 | sceIoLseek | 0x54F5FB11 | sceIoDevctl |
| 0x42EC03AC | sceIoWrite | 0x779103A0 | sceIoRename |
| 0xACE946E8 | sceIoGetstat | 0x89AA9906 | sceIoOpenAsync |
| 0x3251EA56 | sceIoPollAsync | 0xA0B5A7C2 | sceIoReadAsync |
| 0xE23EEC33 | sceIoWaitAsync | 0x35DBD746 | sceIoWaitAsyncCB |
| 0xAB96437F | sceIoSync | 0xB293727F | sceIoChangeAsyncPriority |
| 0x71B19E77 | *unresolved* | 0xFF5940B6 | *unresolved (likely sceIoCloseAsync)* |

Seven of the twenty are the async family — the dominant design decision of this gate (§4.3). The two unresolved NIDs are pinned down in WP0 against the PPSSPP source tables.

## 4. Architecture

All code in the profile (ADR-004): `profiles/p3p/host/hle/p3p_hle_io.cpp` (+ header), an fd/dir handle table owned by the profile host, registered with `Runtime::register_hle` by NID. The kernel (Gate 4B) is not extended for I/O in this gate.

### 4.1 Devices and the single-root layout
`translate_path()` strips any `device:` prefix and joins under one game root, so the layout is:

```text
profiles/p3p/game/            <- game_root (gitignored)
├── PSP_GAME/USRDIR/...       <- extracted ISO tree (disc0:/PSP_GAME/...)
└── PSP/SAVEDATA/...          <- writable savedata area (ms0:/PSP/...)
```

`disc0:` is read-only by policy (writes return the PSP read-only error); `ms0:` paths (which land under `PSP/…`) are writable. The executable gains `--game-root <dir>` (default `profiles/p3p/game`) and calls `runtime.set_game_root(...)`.

### 4.2 File serving
Host `std::fstream`-backed handles with 64-bit offsets (`umd0.cpk` is a large archive; offsets must not truncate). `sceIoGetstat` fills the PSP `SceIoStat` layout (mode/size/timestamps) from host stat. Directory handles enumerate the host directory into `SceIoDirent` entries in readdir order.

### 4.3 Async semantics
Headless synchronous completion: Async opens/reads perform the operation immediately and record a completed async status; `sceIoPollAsync` reports done with the result, `sceIoWaitAsync`/`WaitAsyncCB` return it, `sceIoSync` returns the last result for the fd. This matches how PPSSPP behaves when the operation finishes before the poll, and avoids a worker-thread design the cooperative runtime does not need. Risk noted in §6: a game loop that polls for a *not-yet-done* state must never spin forever — synchronous completion guarantees the first poll already sees completion.

### 4.4 CPK is game-side
The CPK archives are parsed by the game's own CRI middleware running as recompiled guest code: the host only serves bytes through `sceIo*`. No CPK parser is written. WP1 verifies this from the trace (open `USRDIR/umd0.cpk` → header/TOC reads and seeks, no host-side format knowledge required).

### 4.5 Raw LBN access (watch item)
The `disc0:/sce_lbn0x%x_size0x%x` string shows the game (or its CRI layer) can address disc sectors directly. If the boot reaches an LBN open/devctl, a directory tree cannot serve it — the ISO file itself (kept at `~/recompile/p3p_media/`) becomes the backing store for an LBN device. This is implemented **only if trace evidence shows it is hit** (WP0/WP2), not speculatively.

## 5. Work Packages

### WP0 — Media + trace groundwork (needs the user's ISO)
1. Extract the ISO with `7z x` into `profiles/p3p/game/`; keep the ISO at `~/recompile/p3p_media/` for potential LBN backing. Verify the tree contains `PSP_GAME/USRDIR/umd0.cpk`, `umd1.cpk`, `MEMST.CPK`, `module/*.prx`.
2. Resolve the two unknown NIDs against PPSSPP's tables; write the ordered I/O call checklist for the init path by decoding the PPSSPP calls trace (jal targets → import names) — extending the Gate 4B windowing approach.
3. Smoke: `PPSSPPHeadless` with the ISO mounted reproduces the reference call sequence for comparison.

### WP1 — Synchronous core (exit: first data file served)
`sceIoOpen/Read/Lseek/Close/Getstat` + fd table + `--game-root` wiring + unit tests (path translation incl. traversal rejection, fd lifecycle, seek/read against a fixture tree).
**Exit criteria**: boot passes the first `sceIoOpen` under `USRDIR` (a successful fd + correct bytes read, checked against the file on disk); init progresses measurably beyond the 2026-10-23 wall (import-call evidence, new stop point recorded in EVIDENCE).

### WP2 — Directory, async, devctl (exit: init's I/O surface complete)
`Dopen/Dread/Dclose`, the async family (§4.3), `sceIoDevctl` minimal set, `Write/Rename` for the `ms0:` area. LBN backing (§4.5) only if the trace demands it.
**Exit criteria**: every IoFileMgr call on the init path (per the WP0 checklist) is implemented — no diagnostic stub for IoFileMgr fires during boot; boot reaches a non-I/O wall (recorded) or the next gate's territory.

### WP3 — ModuleMgr assessment (decision gate inside the gate)
Early init calls `sceKernelLoadModule` for `USRDIR/module/*.prx` (user-extracted copies already in `~/recompile/p3p_modules/`). Loading a PRX means relocating it and executing its code — via the runtime's interpreter fallback without AOT, or with per-module codegen. WP3 is **assessment only**: determine from trace evidence whether init tolerates load failure, what the modules export that init actually calls, and size the loading design. Implementation is a separate authorization (likely its own gate/ADR).

### WP4 — Verification loop
`scripts/verify_io.sh`: build → I/O unit tests → boot with media → progression report (stop point + first-I/O checklist diff vs the PPSSPP reference) → results into EVIDENCE/GATES/PROJECT_STATE, commit + push.

## 6. Risks & Constraints

- **Async polling loops**: if any init code waits on an async result with a timeout-less poll loop, synchronous completion (§4.3) is the safe default; a hang at a poll is the signature of getting this wrong.
- **Stat/layout fidelity**: `SceIoStat`/`SceIoDirent` layouts must match PPSSPP's exactly ( CRi middleware reads file sizes for `umd0.cpk` from Getstat — a wrong size field breaks TOC parsing downstream in guest code, far from the cause).
- **LBN/raw access** (§4.5): would force ISO-backed serving; detected early by WP0's checklist if present on the init path.
- **Module loading scope creep**: WP3 is deliberately assessment-only.
- **Media/version match**: media must be the same ULES-01523 dump the EBOOT was analyzed from (§2).
- **VPS resources**: ISO ~1.8 GB + extracted tree ~1.8 GB — disk is fine after the 2026-10-23 cleanup (20 GB free); nothing here is CPU-heavy.

## 7. Decisions Needed From User

1. **Media**: place your own ULES-01523 ISO dump at `~/recompile/p3p_media/` (I extract and wire everything from there). No other source is acceptable under the project's BYOG rule.
2. **Authorization scope**: WP0–WP2 (file I/O complete, module loading assessed in WP3), or include a module-loading design in this gate?
3. **Savedata**: `ms0:` writes land in `profiles/p3p/game/PSP/SAVEDATA/` — acceptable, or should savedata live outside the profile tree?

## 8. Evidence & Bookkeeping

Per the session-handoff protocol: record commands and raw results in `EVIDENCE.md` per WP, update `PROJECT_STATE.md`/`GATES.md` at gate exit, and leave the tree clean (commit + push to `fork/feature/p3p-recomp`). Media and extracted trees stay out of git (gitignored / outside the repo).
