# Ray traced lighting on a deferred lighting path

Status: design for branch `deferred-rt`. Replaces the "trace after the frame,
read next frame" scheme described in the old `rt_lighting.hpp` header.

## Why

The forward renderer has no G-buffer. Today the RT pass traces from the opaque
depth after the frame, and the surfaces read the result in the next frame by
reprojecting their own position into it (`rtLightAt`). Everything that moves on
its own (units, what they carry, doors, ships) then needed a second placement
(`RtPrevPos`, the prev-bone ring, prev model matrices, the mover list and its
tile tagging), and where a surface found nothing of itself it borrowed from
beside (`rtBorrowDist`, the rings, the orange debug colour), with history resets
on cuts and rooms. That machinery is the source of the remaining ghosts and
smears.

The new design traces the frame's own surfaces in the same frame:

1. Opaque surfaces draw as now, computing the original client's colour in their
   own shaders, and in the same draw write a compact G-buffer next to it (MRT).
2. The RT pass traces from that G-buffer immediately (half resolution, 1 spp,
   temporal + a-trous as today, but reprojecting only current-frame data).
3. A full-screen pass applies the traced terms to the opaque colour.
4. Transparent and blended geometry draws after that, forward, reading the
   current frame's traced result at its own position.

No one-frame delay, no movers, no borrowing.

## RT off stays the forward path, untouched

RT off keeps today's single scene pass, today's pipelines and today's draw
order (terrain, sky, WMO with its blended batches interleaved per group,
characters, models, effects). Deferred-always is not pixel-identical: the RT-on
path has to draw every blended batch after every opaque one (they need the
traced result), and moving the WMO's windows after the doodads changes the
pixels wherever a doodad stands behind a window. So:

* The forward shaders keep one source. A specialization constant
  (`kGbuffer`, constant_id 0) selects the G-buffer output at pipeline creation;
  with it false (every existing pipeline) the shader computes exactly what it
  computes today (the RT-off math is unchanged; `rtParams.x` is 0).
* The opaque pipelines get a second build against a new G-buffer render pass
  (`PipelineBuilder::setGbufferOutputs`): terrain (+wireframe), grass, WMO
  opaque (+wireframe), M2 opaque / cutout / detail-doodad bases with all their
  cull/depth variants, character opaque / alpha-test bases with variants.
  Pipelines are created once at load or MSAA change, not per draw.
* The renderers choose the set per call (`gbuffer` argument), never from a
  persistent flag: the water reflection pass keeps calling terrain and WMO with
  the forward set.

## Pass order with RT on

```
beginFrame:  G-buffer pass begins    [colour, depth, GB0, GB1] (clear)
Phase A      terrain + grass, WMO opaque, characters opaque, M2 opaque   (secondaries)
             end pass
RT           scene update, probes (Full), rt_prepare (from depth + GB),
             rt_trace, rt_temporal, rt_filter x3          (compute, half res)
Apply        full-screen pass on the colour attachment (dual-source blend:
             dst * F + A), per sample under MSAA
Phase B      continue pass (colour + depth LOAD; MSAA: + resolve at its end)
             sky, WMO blended, M2 blended + blended characters + particles +
             ribbons, blob shadows / weather / effects, selection     (secondaries)
             water break, refraction copy, water continue pass  (unchanged)
```

The sky moves behind the opaque phase: it is depth tested against the far plane
and only fills empty pixels, so it is the same picture, and it stays in front of
every blended batch. Phase-A secondaries inherit the G-buffer pass; phase-B ones
inherit the scene pass (the continue pass is compatible with it). WMO and M2 get
a second secondary each (`SEC_WMO_BLEND`, `SEC_M2_BLEND`) recorded by the same
worker; with RT off they record into one buffer in today's order.

WMO blended batches are recorded during the opaque walk as references (group,
merged batch, push values) and replayed into the blended buffer: no second cull,
no extra per-draw work. M2's opaque and blended halves were already two loops;
they become `renderOpaque` / `renderBlended`.

If the world is not drawn in a frame that began the G-buffer pass, the phase
switch still happens (idempotent `leaveGbufferPhase`), without the trace.

## G-buffer layout

Colour (attachment 0, unchanged format): `C0`, the surface's final colour with
the traced terms at their neutral values: sun visibility 1 (only the shadow map
shades), occlusion 1, no point lights, baked shade 1, and the RT-on shadow
formula `(1-k) + k*lit`. Fog is applied as today.

