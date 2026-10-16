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
| 0 | normal oct u (10) - knee r (5) - outside share bit 0 |
| 1 | normal oct v (10) - knee g (5) - outside share bit 1 |
| 2 | shadow-map lit (8) - flags (7) - outside share bit 2 |
| 3 | g.r (8) - g.g (8) |
| 4 | g.b (8) - occ.r (8) |
| 5 | occ.g (8) - occ.b (8) |
| 6 | kp.r (8) - kp.g (8) |
| 7 | kp.b (8) - knee b (5) - outside share bits 3-5 |

`g = G/C0`, `occ = (Y-E)/C0` (the most occlusion can take), `knee = E/Y`,
`kp = Kp/2`. Flags: valid (an RT surface), sun-shadowed, baked (bakedShade and
the lights less the building's own), lift-after-min (characters), unit on an interior floor, interior
(baked light of a room: the room's own light shades it, see "Interior light"
below), transition (below). A cleared
G-buffer is "not an RT surface": the sky, grass, the debug views 1-4 and
anything drawn with the colour only are left as they are by the apply pass.

A WMO transition batch draws an outside pass times alpha and an inside pass
times one less alpha (wmo.frag). The two take different traced terms: the
outside pass the sun's shadow, the inside pass (the room's baked colour) the
baked shade and the room's own light. Written as one surface under one shade,
the sun the trace finds blocked indoors (leaving 30-40% of the colour at the
usual shadow darkness) multiplied the inside pass's baked colour too, with
its interior shade: the floor and frames at doors went black. `rtSurfaceMix`
now flags the texel transition and stores the outside pass's share of G (6
bits, from the knees' sixth bits and the spare ones); the apply pass scales
that share by the sun's ratio and the rest by the baked shade, the outside
share taking every point light and the inside the others. The outside part
keeps the outside pass's sun flag, the inside part the inside pass's baked
and interior flags.

16 bytes per pixel (per sample under MSAA). Position comes from the depth
buffer, which is already there; nothing is written twice.

## Trace inputs and outputs

`rt_prepare` builds the half-resolution trace G-buffer from the depth and GB0:
per 2x2 block the nearest sample among the valid RT surfaces (any sample if
none, so terrain under grass still gets traced), the geometric normal rebuilt
from depth as today (robust ray origins), the distance, which sub-pixel, and
whether it was valid. `rt_trace` uses the G-buffer's shading normal for the
point lights' N.L, turned to the side of the geometric face and replaced by
the geometric normal where the two differ by more than 60 degrees. It was
turned toward the camera once: the Goldshire inn's interior floors carry
vertex normals tilted far off up near walls and doors, the flip then turned
them down or sideways as the camera moved, the overhead lamps' N.L went to
zero, and black discs and patches in view 6 turned with the camera. Outputs as today: (sun, AO, point visibility, distance) and
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

Views 1-4 (the probes) are drawn by the surface shaders, and only by the
surfaces that read the probes (`rtGiRatio`): those lit by the zone's light.
What a building's baked light colours - interior WMO groups, their MODD-lit
doodads, units and game objects on their floors - takes no probe light and
shows its own colour there. A flat grey (ratio 1, "open field") on furniture
indoors meant an object on the outdoor path: game objects (the inn's chairs
and chests) that had found no floor because they were placed before the
building loaded, fixed by asking again when buildings load
(`WMORenderer::instanceGeneration`).

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

## Point lights from what glows

Besides the lights the files carry (M2 lights, MOLT) and the ones given to
fires and lamps without one (fireLight, lampLight), two more kinds of point
light (rt_glow.hpp, `Source::Emissive` and `Source::Particle`, each with its
own intensity and reach slider in Debug Lights):

