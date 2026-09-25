#pragma once
#include "reference.hpp"
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <anima/assets/mesh_snapshot.hpp>
#include <anima/ui/context.hpp>
#include <chrono>
#include <iostream>

namespace ui_test {
inline void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
inline void test_attribute_conversion() {
    // Attribute conversion uses Variant's public conversion path, including
    // bool-to-string conversion in optimized GCC builds.
    for (const bool value : {false, true}) {
        for (Rml::String text : {Rml::String{}, Rml::String{"old"}, Rml::String(128, 'x')}) {
            const Rml::Variant attribute{value};
            require(attribute.GetInto(text) && text == (value ? "1" : "0"), "Boolean UI attribute conversion failed");
        }
    }
}
inline int run(int argc, char **argv) {
    require(argc >= 4, "Usage: consumer --ui OUTPUT ASSETS [--no-present-fences]");
    const std::filesystem::path output = argv[2], assets = argv[3];
    std::filesystem::create_directories(output);
    require(SDL_Init(SDL_INIT_VIDEO), "SDL initialization failed");
    struct Quit {
        ~Quit() { SDL_Quit(); }
    } quit;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{
        SDL_CreateWindow("Anima external UI consumer", 640, 480,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY),
        SDL_DestroyWindow};
    require(bool(window), "UI window creation failed");
    const float logical_scale = SDL_GetWindowDisplayScale(window.get()) / SDL_GetWindowPixelDensity(window.get());
    require(SDL_SetWindowSize(window.get(), int(640 * logical_scale), int(480 * logical_scale)),
            "UI initial sizing failed");
    anima::RendererOptions options;
    options.validation = true;
    options.disable_present_fences = argc > 4 && std::string_view(argv[4]) == "--no-present-fences";
    options.capture = output / "empty-ui.ppm";
    anima::VulkanRenderer renderer(window.get(), options);
    renderer.set_view(anima::identity());
    const auto window_id = SDL_GetWindowID(window.get());
    const auto started = std::chrono::steady_clock::now();
    unsigned frames = 0, captures = 1;
    unsigned clicks = 0;
    anima::UiContext ui(window.get(), renderer);
    // Exact pixel comparisons below require a stationary scroll position.
    ui.context().SetDefaultScrollBehavior(Rml::ScrollBehavior::Instant, 1.F);
    bool duplicate_rejected = false;
    try {
        anima::UiContext duplicate(window.get(), renderer, "duplicate");
    } catch (const std::logic_error &) {
        duplicate_rejected = true;
    }
    require(duplicate_rejected, "Second live RmlUi owner was accepted");
    ui.load_font(assets / "LatoLatin-Regular.ttf");
    auto document = ui.open_document(assets / "controls.rml");
    auto &doc = document.native();
    auto *input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(doc.GetElementById("name"));
    auto *select = rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(doc.GetElementById("choice"));
    auto *action = doc.GetElementById("action");
    auto *scroll = doc.GetElementById("scroll");
    require(input && select && action && scroll, "Public RmlUi controls unavailable");
    auto subscription = document.element("action").on("click", [&](const anima::UiEvent &) { ++clicks; });
    document.show();
    bool forward_window_events = true;
    const auto pump = [&] {
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(25), "UI watchdog expired");
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                    "UI smoke interrupted");
            if (forward_window_events || !SDL_GetWindowFromEvent(&event))
                (void)ui.process_event(event);
        }
    };
    const auto frame = [&] {
        for (;;) {
            pump();
            ui.update();
            if (ui.render()) {
                ++frames;
                break;
            }
            SDL_Delay(5);
        }
    };
    const auto capture = [&](const char *name) {
        renderer.request_capture(output / (std::string(name) + ".ppm"));
        frame();
        ++captures;
    };
    frame();
    std::uint64_t event_sequence = SDL_GetTicksNS();
    const auto dispatch = [&](SDL_Event event) {
        // Events pass through SDL's real queue and the public bridge, then RmlUi's
        // actual hit testing/editing/event dispatch. No game or widget internals.
        event.common.timestamp = ++event_sequence;
        require(SDL_PushEvent(&event), "SDL event injection failed");
        anima::UiInputResult result;
        bool found = false;
        SDL_Event received{};
        while (SDL_PollEvent(&received)) {
            const auto current = ui.process_event(received);
            if (received.type == event.type && received.common.timestamp == event.common.timestamp) {
                result = current;
                found = true;
            }
        }
        require(found, "Injected SDL event was not delivered");
        return result;
    };
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    focus.window.windowID = window_id;
    (void)dispatch(focus);
    const auto point = [&](Rml::Element *element) {
        const auto pos = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const auto size = element->GetBox().GetSize(Rml::BoxArea::Border);
        const float density = SDL_GetWindowPixelDensity(window.get());
        return Rml::Vector2f{(pos.x + size.x / 2) / density, (pos.y + size.y / 2) / density};
    };
    const auto click = [&](Rml::Element *element) {
        const auto pos = point(element);
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.windowID = window_id;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = pos.x;
        event.button.y = pos.y;
        const auto down = dispatch(event);
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        const auto up = dispatch(event);
        if (!down.consumed || !up.consumed)
            std::cerr << "Click capture: down=" << down.consumed << " up=" << up.consumed << " pointer=" << down.pointer
                      << "/" << up.pointer << '\n';
        return down.consumed && up.consumed;
    };
    require(click(input), "Text field did not capture pointer events");
    ui.update();
    require(ui.input_state().keyboard && ui.input_state().text && SDL_TextInputActive(window.get()),
            "Text input did not activate");
    SDL_Rect caret{};
    int cursor = 0;
    require(SDL_GetTextInputArea(window.get(), &caret, &cursor), "SDL caret area unavailable");
    const auto input_point = point(input);
    require(caret.x >= 28 * logical_scale && caret.x < input_point.x && caret.y > 0 &&
                caret.y < input_point.y + 25 * logical_scale && caret.h <= 40 * logical_scale,
            "Text caret was not converted to SDL window coordinates");
    SDL_Event text{};
    text.type = SDL_EVENT_TEXT_INPUT;
    text.text.windowID = window_id;
    text.text.text = "Ren\xc3\xa9";
    require(dispatch(text).consumed, "UTF-8 text was not captured");
    require(input->GetValue() == "Ren\xc3\xa9", "UTF-8 edit did not reach RmlUi input");
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.windowID = window_id;
    key.key.key = SDLK_BACKSPACE;
    require(dispatch(key).consumed, "Editing key escaped UI capture");
    key.type = SDL_EVENT_KEY_UP;
    (void)dispatch(key);
    require(input->GetValue() == "Ren", "Backspace split a UTF-8 character or failed");
    text.text.text = "\xc3\xa9";
    (void)dispatch(text);
    capture("edited");
    require(click(action) && clicks == 1, "Button click event did not reach the consumer listener");
    require(!SDL_TextInputActive(window.get()), "Text input remained active after blur");
    require(click(select), "Select did not capture pointer input");
    frame();
    capture("dropdown");
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_DOWN;
    (void)dispatch(key);
    key.type = SDL_EVENT_KEY_UP;
    (void)dispatch(key);
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_RETURN;
    (void)dispatch(key);
    key.type = SDL_EVENT_KEY_UP;
    (void)dispatch(key);
    ui.update();
    require(select->GetValue() == "b", "Select keyboard navigation did not change value");
    const auto scroll_pos = point(scroll);
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.windowID = window_id;
    motion.motion.x = scroll_pos.x;
    motion.motion.y = scroll_pos.y;
    (void)dispatch(motion);
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.windowID = window_id;
    wheel.wheel.y = -3;
    require(dispatch(wheel).consumed, "Scroll wheel escaped an interactive panel");
    for (unsigned i = 0; i < 5; ++i) {
        SDL_Delay(20);
        frame();
    }
    require(scroll->GetScrollTop() > 0, "RmlUi overflow did not scroll");
    require(click(input), "Refocusing input failed");
    ui.update();
    document.hide();
    ui.update();
    require(!ui.input_state().keyboard && !ui.input_state().text && !SDL_TextInputActive(window.get()),
            "Hidden document retained keyboard/text capture");
    document.show();
    ui.update();
    {
        const auto pos = point(action);
        SDL_Event held{};
        held.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        held.button.windowID = window_id;
        held.button.button = SDL_BUTTON_LEFT;
        held.button.x = pos.x;
        held.button.y = pos.y;
        require(dispatch(held).consumed && ui.input_state().pointer, "Hidden-drag fixture did not capture");
        document.hide();
        ui.update();
        require(!ui.input_state().pointer, "Hidden document retained a held pointer");
        held.type = SDL_EVENT_MOUSE_BUTTON_UP;
        require(dispatch(held).consumed && clicks == 1, "Cancelled UI drag escaped or clicked");
        document.show();
        ui.update();
    }
    motion.motion.x = 600 * logical_scale;
    motion.motion.y = 420 * logical_scale;
    require(!dispatch(motion).consumed && !ui.input_state().pointer, "Uncovered world region captured pointer");
    SDL_Event drag{};
    drag.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    drag.button.windowID = window_id;
    drag.button.button = SDL_BUTTON_LEFT;
    drag.button.x = motion.motion.x;
    drag.button.y = motion.motion.y;
    require(!dispatch(drag).consumed, "World drag started in UI");
    const auto button_point = point(action);
    motion.motion.x = button_point.x;
    motion.motion.y = button_point.y;
    require(!dispatch(motion).consumed && !ui.input_state().pointer, "UI stole a world drag crossing a panel");
    require(!dispatch(wheel).consumed, "UI stole scrolling during a world drag");
    drag.type = SDL_EVENT_MOUSE_BUTTON_UP;
    drag.button.x = button_point.x;
    drag.button.y = button_point.y;
    require(!dispatch(drag).consumed && clicks == 1, "World drag release activated UI");
    drag.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    require(dispatch(drag).consumed, "UI drag did not start");
    motion.motion.x = 600 * logical_scale;
    motion.motion.y = 420 * logical_scale;
    require(dispatch(motion).consumed && ui.input_state().pointer, "UI drag escaped into world input");
    drag.type = SDL_EVENT_MOUSE_BUTTON_UP;
    drag.button.x = motion.motion.x;
    drag.button.y = motion.motion.y;
    require(dispatch(drag).consumed && !ui.input_state().pointer && clicks == 1, "UI drag release escaped or clicked");
    document.hide();
    ui.update();
    document.show();
    ui.update();
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_W;
    require(!dispatch(key).consumed, "Unfocused UI captured gameplay WASD");
    key.type = SDL_EVENT_KEY_UP;
    (void)dispatch(key);
    require(click(input), "Focus before focus-loss check failed");
    ui.update();
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    require(dispatch(focus).focus_lost && !ui.input_state().keyboard && !SDL_TextInputActive(window.get()),
            "Focus loss did not release text/keyboard");
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    (void)dispatch(focus);
    auto mesh = std::make_shared<anima::MeshSnapshot>();
    mesh->vertices = {{{-1, -1, .4F}, {0, 0, 1}, {.1F, .3F, .8F}, {}},
                      {{1, -1, .4F}, {0, 0, 1}, {.1F, .3F, .8F}, {}},
                      {{0, 1, .4F}, {0, 0, 1}, {.1F, .3F, .8F}, {}}};
    anima::Primitive primitive;
    primitive.vertex_count = 3;
    mesh->primitives.push_back(primitive);
    renderer.set_scenes({reference_test::scene(*mesh)});
    renderer.request_capture(output / "mesh-no-ui.ppm");
    while (!renderer.draw())
        SDL_Delay(5);
    ++frames;
    ++captures;
    capture("mesh-ui");
    for (unsigned i = 0; i < 8; ++i) {
        renderer.set_scenes({});
        frame();
        renderer.set_scenes({reference_test::scene(*mesh)});
        frame();
    }
    capture("mesh-ui-repeat");
    // A consumer can miss a window notification. Layout still sees the new SDL
    // dimensions; the renderer must recover without waiting for acquisition,
    // which is skipped while that layout disagrees with the stale swapchain.
    forward_window_events = false;
    require(SDL_SetWindowSize(window.get(), int(760 * logical_scale), int(540 * logical_scale)), "UI resize failed");
    require(SDL_SyncWindow(window.get()), "UI resize did not settle");
    ui.context().SetDensityIndependentPixelRatio(.5F);
    frame();
    capture("resized-ui");
    forward_window_events = true;
    require(ui.context().GetDensityIndependentPixelRatio() == SDL_GetWindowDisplayScale(window.get()),
            "UI DPI ratio is stale");
    int width = 0, height = 0;
    require(SDL_GetWindowSizeInPixels(window.get(), &width, &height), "No UI drawable");
    require(ui.context().GetDimensions() == Rml::Vector2i(width, height), "UI framebuffer dimensions are stale");
    require(click(input), "Hit testing failed after resize/DPI conversion");
    require(SDL_MinimizeWindow(window.get()), "UI minimize failed");
    while (!(SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED)) {
        pump();
        SDL_Delay(5);
    }
    renderer.set_scenes({});
    ui.update();
    require(!ui.render(), "Minimized UI rendered");
    require(SDL_RestoreWindow(window.get()), "UI restore failed");
    while (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED) {
        pump();
        SDL_Delay(5);
    }
    capture("restored-ui");
    subscription.disconnect();
    const auto ui_stats = ui.stats();
    require(!ui_stats.log_errors && !ui_stats.log_warnings, "RmlUi emitted diagnostics");
    ui.shutdown();
    ui.shutdown();
    require(!SDL_TextInputActive(window.get()), "UI shutdown left text input active");
    renderer.request_capture(output / "after-ui-shutdown.ppm");
    while (!renderer.draw())
        SDL_Delay(5);
    ++frames;
    ++captures;
    // RmlUi globals, font atlases and GPU sources can be recreated repeatedly.
    for (unsigned i = 0; i < 3; ++i) {
        anima::UiContext again(window.get(), renderer);
        again.load_font(assets / "LatoLatin-Regular.ttf");
        auto reopened = again.open_document(assets / "controls.rml");
        reopened.show();
        again.update();
        while (!again.render()) {
            again.update();
            SDL_Delay(5);
        }
        ++frames;
        again.shutdown();
    }
    unsigned rejected_features = 0, unsupported_warnings = 0;
    for (const auto *style : {"filter: blur(2px);", "transform: rotate(15deg); overflow: hidden;"}) {
        anima::UiContext unsupported(window.get(), renderer);
        auto invalid = unsupported.documents().from_memory(
            std::string(
                "<rml><head></head><body><div style='position:absolute;left:20px;top:20px;width:80px;height:80px;") +
            style +
            "'><div style='display:block;width:160px;height:160px;background-color:red;'></div></div></body></rml>");
        require(invalid.valid(), "Unsupported-feature fixture did not load");
        invalid.show();
        unsupported.update();
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            bool rejected = false;
            try {
                (void)unsupported.render();
            } catch (const anima::UiUnsupportedFeature &) {
                rejected = true;
            }
            require(rejected, "Unsupported render feature was silently accepted or ceased failing");
        }
        ++rejected_features;
        unsupported_warnings += unsupported.stats().log_warnings;
        require(!unsupported.stats().log_errors, "Unsupported-feature fixture emitted an RmlUi error");
    }
    { // A corrected document can render after unsupported contexts are destroyed.
        anima::UiContext recovered(window.get(), renderer);
        recovered.load_font(assets / "LatoLatin-Regular.ttf");
        auto reopened = recovered.open_document(assets / "controls.rml");
        reopened.show();
        recovered.update();
        while (!recovered.render()) {
            recovered.update();
            SDL_Delay(5);
        }
        ++frames;
    }
    const auto stats = renderer.shutdown();
    require(stats.presented_frames == frames && stats.capture_count == captures && !stats.validation_errors &&
                !stats.validation_warnings,
            "UI renderer counters or validation failed");
    std::cout << "RESULT {\"frames\":" << frames << ",\"captures\":" << captures << ",\"clicks\":" << clicks
              << ",\"utf8_edit\":true,\"select_keyboard\":true,\"scroll\":true,"
                 "\"hidden_focus_released\":true,\"world_input_passthrough\":true,\"resize_hit_test\":true,"
                 "\"drag_ownership\":true,\"hidden_drag_released\":true,\"caret_coordinates\":true,\"duplicate_owner_"
                 "rejected\":true,\"context_"
                 "recreations\":4,"
              << "\"rejected_features\":" << rejected_features << ",\"unsupported_warnings\":" << unsupported_warnings
              << ",\"display_scale\":" << SDL_GetWindowDisplayScale(window.get())
              << ",\"ui_warnings\":0,\"ui_errors\":0,\"validation_warnings\":0,\"validation_errors\":0}\n";
    return 0;
}
} // namespace ui_test
