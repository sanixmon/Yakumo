# GATE6_PLAN.md — Gate 6: 2D Graphics Engine to Vulkan

Status: AUTHORIZED by user 2026-10-23 ("iya" to proceeding with Gate 6 after
Gate 5C PASS). Prerequisite Gate 5 PASS is satisfied. This plan follows the
same pattern as GATE5_PLAN.md: recon findings first, then staged work
packages, each independently verifiable.

## Recon findings (2026-10-23)

- **P3P's GE surface is small.** The import table contains only:
  `sceGeListEnQueue`, `sceGeListEnQueueHead`, `sceGeListSync`,
  `sceGeListUpdateStallAddr`, `sceGeDrawSync`, `sceGeEdramGetAddr`,
  `sceGeEdramGetSize`, `sceGeSetCallback`, `sceGeUnsetCallback`,
  `sceGeBreak`, `sceGeContinue` (library `sceGe_user`), plus the
  `sceDisplay` family already handled. **No `sceFont` imports exist** —
  P3P text is texture-based (CRI/UI atlases), so the ROADMAP's "font
  rasterization" item does not bind an HLE surface for this game.
- **The core has no renderer.** `src/` + `include/psprecomp/` contain no
  Vulkan/GE code at all; graphics is per-profile by design (ADR-004 keeps
  it that way).
- **MHP3rd's renderer is not reusable as-is.** `profiles/mhp3rd/host/gpu/`
  is ~17k lines including a 481KB `vulkan_renderer.cpp`, texture-pack and
  replacement-texture systems, and MH-specific UI texture handling. Its
  *architecture* (a GE state machine feeding a renderer) is the reference;
  its code is game-tuned and will not be ported wholesale.
- **Current P3P state:** `p3p_hle_gedisplay` records list ids and reports
  every list instantly done; nothing ever reads a display list or touches
  VRAM. The game already submits lists every frame during boot.
- **Environment:** the VPS has Vulkan ICDs including Lavapipe (software),
  but **no** Vulkan/SDL3 development headers and no `glslangValidator`
  (the mhp3rd renderer would also be disabled here). WP1/WP2 below need
  no new packages; WP3 needs a package decision (see Risks).

## Design

A profile-local GE stack in `profiles/p3p/host/gpu/`, split so the front
end is testable headless and the back end can change:

1. **Display-list executor** (`ge_executor`): walks a submitted list in
   guest memory — u32 commands, opcode in the top byte — maintaining the
   GE register state (mirroring PPSSPP `GPU/ge_constants.h` +
   `GPUCommon` semantics), honouring stall addresses, `SIGNAL` (GE
   callback via the kernel callback registry), `FINISH`/`END`, and
   `CALL`/`RET`/`JUMP` control flow. Produces a stream of decoded draw
   calls (primitive + fully-resolved state + vertex/index source ranges)
   for the back end. Execution happens at EnQueue/UpdateStall time so
   `sceGeListSync`/`sceGeDrawSync` answers become honest.
2. **Software rasterizer** (`ge_soft`): consumes draw calls and renders
   the 2D subset — sprites and triangles, texture sampling with PSP
   texture formats (CLUT, swizzle, T4/T8/T16/T32), alpha test/blend,
   scissor, viewport/offset — into the guest VRAM framebuffer the game
   points `sceDisplay` at. This is the dev-loop renderer: deterministic,
   headless, unit-testable per draw call.
3. **Vulkan back end** (`ge_vulkan`, WP3): presents the VRAM framebuffer
   through an offscreen Vulkan pass (Lavapipe on the VPS) and later a
   windowed swapchain for desktop runs. Until WP3, verification uses a
   VRAM→PNG dump from the host (`--dump-frame`) as the screenshot path.

## Work packages

- **WP1 — Display-list executor.** Walker + GE state + control flow +
  stall/signal semantics + draw-call recording (no pixels). Unit tests
  with synthetic lists (state opcodes, PRIM vertex counts, CALL/RET,
  stall resume, SIGNAL callback). Boot wiring: EnQueue executes, stats
  logged (commands/draws per list), ListSync reflects real completion.
  *Exit: tests green; boot log shows lists decoded with nonzero draws.*
- **WP2 — Software rasterizer (2D subset) + frame dump.** Vertex fetch
  (transformed + raw 2D sprites), texture decode (CLUT/swizzle/formats),
  blending/alpha/scissor, framebuffer writes into VRAM; host
  `--dump-frame out.png` after N frames. *Exit: a boot run produces a
  recognizable frame (ATLUS/logo or title background) in the dump;
  per-draw unit tests green.*
- **WP3 — Vulkan presentation.** Offscreen Vulkan present of the VRAM
  framebuffer (Lavapipe), screenshot parity with WP2's dump, then the
  windowed path (SDL3) for desktop. *Exit: Vulkan-rendered frame matches
  the software frame for the boot sequence.* Requires the package
  decision below.

## Risks / decisions

- **Vulkan dev packages (WP3 only):** `libvulkan-dev`, SDL3, glslang are
  absent on the VPS. Installing them is a user decision; WP1/WP2 proceed
  without them and already produce visible frames.
- **GE fidelity traps** (from the reference): vertex type decoding
  (through/transformed mode, weight counts), texture swizzle + CLUT
  shifts, signal behaviours (`sceGeSetCallback` + signal numbers the
  game uses for sync), list stall mid-PRIM. Each is handled per PPSSPP
  semantics and pinned by a unit test when first exercised.
- **Scope discipline:** 3D (Tartarus/battle, matrix stacks, lighting,
  skinning) is Gate 8. Gate 6 covers the 2D path the boot/title/UI use;
  3D state opcodes are *decoded and recorded* by WP1 but rasterization of
  lit/skinned 3D draws is explicitly deferred.

## Gate 6 exit criteria (proposed)

1. Display lists are really executed (WP1) with honest sync status.
2. The 2D subset rasterizes into VRAM (WP2); a boot frame dump is
   recognizable (logo/title).
3. Vulkan presents the framebuffer offscreen with parity to the
   software frame (WP3), windowed present available on desktop.
4. All prior suites + verify_boot remain green.