* Emissive surfaces. At model load (`M2Renderer::computeGlow`) every batch
  drawn unlit (M2 flag 0x1), additive (blend 3, 4) or as a glow card, whose
  texture is bright enough, has its triangles binned into 6-yard cells of the
  model's space (`GlowBuilder`); the brightest four cells become patches and
  the rest are merged into the nearest. A patch's colour is the texture's
  mean over its visible texels (the BLP's small mip, `averageColor` x
  `alphaCoverage`) times the batch colour and transparency, and per frame its
  animated batch's colour and alpha tracks where they move. Its light is at
  the patch's middle, of its hue, with strength and reach growing with the
  square root of its power (brightness x area x the instance's scale squared),
  capped (`glowLight`). WMOs (`WMORenderer::gatherGlows`): in exterior groups,
  additive batches only, where no MOLT light stands within a few yards (that
  light already is the glow's). Not the unlit opaque ones: in a building
  those are windows, trims and signs painted with their light in them (the
  inns' `mm_*_windows` textures), and each became a light at the window that
  shone through the wall into the room behind it and cast the door frames'
  hard shadows across its floor. Windows (MOMT 0x10) give none either. An
  emissive doodad of an interior is `baked` like its lamps.
* Particle emitters. While `prepareM2Particles` writes the particles out, an
  instance within 100 yards sums each emitter's live particles
  (`ParticleGlow`): position, colour x the texture's mean, weighted by alpha x
  quad area x blend (adds 1, unlit alpha 0.35, lit or modulate 0). One light
  per emitter at the weighted centroid, its strength from the summed area
  (a third of a surface's, `kParticleGain`), its reach at most 10 yards; one
  model's emitters together at most a fire's strength
  (`kMaxParticleModelStrength`), so a mount with a flame on every hoof does
  not light like a dozen fires. It
  follows the effect's flicker since it is rebuilt every frame. The lights
  are read by `collectRtLights` after the M2 worker is joined (or the previous
  frame's on the single-threaded path).

No double light: a model with an M2 light, a fire or a lamp gets neither
glow patches nor particle lights; a model with glow patches gets no particle
lights.

No point light depends on the time of day: lamps (lampLight), fires, glows,
particles give all their light at every hour, inside and out (the user's
choice; the original's own time-dependent lighting - the Light.dbc colours, the
drawn window glow - is untouched).

A light behind a surface's geometric face (below a floor, behind a wall)
gives it nothing, whatever its normal map says (`rt_trace`): a bumped plank's
normal leans toward the hearth's light a yard under the upper floor of the
Goldshire inn and took it in grainy orange streaks. (For a time baked lights'
rays from baked surfaces passed the buildings by; through the floor and the
walls that let every lamp reach every room, and it was taken out again.)

Units cast point-light shadows near themselves (`rtUnitShadowReach`,
`rtUnitReach`). A unit's traced body is its own triangles cut into rigid
pieces, each held by one bone and placed by this frame's bones (the nearest
16 within 40 yards; the rest one mesh in a standing pose), so it does not
hold the skin at the joints. For a while (7a9053b0) every point light's
shadow ray passed the units by: under a lamp above and behind it the
player's body threw a hard wedge as large as a room across the floor ahead -
the farther the floor from the body against the body's distance to the
lamp, the more the pieces' errors are magnified - and with the camera close
in it was a black shape with nothing in view to cast it (the Goldshire inn's
upper floor; the black discs seen there were partly the shading-normal flip
above). Now a unit stops a point light's shadow ray only within the stretch
near the surface: at most 5 yards, and at most three quarters of the way to
the light, so the shadow is never more than four times the body's size. That
end is drawn per ray between 50% and 100% of it, so the shadow is whole up
to about 1.6 times the body's size and fades out beyond instead of ending at
an edge; the light's size (`rtLightRadius`) gives its penumbra. A unit's own
surface is never shadowed by its own pieces or by what it carries: a ray that
starts inside its group's box passes the group by (`rtInGroup`), as for the
sun; the box's floor is lifted 0.15 yards so the ground at its feet still
takes its shadow. Other units' pieces shadow it as they shadow the floor. In
the hardware tracer a grouped instance is never opaque (it comes to the
candidate loop), where one past the reach is turned down; the software
tracer clips a grouped instance's walk at the reach. The probes' rays (GI)
leave the reach at its default, unlimited. A pixel whose shadow ray a unit
stopped keeps a short history (`UNIT_HISTORY`), so the shadow follows the
body.

Two more kinds of model are left out of the traced scene as occluders
(`M2Renderer::registerRtModel`), found the same evening by colouring every
pixel by the mesh that stopped its ray to the brightest light:

* What gives light from inside itself - a lamp or lantern (`isLamp`), a model
  with glow patches or its own M2 light. The light sits in its glass or its
  flame, and the frame, the cap and the bars around it took most of its rays:
  a hanging lantern's bars threw hard wedges across the floor of the room
  below it. Such a fixture's shadow of other lights is small.
* A flat sheet - half a yard or more across and no thicker than a tenth of
  that (`rt_lights::isFlatSheet`): a rug, a doormat, a sleeping mat. The trace
  starts a few hundredths of a yard off the opaque surfaces in the G-buffer;
  a rug lying just above the floor covered every ray from the floor beneath it
  and blacked out its exact outline (StormwindRug01 at the doors of the inn's
  rooms).

A lamp's light, and a glow patch, follow the bone that holds the glass
(`rt_lights::dominantBone`, `boneCarried`): a lantern on its chain swings away
from its bind pose, and a light left at the bind pose sat beside the glass.

## Interior light

Inside, the original shows a room's baked vertex colours evenly. With RT on,
a baked colour of an interior - the WMO's baked passes in the camera's
interior pass that are not under the sun, its doodads lit by their MODD
colour - is scaled by

```
l = clamp(L / fullAt, 0, 1)
s = base + (1 - base) * l * (2 - l)      (rtInteriorShade)
```

The light eases into full (`l (2 - l)`): taken linearly, a strong lamp's
light crossed `fullAt` near the end of its reach, and the room's colour
jumped there along an arc.

where `L` is the brightest channel of the shadowed point light at the
pixel, every light counted, the building's own baked MOLT lights included,
each with its traced shadow (the apply pass's `point / wsum` before the
point-light strength). `base` and `fullAt` are Debug Graphics sliders
(50% and 100% by default). Where the lamps shine fully all of the painted
colour remains; where none reaches, `base` of it. Weighted by the pixel's
match weight, so a pixel without a traced result is as painted.

A unit on an interior floor (`RT_GB_UNIT_FLOOR`) takes neither: its colour
is its floor's, which holds the room's lamps as they reach the floor already.
Shaded again at its own pixels, an upright body took the shadow of every
building light in front of its face that the floor turns away (the hearth's
light a yard under the Goldshire inn's upper floor), and the NPCs upstairs
drew at half their colour or less. Only the other lights add to it, as for a
see-through unit (`rtLightBehind`).

The baked lamps' shadow takes their share of the baked colour: `baked /
(baked + 0.5)` (the rest of the bake taken as half a lamp's worth) times the
blocked part (`rtBakedDarkness`), times the Debug Shadows strength. It took
`min(baked, 1)` before, so a strong lamp counted as the whole colour until
the very end of its reach, and the shadow of a wall or post it shone past
ended in a hard arc there.

Shadows of point lights are soft by the light's size: the shadow ray aims at
a point jittered within `clamp(0.04 * reach, 0.15, 0.5)` yards of the light
(`rtLightRadius`), and stops that much shorter, so a lamp's shadow of a post
or a frame has a penumbra rather than a knife edge to its end.

It combines with the baked lamps' shadow (`rtBakedShade`) as the lesser of
the two (`rtInteriorBakedShade`), so a lamp's shadow does not darken twice;
with `base` at 100% the look is exactly the previous one. It goes through the
same G-buffer term as the baked shade (`G`, the part of C0 the shade
multiplies), in the apply pass and, for blended geometry, in the forward
shaders. With RT off nothing changes (`rtTune2.yz` are read only through
the traced terms, whose weight is 0).

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
* Glow lights: at model load, one pass over the glowing batches' triangles
  (M2 and WMO groups). Per frame, the lit-instance list grows by the models
  with glow patches (a distance test each, up to four lights each when near),
  a few additions per particle for the emitters of instances within 100
  yards, and the WMO glows of instances in range. More lights compete for the
  128 the trace takes; the trace's per-pixel loop over the lights is the same
  loop with more entries (up to the same 128).
* Interior light: a few ALU ops per pixel in the apply pass and in the
  blended forward shaders; no new texture reads.

## Risks

* Glow lights: a model whose unlit batches are not light sources (a sign
  painted bright, a bright unlit cloth) gets a light; the brightness floor
  (0.35 for opaque unlit batches) keeps most of them out. Particle lights on
  baked interior surfaces add to light the bake may already hold (the hearth's
  MOLT light), so a hearth's walls are brighter than painted.
* Glowing batches become many lights in a city; the ranking keeps the 128
  most important.
* Units' point-light shadows end 5 yards from the body (or three quarters
  of the way to the light), fading out from half that: a lamp low behind a
  unit, or just above its head, casts a shorter shadow than it would.

* Opaque M2 batches with "no depth write" (material flag 0x10) leave no depth;
  the sky, now drawn after them, can cover them where nothing stands behind.
* WMO transition batches: the outside pass's sun flag and the inside pass's
  baked and interior flags are assumed (a unified WMO's unlit outside pass is
  taken unbaked), and the point lights are split by G's shares, not Kp's.
* Terrain specular (option, off by default) is scaled with the sun shadow ratio
  rather than with the lit value.
* Pixels at thin features that find no traced texel of their own fall back to
  the non-RT look (as today's "no result").
* The G-buffer needs `R16G16B16A16_UNORM` as a colour attachment at the MSAA
  sample count, dual-source blending and independent blending; without them RT
  stays off with a log line. Under MSAA the apply pass also needs
  `sampleRateShading`.
* Foliage and other alpha-tested thin geometry often finds no traced texel
  within the 3x3 match at half resolution (red in debug views 5 and 7) and so
  shows the non-RT look there. The old path borrowed a neighbour's result for
  those pixels; the new one does not, so leaves can look flatter than before.
* Every opaque pipeline gets a G-buffer twin (specialization constant), which
  doubles those pipelines' creation at startup and in the pipeline cache.
* Transparent units (`rtLightBehind`) get the shadow map at RT strength and the
  point lights of what stands behind them, but no traced AO.
