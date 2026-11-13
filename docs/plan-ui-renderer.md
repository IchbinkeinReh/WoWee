# The interface renderer - plan

**Status:** design, nothing built. Written 2026-10-22.

**Scope:** everything the game draws in 2D - FrameXML, the pre-game screens, the
shared surfaces drawn into FrameXML's frames (minimap, world map, taxi map), and
the world-space overlays (nameplates, chat bubbles, floating combat text,
weather). Not the developer tools, and not the standalone asset tool, which keep
ImGui (section 9).

---

## 1. Why

FrameXML draws every element of the interface now, and nothing in it is ImGui's.
ImGui is still the thing that puts it on the screen: `WidgetRenderer` turns the
widget tree into `ImDrawList` calls, `paper_ui` does the same for the login,
realm and character screens, and ImGui's Vulkan backend records the result in
the overlay pass. About 900 ImGui calls are left in the game, and none of them
is a window FrameXML could take over. What is left is the drawing layer itself.

Leaving ImGui underneath costs more than the dependency:

- **Blend modes.** One ImGui draw list has one blend state. `alphaMode="ADD"` art
  is uploaded a second time with alpha taken from brightness, which only looks
  close to additive over a dark scene. `ALPHAKEY` goes down the same additive
  route, and `MOD` is drawn as ordinary blending. `SetDesaturated` is a no-op.
- **Order is call order.** Nameplates, chat bubbles and minimap blips share
  ImGui's background list with the interface, and what is added last is on top.
  Drawing the panels first once put every nameplate in the world on top of the
  bags. The fix was to move a call. Nothing states the rule.
- **Text is scaled, not rasterised.** Each face is built into one atlas at 18px
  and scaled to whatever FrameXML asks for, mostly 10 to 14. A glyph scaled down
  that far reads as faint, which is why `RasterizerMultiply` is 1.35.
- **Textures never leave.** `uploadImGuiTexture` has no counterpart, so the
  Interface art cache grows for the whole session.
- **The shared surfaces lag a frame.** The world map is an ImGui window placed
  over `WorldMapFrame` using last frame's rect, because it runs before the tree
  is laid out.
- **Input goes through ImGui's IO.** `WantCaptureMouse`, `ImGuiKey` as the key
  type in the keybinding manager, and the SDL text-input toggle that ImGui's
  backend turns off whenever one of its own fields loses focus. The application
  turns it back on every frame.

None of these problems is in the interface. They all come from the layer
underneath it.

## 2. What it is not

- **Not a GUI library.** `WidgetTree` already does layout, anchors, strata and hit
  testing. FrameXML and `paper_ui` are the controls. This plan replaces only what
  they draw through.
- **Not a retained scene.** It is rebuilt every frame from a command list, like
  the ImGui path it replaces. About 666 of 28,000 widgets are visible at once, so
  building the vertices is not the expensive part (see
  `wowee_framexml_widget_count`: the cost was the layout walk).
- **Not a new pass.** It records into the existing overlay pass, in the same place
  `ImGui_ImplVulkan_RenderDrawData` is called today (`Renderer::endFrame`). That
  pass is single-sampled, colour-only and loads the swapchain, and that is
  already correct for this.

## 3. Shape

| Piece | Where | Needs a device | Replaces |
|---|---|---|---|
| `DrawList` - vertices, indices, commands | `include/ui/draw_list.hpp` | no | `ImDrawList` |
| `FontSystem` - faces, metrics, glyph cache | `src/ui/font_system.cpp` | no (metrics), yes (atlas upload) | ImGui's font atlas, `interface_fonts` |
| `UiTextureCache` - handles, upload, eviction | `src/rendering/ui_texture_cache.cpp` | yes | `uploadImGuiTexture`, `ImTextureID` |
| `UiRenderer` - pipelines, buffers, recording | `src/rendering/ui_renderer.cpp` | yes | `imgui_impl_vulkan` |
| `UiInput` - mouse, keys, text, capture | `src/ui/ui_input.cpp` | no | `ImGuiIO`, `ImGuiKey`, `imgui_impl_sdl3` |

The two halves that need no device follow the rule `widget_tree` set: anything
that can be tested without Vulkan is kept apart from the part that cannot. The
`DrawList` output and every text measurement can then be asserted in a unit test
and in `framexml_run`.

## 4. `DrawList`

### Geometry

