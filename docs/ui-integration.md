# UI runtime and document ownership

The optional [`anima::ui` API](../include/anima/ui/context.hpp) embeds RmlUi in
Anima's SDL/Vulkan runtime. Game screens, navigation, records and event policy stay
in the consuming project.

## Build

Enable `ANIMA_BUILD_UI=ON` together with `ANIMA_BUILD_DESKTOP=ON`, then link
`anima::ui`. Existing presets default UI off; headless configurations do not fetch
RmlUi or search for FreeType unless `ANIMA_BUILD_UI_DOCUMENTS` is enabled. RmlUi's public headers/core target propagate to UI
consumers, so normal RmlUi elements, documents, form controls and events are usable.
RmlUi 6.3 source is pinned to `ba95ffe8bfb6370efb2cdcca927eaad4710c5413` with a
verified archive SHA-256. A FreeType development package is required for fonts.
The dependency uses static RmlUi, without samples, debugger use, Lua, SVG or Lottie.
The regular FetchContent source override can support an offline build from that
exact source; overrides are caller-owned and must preserve the qualified revision.

## Ownership and loop

```cpp
#include <anima/ui/context.hpp>

anima::VulkanRenderer renderer(window, renderer_options);
anima::UiContext ui(window, renderer);
ui.load_font(font_path);
auto menu = ui.open_document(menu_path);
menu.show();
auto enter = menu.element("enter").on("click", on_enter);

while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        const auto input = ui.process_event(event);
        if (input.focus_lost) clear_held_game_actions();
        if (!input.consumed) process_game_event(event);
    }
    // Apply queued game navigation and document changes outside event callbacks.
    // renderer.set_scenes({}) remains valid here, including an empty selection for menus.
    ui.update();
    if (!ui.render()) SDL_Delay(10); // Draws scene and UI together; no second draw().
}
ui.shutdown();
const auto renderer_stats = renderer.shutdown();
// Destroy the window after renderer; quit SDL last.
```

`open_document()` returns a move-only document owner and throws on load failure.
Keep that owner while the document is in use; destruction closes it. `context()`
and document/element `native()` methods provide borrowed RmlUi access for custom
widgets. Use checked handles and scoped subscriptions for document interaction.
`load_font()` throws on failure.

Continue forwarding window events during resize/fullscreen transitions. The
renderer also reconciles a changed surface when UI dimensions disagree with its
swapchain, so a missed notification cannot strand rendering before image
acquisition. Temporary layout/surface disagreement defers the frame; it does not
recreate unchanged GPU targets repeatedly.

The current integration owns RmlUi's process globals and supports one live UI context
and one borrowed window/renderer. All operations, event callbacks and scene updates
run on that same thread. Sequential create/shutdown cycles are supported. Call
`ui.shutdown()` before renderer shutdown; the UI destructor also shuts it down.
Do not call `Rml::Initialise`/`Shutdown` or change global interfaces independently.

`process_event` returns per-event consumption and current pointer/keyboard/text
capture. Clear held gameplay actions on focus loss and when entering a modal game
screen; engine input capture is not a game action-state machine. SDL text input is
activated for text editing and stopped on blur, hiding the focused element, focus
loss and shutdown. A pointer press retains its starting UI/world ownership until
release. Hiding or closing the pressed UI document cancels its drag and consumes
the eventual release, without retaining pointer capture. Drawable pixel
dimensions and display scale refresh on update/events; author scalable layouts in
`dp`, with CSS `px` representing framebuffer pixels. Pass original SDL coordinates;
the bridge converts pointer and caret coordinates across pixel density.

## Rendering

Panels, buttons, selects, text inputs, font atlas textures, textured geometry,
premultiplied alpha, rectangular scissor clipping and unmasked transforms are
supported. UI is composited in linear light after world display conversion,
using the same frame and presentation pass. Non-sRGB presentation formats are
not qualified for UI.

Advanced render layers, filters/backdrop effects, custom middleware shaders,
mask/rounded/transformed clipping, shader-based gradients, SVG and Lottie are
unsupported. The backend
reports unsupported renderer features explicitly instead of silently dropping
their effect. A context that encountered an unsupported render feature must be
recreated after correcting the document. Basic rounded decoration geometry does
not itself promise rounded clipping support. Font shaping/fallback coverage follows
the default FreeType engine and the fonts supplied by the application. Validate
text shaping and IME behavior with the target fonts and devices.

PNG/JPEG file textures and generated RGBA/font atlases are supported. The bridge
accepts file textures up to 64 MiB encoded and texture dimensions up to 8192 per
axis; the shared image decoder enforces that dimension bound before allocation.
Generated textures have the same dimension limit. The bridge expands triangles
into a CPU stream capped at 2,000,000 vertices per frame and
uploads that stream each draw. GPU textures are cached until RmlUi and recorded
frames release their sources; eviction waits for graphics completion. Scene
replacement does not invalidate UI textures. This is not a throughput benchmark.