Every forward lighting formula, branch by branch (terrain, terrain-unlit layers,
ground cover, M2 exterior / interior doodad / game object, character exterior /
interior floor / easing ambient, WMO passes unlit / outside / averaged /
interior, unified or not, transition pairs), has the shape, per colour channel:

```
C = C0 + G*(r - 1) - r*max(0, Y*(1 - a) - E) + Kp * P
r = sigma(lit') / sigma(lit0)  (if sun-shadowed)  *  bakedShade (if baked)
```

* `G`: the part of C0 the sun's shadow scale and the baked shade multiply.
* `Y`, `E`: the occlusion's slope inside the clamp chain and the clipped excess.
  The chains are nested `min(1, positive affine)` (the Diffuse light clamp, the
  unified MapObj `clamp(light*0.5 + v + c29)`), which always fold to a single
  knee `min(K, Y*a + Z)`, so this is exact, not linearised.
* `Kp`: what the point light (or, for baked colour, the other lights) is
  multiplied by on its way to the screen (texture stages, fog).
* `lit0`: the shadow map's value (after the edge-on lift for the shadow
  variants); characters lift after the min, flagged.

Everything after the lighting (texture combiners, `surface()`, fog) is a
positive affine map per channel, so `G`, `Y`, `E`, `Kp` are carried through it
by the shader and stored in screen units. Stored relative to C0 they fit in
8 bits:

Two `R16G16B16A16_UNORM` attachments, 128 bits, packed as 16-bit words
(`rt_gbuffer.glsli`, mirrored in `rt_gbuffer_pack.hpp` for the unit tests):

| word | bits |
|------|------|
| 0 | normal oct u (10) - knee r (6) |
| 1 | normal oct v (10) - knee g (6) |
| 2 | shadow-map lit (8) - flags (4) - spare |
| 3 | g.r (8) - g.g (8) |
| 4 | g.b (8) - occ.r (8) |
| 5 | occ.g (8) - occ.b (8) |
| 6 | kp.r (8) - kp.g (8) |
| 7 | kp.b (8) - knee b (6) |