```cpp
struct UiVertex { float x, y; float u, v; uint32_t rgba; };   // 20 bytes, as ImDrawVert

struct UiDrawCmd {
    UiTexture texture;      // cache handle; 0 is the white texel
    UiBlend   blend;        // Alpha, Add, Mod, AlphaKey
    uint8_t   flags;        // Desaturate
    UiRect    clip;         // pixels, already intersected with the stack
    uint32_t  firstIndex, indexCount, vertexOffset;
};
```

Indices are 32-bit, so no command is ever split because it ran out of 16-bit
range. A new command starts only when texture, blend, flags or clip change.
Consecutive draws with the same state go into one. FrameXML's order is strict,
by strata and then level, so commands are merged in place and never reordered.

### The call set

The call set is what the game actually uses, counted across `src/`:

| Call | Uses | Notes |
|---|---|---|
| `text` | 71 | through `FontSystem`; markup stays in `text_markup.hpp` |
| `rectFilled` / `rect` | 49 | rounding as ImGui does it, for `paper_ui` |
| `image` / `imageQuad` | 25 | `imageQuad` carries `SetTexCoord`'s 8-argument form |
| `line` / `polyline` / `path*` | 30 | `paper_ui`'s hand-drawn strokes, the map layers |
| `circle` / `circleFilled` | 35 | blips, the cooldown sweep |
| `triangle*` / `quad*` / `convexFilled` | 25 | arrows, markers |
| `rectFilledMultiColor` | 10 | `SetGradient`, the colour picker's bar |
| `primReserve` / `primVtx` / `primIdx` | 12 | the cooldown fan and the colour wheel |
| `pushClip` / `popClip` | 10 | scroll frames |

That is about fifteen functions. Anything outside this set is not built until
something needs it.

The non-finite rect check in `WidgetRenderer::draw` moves into the `DrawList`
itself. A NaN vertex that reaches the rasteriser ends in `VK_ERROR_DEVICE_LOST`
whichever caller produced it, so every caller gets the check.

### Layers instead of two lists

```cpp
enum class UiLayer : uint8_t {
    World,       // nameplates, chat bubbles, floating combat text, weather
    Interface,   // the FrameXML tree; paper_ui's page
    Overlay,     // the item on the cursor, paper_ui's dropdowns and modals
    Dev,         // performance HUD, while it exists
};
```

Each layer is a separate `DrawList`, recorded in this order. "The interface is
over the world" becomes a declaration instead of a rule about call order. The
tree's own strata stay inside `Interface`, where they already work.

### Surfaces drawn into a frame

A frame that shows something the client renders currently either carries a
texture (`externalTexture`: portraits, the paperdoll, the minimap composite) or
has an ImGui window placed over its rect (world map, taxi map). The second kind
becomes a callback on the widget:

```cpp
using SurfaceFn = std::function<void(DrawList&, UiRect frame, float scale)>;
```

The tree calls it while drawing, in the frame's own position in strata order,
with the frame's clip already pushed. The world map's layers (player marker,
corpse, party dots, POIs, quest POIs, taxi nodes, zone highlight) draw into the
frame instead of over it. The frame's own border art goes on top, the clip
comes from the scroll frame, and the one-frame lag goes away because the
callback runs after layout. The minimap's Vulkan pass still renders a frame
behind, because that pass runs before the tree. Its blips move into a
callback.

## 5. `FontSystem`

### Faces and metrics

The faces stay the same: FRIZQT, MORPHEUS, SKURRI, ARIALN and FRIENDS, read
from disk or from the archives exactly as `UIManager` reads them now. CJK and
Hangul are a fallback chain on each face instead of a merge into it. Each
face's em scale (`fontEmSizeScale`) is applied the same way.

Metrics come straight from the font file through `stb_truetype`: advance widths,
kerning, ascent and descent. No rasteriser is involved. So `interfaceTextWidth`,
`interfaceTextLines` and the label sizing in `sizeFontStringWidget` work without
a device, and `framexml_run` measures text the way the game does instead of
through a headless ImGui context.

`stb_truetype.h` is vendored into `extern/` next to `stb_image.h`. ImGui already
compiles it in, so the metrics come from the same code with the same results.

### Glyph cache

- Key: face, pixel size and codepoint. The size is the size drawn, which is
  `fontHeight` × the widget's effective scale, rounded to a whole pixel. Glyphs
  are rasterised at that size instead of scaled from 18px. That fixes faint
  small text at its cause, and `RasterizerMultiply` is re-tuned afterwards and
  probably dropped.