Dependency/font notices are recorded in [provenance](../third_party/rmlui/README.md).
The consumer packages its FreeType runtime and any transitive libraries; Anima
provides a separate SDL runtime-copy helper. Exercise GPU rendering, cross-monitor
DPI changes and hardware IME behavior on each target platform.

## Owned documents and scoped events

`UiContext::open_document(path)` returns a move-only `UiDocument`. It closes on
destruction. `element(id)` returns a checked node handle and throws for a missing
ID; `find(id)` returns an empty handle. Removed/replaced nodes reject access;
a newly created node with the same ID does not revive an old handle. Detached
native nodes reject access until reattached to their document. Document close and
host shutdown invalidate handles immediately, even before RmlUi releases its
queued document storage.

```cpp
auto document = ui.open_document("menu.rml");
document.element("status").set_text("Ready <player>"); // Literal text, not markup.
auto subscription = document.element("play").on("click", [&](const anima::UiEvent &event) {
    document.element("status").set_text("Starting");
    event.stop_propagation();
});
document.show();
```

Subscriptions are move-only and disconnect on destruction. Keep the returned
token for as long as the listener is needed. Self-disconnection, node replacement
and document closure inside a callback are supported; listener storage stays
alive through that invocation. `UiEvent` offers checked targets, typed parameter
reads and propagation control. Copies expire when the callback returns. Callback
exceptions are captured and rethrown outside native dispatch by `process_event`
or `update`. An independently hosted document runtime calls `check_events()`.
Do not destroy/shut down the UI context or document host inside an event callback;
explicit host shutdown is rejected there. Closing an individual document is safe.

The common API provides literal text, markup, classes/properties/attributes and
checked form-control values. `native()` is an explicit borrowed escape hatch for
advanced widgets and custom element instancing. Never retain a native reference
across DOM mutation, close or shutdown.

## Headless hosting and scene components

`ANIMA_BUILD_UI_DOCUMENTS=ON` builds `anima::ui_documents` without SDL, Vulkan or
asset libraries. RmlUi and FreeType remain the backend. A non-desktop host supplies
an initialized `Rml::Context` to `UiDocuments`, calls `load`/`from_memory`, and must
shut down/destroy `UiDocuments` before destroying that native context or RmlUi's
process globals. `UiContext` owns this ordering automatically. Document/element
handles and disconnected subscriptions may safely outlive the managed host.
Use one document host per native context; do not unload its native context
independently. The host does not implement rendering or input itself.

When assets are enabled, `anima::ui_scene` supplies `UiPanel` and explicit
`anima.ui-panel.v1` component codecs. Each panel owns an independent document.
The caller supplies a stable asset key and a resolver from that key to a path.
Prefabs persist the key and authored visibility, not pointers, subscriptions,
game state or arbitrary DOM changes. Component removal, scene destruction and
prefab rollback close their documents. Codecs reject a destroyed/shut-down host.

```cpp
auto object = scene.create("menu");
object.add_component<anima::UiPanel>(ui.documents(), "ui.menu", "menu.rml", true);
anima::ComponentCodecs codecs;
anima::add_ui_component_codec(codecs, ui.documents(), resolve_ui_asset);
// Before dispatch/layout, after applying component enablement or visibility:
anima::sync_ui_panels(scene);
```

`sync_ui_panels` combines inherited object activation, component enablement and
`UiPanel::set_visible` state. Hiding takes effect at this explicit boundary. An
enabled component can remain authored-hidden. Showing a panel directly through its document is
transient; the next sync applies component visibility. Panels use screen-space
layout, and applications update document contents explicitly.

The driver also accepts a `SceneSet`. It requires idle scenes and validates every
document before changing visibility. Native show/hide events may mutate objects;
the current panel stays pinned through its event, removed panels and documents
closed by earlier events are skipped, and new panels wait for the next call.
Scheduling and membership stay locked through these events, so nested drivers,
updates and scene-set changes reject. Call `UiDocuments::check_events()` to report
captured failures. Callback effects are not transactional; see
[runtime ownership and ordering](runtime-lifecycle.md).

`ui_documents` tests ownership, literal text, typed values, expired nodes/events,
self-disconnection, callback failures, shutdown across RmlUi recreation and
component/prefab rollback. `consumer_ui_documents` builds an independent
application with no graphics or asset dependencies; `consumer_ui_scene` adds
assets and verifies additive panel synchronization and teardown without graphics.
Both use the consolidated consumer dependency build. Run these checks with the
[local qualification workflow](local-qualification.md). GPU rendering, DPI and
hardware input need separate desktop checks.