`g = G/C0`, `occ = (Y-E)/C0` (the most occlusion can take), `knee = E/Y`,
`kp = Kp/2`. Flags: valid (an RT surface), sun-shadowed, baked (bakedShade and
the lights less the building's own), lift-after-min (characters). A cleared
G-buffer is "not an RT surface": the sky, grass, the debug views 1-4 and
anything drawn with the colour only are left as they are by the apply pass.

16 bytes per pixel (per sample under MSAA). Position comes from the depth
buffer, which is already there; nothing is written twice.

## Trace inputs and outputs

`rt_prepare` builds the half-resolution trace G-buffer from the depth and GB0:
per 2x2 block the nearest sample among the valid RT surfaces (any sample if
none, so terrain under grass still gets traced), the geometric normal rebuilt
from depth as today (robust ray origins), the distance, which sub-pixel, and
whether it was valid. `rt_trace` uses the G-buffer's shading normal for the
point lights' N.L. Outputs as today: (sun, AO, point visibility, distance) and
(point light unshadowed, baked part), now for the current frame.

`rt_temporal` reprojects each texel's own world position with the previous
view-projection (camera motion vectors from depth, the standard way), rejects
by distance and normal against the previous trace G-buffer, then clamps to the
current neighbourhood's spread. Moving units get no special path: their history
is rejected where it does not match and clamped where it does; the unit-hit
shortening stays. No resets on cuts or rooms are needed: what does not match is
rejected.

## Apply pass

A full-screen triangle into the colour attachment, `dst * F + A` via
dual-source blending (`src1 = F`, `src0 = A`), so `C0` is never copied. Per
pixel (per sample under MSAA, sample-rate shading):

* decode the G-buffer; invalid -> F = 1, A = 0;
* find the traced texels of this surface in the 3x3 around it by distance and
  normal (joint bilateral), giving the weight `w` (1 when found, as today's
  `rt.weight`); no borrowing - a pixel that finds nothing of itself falls back
  to the non-RT look exactly as today (`0.7 + 0.3*shadow`, no occlusion, no
  point lights);
* evaluate the formula above with the Debug Graphics strengths
  (`rtTune`, `lightTune`, `rtTune2`) applied as today.

Debug views 5 (sun), 6 (point lights) and 7 (AO) are written here for opaque
surfaces and in the forward shader for blended ones. 8 and 9 are replaced:
8 = temporal history length, 9 = upsample match (green own surface, red none).

## Transparent and blended geometry

Stays forward, drawn after the apply pass. It reads the current frame's
filtered result (`uRtLight`, `uRtPointLight`, set 0 bindings 3 and 5, now the
current slot) at its own projected position, with the same 3x3 distance match.
Where the texels there are its own surface (an alpha-blended batch on an opaque
one, a blended layer of a model) it is lit as traced; where they are not (glass
in front of a room, a leaf card in front of the sky) it falls back to the non-RT
look, as a surface with no result does today.

A unit drawn see-through (a faded unit, the player's own body as the camera
closes in) is not in the G-buffer, so the texels at its pixels are what stands
behind it. Rather than the non-RT look - whose weaker shadow and missing
torchlight would change the body's light the moment it starts to fade - a unit
that finds nothing of itself takes the shadow map's shadow at the traced
strength, no occlusion, and the point lights of the surface right behind it
(which it shares a place with). Particles, ribbons and water read nothing, as
today.

## MSAA

The G-buffer pass is multisampled like the colour (alpha-to-coverage applies to
every attachment). It stores the MSAA colour and depth (a cost RT-on pays for
the phase split); the continue pass loads them and resolves at its end, so
blended geometry keeps MSAA edges. `rt_prepare` reads sample 0 of the depth and
G-buffer, as today's `_ms` variant. The apply pass runs per sample from the
multisampled G-buffer (exact at edges); without sample-rate shading it runs per
pixel from sample 0.

## Render scale and upscalers

The G-buffer is created at the scene render extent (FSR/FSR2 internal size,
FXAA, or the swapchain) and follows it on resize. Framebuffers are cached per
colour view. FSR2's jitter is in the matrices the trace uses, as today.

## What gets deleted

* `rt_mover.hpp`, `rt_movers.glsli`, `RtLighting::kMaxMovers`, mover buffers,
  binding 13, `writeMovers`, tile tagging in `rt_prepare`, mover logic in
  `rt_temporal` and `rt_filter` (negated distances).
* The see-through ray and the unit/beside test in `rt_prepare`.
* `rtLightAt`'s borrowing, `rtTexelDist`, `rtBorrowDist`, `rtSurfaceIsUnit`,
  `rtLightSeeThrough`, debug views 8/9 as they were.
* `RtPrevPos` in character/M2/WMO shaders; the character prev-bone buffer
  (`PrevBoneSSBO`, `writeFrameBones`' copy); M2 `rtPrevBuffer_`, `rtPrevModel`,
  `rtPrevBones`, `rtPrevRef`, `prepareRtMovers`; WMO `rtPrevModel`,
  `rtPrevFrame`, `prevModel` push constant, `prepareRtMovers`; character
  `rtPlacedModel`, `rtMovers_`.
* `RtLighting::noteCamera` (cuts, rooms, teleports), `ConsumerData::viewProj`,
  `rtViewProj` / `rtCameraPos` in the per-frame block, the scene-closed hook
  that recorded the trace for the post-process path.

Kept: modes 0-3, the debug views 1-7, all Debug Graphics sliders, eye
adaptation, the point-light collection, hardware ray queries and the software
BVH, the probe GI, the CameraZoom Lua functions.

## Performance-relevant changes (to be measured separately)

* CPU: no new draws, no depth prepass. Per frame: two more secondaries
  (WMO/M2 blended), one render pass break, the mover list and the prev-bone
  copies are gone (they cost CPU per unit and per moving doodad).
* GPU: +16 bytes/pixel written by opaque draws (x samples under MSAA), one
  full-screen apply pass (per sample under MSAA), an MSAA colour/depth store and
  reload at the phase break with MSAA. The trace itself is unchanged in cost.

## Risks

* Opaque M2 batches with "no depth write" (material flag 0x10) leave no depth;
  the sky, now drawn after them, can cover them where nothing stands behind.
* WMO transition batches mix two passes with possibly different shadow/baked
  flags into one G-buffer texel; the flags of the inside pass are used.
* Terrain specular (option, off by default) is scaled with the sun shadow ratio
  rather than with the lit value.
* Pixels at thin features that find no traced texel of their own fall back to
  the non-RT look (as today's "no result").
* The G-buffer needs `R16G16B16A16_UNORM` as a colour attachment at the MSAA
  sample count, dual-source blending and independent blending; without them RT
  stays off with a log line.