- Atlas: `R8` pages, 2048 square, filled by a shelf packer. The image view's
  swizzle is `(ONE, ONE, ONE, R)`, so the shader samples a glyph as white with
  coverage in alpha, the same as any other texture. One pipeline serves both.
- Upload: glyphs are rasterised on the CPU while the frame's text is drawn. The
  dirty rows are copied in `Renderer::endFrame` before the overlay pass opens,
  because a copy is not allowed inside a render pass. They go through the
  existing `ui_upload_budget` staging path.
- Full: the least recently drawn page is cleared and its glyphs are rasterised
  again on demand. The interface draws a few hundred distinct glyphs, so this
  should almost never happen. It is logged when it does.
- Outlines: drawn by offsetting copies, as now, so the first build matches. A
  rasterised outline glyph comes later if offset copies look wrong at the new
  sizes.

Kerning changes widths, and FrameXML sizes from widths (MoneyFrame, and the
auction house anchors that hang off a caption's right edge). It ships off,
matching ImGui, and gets a switch so it can be judged separately.

## 6. `UiTextureCache`

```cpp
using UiTexture = uint32_t;   // index into the cache; 0 = white texel
```

- One entry per texture: image, view, descriptor set, size, the last frame it
  was drawn, and whether it is owned or borrowed. Interface art is owned.
  Portraits, the paperdoll, the character preview and the minimap composite are
  borrowed: registered by whoever renders them and never freed by the cache.
- Descriptor sets come from the context's own `uiTexturePool_`, which already
  outlives an ImGui restart.
- **Eviction**, which the ImGui path could never do. Owned entries that have not
  been drawn for a while are released once they pass a budget. They are
  destroyed `MAX_FRAMES_IN_FLIGHT` frames later, so the GPU is finished with
  them. A released path is read again on next use. This closes the open item in
  `widget-system.md`.
- `Widget::externalTexture` holds a `UiTexture` instead of a `VkDescriptorSet`
  cast to an integer.
- `ADD` art is no longer uploaded twice. It is one texture drawn with the `Add`
  pipeline.

## 7. `UiRenderer`

- **Pipelines:** one per blend mode, built against the overlay pass.

  | Blend | src | dst | Notes |
  |---|---|---|---|
  | `Alpha` | `SRC_ALPHA` | `ONE_MINUS_SRC_ALPHA` | what ImGui does now |
  | `Add` | `SRC_ALPHA` | `ONE` | WoW's `ADD` |
  | `Mod` | `DST_COLOR` | `ZERO` | WoW's `MOD`, a multiply |
  | `AlphaKey` | as `Alpha` | | fragment discards below 0.5 alpha |

  Desaturation is a push-constant flag mixing towards luminance. It is not a
  pipeline of its own.
- **Shader:** position to clip space by push-constant scale and offset, then
  `texture × vertex colour`. The swapchain is `B8G8R8A8_UNORM`, and colours are
  written as given, as ImGui's backend writes them. That keeps the A/B
  comparison in section 8 exact.
- **Buffers:** one vertex buffer and one index buffer per frame in flight,
  host-visible through VMA, doubled when outgrown and never shrunk.
- **Recording:** inline, in the overlay pass, layer by layer. A command's clip
  becomes `vkCmdSetScissor`. Texture changes bind a set, blend changes bind a
  pipeline, and nothing is bound twice in a row.
- **The loading screen** draws into `overlayClearRenderPass` the way it does now.
- **Swapchain recreation:** pipelines are rebuilt only if the overlay pass
  format changed.
- Tracy zones on build, upload and record, plus a per-frame command and vertex
  count, so a regression shows up as a number.

## 8. Order of work

Every step ships on its own and can be compared against the one before it. The
comparison is the test. While two paths exist, `WOWEE_UI_BACKEND=imgui|native`
picks between them, and `tools/ui_diff.py` compares a screen capture from each
on the same frame. A capture of the login screen, character select, and the
world with bags, spellbook and map open makes up the standing set.

1. **Call sites onto `DrawList`, still drawn by ImGui.** `DrawList` gets a
   forwarding implementation that writes into the `ImDrawList` it used before.
   `WidgetRenderer`, `paper_ui` and its screens, the map layers, nameplates, chat
   bubbles and combat text move over. Text goes through a `Font` handle that
   wraps an `ImFont`. The diff must be zero. This is the large mechanical step,
   and it carries no visual risk.
2. **Native geometry and textures.** `UiRenderer` and `UiTextureCache` record
   what `DrawList` holds. Glyphs still come from ImGui's atlas, sampled as one
   more texture, so text does not change yet. The diff should be zero apart from
   rounding. Real `ADD`, `MOD`, `ALPHAKEY` and desaturation turn on at the end of
   this step, as their own commit, because that diff is supposed to change.
3. **`FontSystem`.** Metrics first, checked against ImGui's on a corpus of every
   string FrameXML sizes from: widths within half a pixel at each size, or the
   test fails. Then the glyph cache. Then re-tune the weight. FrameXML's layout
   should not move. Text should get sharper.
4. **`UiInput`.** Mouse, buttons, wheel, keys and text from SDL events. Capture
   flags come from `frameXmlOwnsMouse`, `interfaceTakingTypedInput`, and
   `paper_ui`'s focus. `ImGuiKey` in the keybinding manager becomes SDL keycodes:
   keycodes, not scancodes, so bindings follow the keyboard layout as they do
   now. The application owns text input on and off, and the per-frame re-ask is
   deleted.
5. **The ImGui windows still in the game.**
   - The world map and taxi map become surface callbacks (section 4).
   - The twelve minimap indicator windows (mail, talents, queues, calendar,
     latency, durability, zoom, mute, clock) are checked against `MinimapCluster`
     one by one. Each is deleted where FrameXML already has it, or becomes a
     surface callback.
   - The nameplate context host goes to `UnitPopup`.
   - The quest pane in `map_window` goes to FrameXML's own quest list.
   - The loading screen goes to `paper_ui`'s drawing on `DrawList`.
   - The Classic combat log window stays, redrawn on `DrawList`.
6. **ImGui out of the game binary.** The performance HUD becomes rows of text in
   the `Dev` layer, and the first-run screen becomes a `paper_ui` screen (section
   9). Then `imgui_impl_vulkan` and `imgui_impl_sdl3` leave the `wowee` target,
   followed by `imgui` itself.

Steps 1 to 3 are the risk, because the whole interface draws through them.
Steps 4 to 6 are cleanup that steps 1 to 3 make possible.

## 9. What keeps ImGui

- **`wowee_assets`**, the standalone asset tool. It is already built on its own
  ImGui target (`imgui_sdlrenderer`) and is a desktop tool, where a desktop look
  is right.
- **The asset-builder panel** (`tools/asset_manager/panel.cpp`, 572 lines). The
  in-game first-run screen embeds it. Step 6 replaces it there with a
  `paper_ui` screen covering what a first run needs: pick the install, build,
  watch the progress, and go back. The tool keeps the full panel.

## 10. Risks

| Risk | What would show it | Guard |
|---|---|---|
| Text widths move and FrameXML layout with them | coin buttons overlapping, captions landing short | step 3's metric corpus test, before any glyph is drawn |
| Order changes between world overlays and the interface | nameplates through panels | explicit `UiLayer`; the standing capture set |
| A batch boundary dropped | wrong texture or clip on a run of widgets | `DrawList` unit tests on merge rules |
| Glyph upload inside the render pass | validation error, device lost on MoltenVK | the upload sits with the refraction copy, outside every pass; a test asserts no copy is recorded while a pass is open |
| Evicting a texture still in flight | flicker or device lost | destruction deferred by frames in flight; `WOWEE_UI_NO_EVICT=1` to rule it out |
| Slower than ImGui | frame time | Tracy zones; step 2 must not be slower than step 1 on the standing set |
| Android and MoltenVK behave differently | anything | same overlay pass ImGui already uses there; nothing new in the device contract |

## 11. Tests

- `tests/test_ui_draw_list.cpp`: merge rules, clip intersection, a blend change
  splits a command, a non-finite rect produces nothing, layers stay separate.
- `tests/test_font_system.cpp`: metrics against a font that ships with the
  repository (`extern/imgui/misc/fonts`, until that goes; then a small OFL face
  checked into `tests/data`), wrapping, `|n`, fallback to the CJK face.
- `framexml_run`: label sizes now come from the real metrics, so the overflow and
  lettered-amount reports in `WidgetRenderer` start telling the truth headless.
- `tools/ui_diff.py`: the A/B capture comparison, for steps 1 to 3.

## 12. Size

Roughly: `DrawList` 600 lines, `FontSystem` 900, `UiTextureCache` 400,
`UiRenderer` 700, `UiInput` 500, plus the edits at each call site. Step 1 is
mostly edits. Steps 2 and 3 are most of the new code. This is several weeks of
work, and most of the elapsed time goes to step 3's verification, not to
writing code.
