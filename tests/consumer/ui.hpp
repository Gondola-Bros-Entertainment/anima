#pragma once
#include "gpu_checks.hpp"
#include "reference.hpp"
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <anima/assets/mesh_snapshot.hpp>
#include <anima/ui/context.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

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
// The removed Python check's comparisons of the UI captures, in the document's density-independent pixels, which
// @p scale converts to framebuffer pixels: exact rectangular clipping, a translated element, white at alpha
// 128/255 blended in linear light over the clear color and over a mesh, the external PNG's texels, rendered
// glyphs, the edited text, and stable pixels across scene replacement.
inline void check_images(const gpu_check::Captures &images, float scale) {
    const auto at = [&](const std::string &name, double x, double y) {
        return gpu_check::pixel(images[name], std::size_t(x * scale), std::size_t(y * scale));
    };
    const auto linear = [](int byte) {
        const auto s = byte / 255.;
        return s <= .04045 ? s / 12.92 : std::pow((s + .055) / 1.055, 2.4);
    };
    const auto srgb = [](double value) {
        return int(std::nearbyint(255 * (value <= .0031308 ? 12.92 * value : 1.055 * std::pow(value, 1 / 2.4) - .055)));
    };
    constexpr int darkest_background = 80, alpha_tolerance = 3, texel_tolerance = 8;
    const auto background = at("empty-ui", 0, 0);
    images.require(std::max({background[0], background[1], background[2]}) < darkest_background,
                   "The UI background is " + gpu_check::text(background), {"empty-ui"});
    // The 160x80 red child is clipped to its 100x50 parent, exactly.
    const auto &empty = images["empty-ui"];
    const auto left = std::size_t(350 * scale), width = std::size_t(100 * scale);
    for (auto y = std::size_t(40 * scale); y < std::size_t(90 * scale); ++y)
        for (auto x = left; x < left + width; ++x)
            images.require(gpu_check::pixel(empty, x, y) == gpu_check::Rgb{255, 0, 0},
                           "The clipped red element is not solid red at " + std::to_string(x) + ", " +
                               std::to_string(y),
                           {"empty-ui"});
    for (const auto &[x, y] :
         {std::pair{349, 60}, std::pair{450, 60}, std::pair{380, 39}, std::pair{380, 90}, std::pair{480, 100}})
        images.require(at("empty-ui", x, y) == background,
                       "Clipping leaked at " + std::to_string(x) + ", " + std::to_string(y), {"empty-ui"});
    images.require(at("empty-ui", 390, 250) == gpu_check::Rgb{0, 255, 0} && at("empty-ui", 360, 240) == background,
                   "The translated green element is not where its transform puts it", {"empty-ui"});
    constexpr double alpha = 128 / 255.;
    std::array<int, 3> over_clear{};
    for (const auto &[image, under] : {std::pair{std::string("empty-ui"), background},
                                       std::pair{std::string("mesh-ui"), at("mesh-no-ui", 400, 160)}}) {
        const auto actual = at(image, 400, 160);
        gpu_check::Rgb expected{};
        for (std::size_t c = 0; c < 3; ++c)
            expected[c] = srgb(alpha + linear(under[c]) * (1 - alpha));
        if (image == "empty-ui")
            over_clear = expected;
        images.require(gpu_check::difference(actual, expected) <= alpha_tolerance,
                       "White at alpha 128/255 over " + gpu_check::text(under) + " in " + image + " is " +
                           gpu_check::text(actual) + " instead of " + gpu_check::text(expected),
                       {image, "mesh-no-ui"});
    }
    images.require(at("mesh-ui", 400, 160) != at("empty-ui", 400, 160),
                   "The mesh does not show through the translucent element", {"mesh-ui", "empty-ui"});
    // The document draws the 2x2 image at 66dp from (480, 230), so each texel spans 33dp and its center is a pixel
    // center at scale 1, where bilinear filtering returns the texel without its neighbors.
    constexpr double image_left = 480, image_top = 230, texel_span = 33;
    for (const auto &[column, row, expected] :
         {std::tuple{0, 0, gpu_check::Rgb{255, 0, 0}}, std::tuple{1, 0, gpu_check::Rgb{0, 255, 0}},
          std::tuple{0, 1, gpu_check::Rgb{0, 0, 255}}, std::tuple{1, 1, over_clear}}) {
        const auto actual =
            at("empty-ui", image_left + (column + .5) * texel_span, image_top + (row + .5) * texel_span);
        images.require(gpu_check::difference(actual, expected) <= texel_tolerance,
                       "The PNG texel in column " + std::to_string(column) + ", row " + std::to_string(row) + " is " +
                           gpu_check::text(actual) + " instead of " + gpu_check::text(expected),
                       {"empty-ui"});
    }
    // The font atlas draws actual glyphs, and the UTF-8 edit changes the input field.
    constexpr int bright_glyph = 200, edited_change = 30;
    constexpr double least_glyphs = 80, least_edits = 30;
    const auto &edited = images["edited"];
    std::size_t glyphs = 0, edits = 0;
    for (auto y = std::size_t(30 * scale); y < std::size_t(55 * scale); ++y)
        for (auto x = std::size_t(28 * scale); x < std::size_t(200 * scale); ++x) {
            const auto c = gpu_check::pixel(empty, x, y);
            if (std::min({c[0], c[1], c[2]}) > bright_glyph)
                ++glyphs;
        }
    for (auto y = std::size_t(70 * scale); y < std::size_t(97 * scale); ++y)
        for (auto x = std::size_t(33 * scale); x < std::size_t(145 * scale); ++x)
            if (gpu_check::difference(gpu_check::pixel(empty, x, y), gpu_check::pixel(edited, x, y)) > edited_change)
                ++edits;
    images.require(double(glyphs) > least_glyphs * scale * scale && double(edits) > least_edits * scale * scale,
                   std::to_string(glyphs) + " glyph pixels and " + std::to_string(edits) + " edited pixels",
                   {"empty-ui", "edited"});
    images.require_same("mesh-ui", "mesh-ui-repeat", "UI and scene replacement changed stable pixels");
    images.require(!gpu_check::same(images["dropdown"], images["edited"]), "The dropdown produced no visible change",
                   {"dropdown", "edited"});
    images.require(!gpu_check::same_size(images["resized-ui"], empty), "The resized UI kept its capture size",
                   {"resized-ui", "empty-ui"});
    images.require_clear("after-ui-shutdown", "The UI kept drawing after its shutdown");
}
inline int run(int argc, char **argv) {
    require(argc >= 4, "Usage: consumer --ui OUTPUT ASSETS [--no-present-fences]");
    const std::filesystem::path output = argv[2], assets = argv[3];
    gpu_check::Video video;
    const auto window =
        gpu_check::window("Anima external UI consumer", 640, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    const float logical_scale = SDL_GetWindowDisplayScale(window.get()) / SDL_GetWindowPixelDensity(window.get());
    require(SDL_SetWindowSize(window.get(), int(640 * logical_scale), int(480 * logical_scale)),
            "UI initial sizing failed");
    require(SDL_SyncWindow(window.get()), "UI initial sizing did not settle");
    anima::RendererOptions options;
    options.validation = true;
    options.disable_present_fences = argc > 4 && std::string_view(argv[4]) == "--no-present-fences";
    anima::VulkanRenderer renderer(window.get(), options);
    renderer.set_view(anima::identity());
    renderer.request_capture(); // The first frame shows the document over the clear color.
    gpu_check::Captures images(output);
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
        require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "UI watchdog expired");
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                    "UI smoke interrupted");
            if (forward_window_events || !SDL_GetWindowFromEvent(&event))
                (void)ui.process_event(event);
        }
    };
    std::uint64_t ui_frames = 0; // Frames that this context's render() calls presented.
    const auto frame = [&] {
        for (;;) {
            pump();
            ui.update();
            if (ui.render()) {
                ++frames;
                ++ui_frames;
                break;
            }
            SDL_Delay(5);
        }
    };
    const auto capture = [&](const char *name) {
        renderer.request_capture();
        frame();
        images.add(name, gpu_check::take(renderer));
        ++captures;
    };
    frame();
    images.add("empty-ui", gpu_check::take(renderer));
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
    // Malformed pointer input goes straight to the context, which must reject it before the event
    // changes anything, whatever presses are held.
    const float not_a_number = std::numeric_limits<float>::quiet_NaN();
    constexpr float beyond_int_range = 1e10F; // Farther than any int framebuffer coordinate.
    constexpr std::string_view invalid_coordinates = "Invalid UI pointer coordinates";
    const auto rejects = [&](const SDL_Event &event, std::string_view expected, const char *failure) {
        bool rejected = false;
        try {
            (void)ui.process_event(event);
        } catch (const std::invalid_argument &error) {
            rejected = std::string_view(error.what()) == expected;
        }
        require(rejected, failure);
    };
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
    auto malformed_wheel = wheel;
    malformed_wheel.wheel.y = not_a_number;
    rejects(malformed_wheel, "Invalid UI wheel delta", "A non-finite wheel delta was accepted");
    require(scroll->GetScrollTop() == 0, "A rejected wheel event scrolled the panel");
    require(dispatch(wheel).consumed, "Scroll wheel escaped an interactive panel");
    for (unsigned i = 0; i < 5; ++i) {
        SDL_Delay(20);
        frame();
    }
    require(scroll->GetScrollTop() > 0, "RmlUi overflow did not scroll");
    // SDL's wheel x is positive to the right, as RmlUi's is, and its y is positive for scrolling up.
    // Both already follow the platform's natural-scrolling setting, which direction only records.
    auto rightward = wheel;
    rightward.wheel.x = 1;
    rightward.wheel.y = 0;
    require(dispatch(rightward).consumed && scroll->GetScrollLeft() > 0, "A rightward wheel did not scroll right");
    auto flipped_upward = wheel;
    flipped_upward.wheel.y = 1;
    flipped_upward.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    const float scrolled_down = scroll->GetScrollTop();
    require(dispatch(flipped_upward).consumed && scroll->GetScrollTop() < scrolled_down,
            "A flipped wheel scrolled against the direction its values report");
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
        auto malformed_release = held;
        malformed_release.type = SDL_EVENT_MOUSE_BUTTON_UP;
        malformed_release.button.y = not_a_number;
        rejects(malformed_release, invalid_coordinates, "A non-finite release of a cancelled press was accepted");
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
    auto malformed = drag;
    malformed.type = SDL_EVENT_MOUSE_BUTTON_UP;
    malformed.button.x = not_a_number;
    rejects(malformed, invalid_coordinates, "A non-finite release during a world drag was accepted");
    malformed = motion;
    malformed.motion.y = not_a_number;
    rejects(malformed, invalid_coordinates, "Non-finite motion during a world drag was accepted");
    malformed = drag;
    malformed.button.button = SDL_BUTTON_RIGHT;
    malformed.button.x = beyond_int_range;
    rejects(malformed, invalid_coordinates, "An out-of-range press during a world drag was accepted");
    require(!dispatch(motion).consumed && !ui.input_state().pointer, "A rejected pointer event ended the world drag");
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
    // The UI drag above left the HUD button focused. It must not take gameplay keys, and a key keeps its
    // starting ownership: a release whose press reached gameplay goes to gameplay after a control takes focus.
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_W;
    key.key.scancode = SDL_SCANCODE_W;
    require(!ui.input_state().keyboard && !dispatch(key).consumed, "Focused button captured gameplay WASD");
    require(click(action) && clicks == 2, "HUD click during a held key failed");
    key.type = SDL_EVENT_KEY_UP;
    require(!dispatch(key).consumed, "Key release after a HUD click was captured");
    key.type = SDL_EVENT_KEY_DOWN;
    require(!dispatch(key).consumed, "HUD focus captured the next gameplay key");
    require(click(input), "Text field click failed");
    ui.update();
    require(ui.input_state().keyboard, "Text field did not capture the keyboard");
    key.type = SDL_EVENT_KEY_UP;
    require(!dispatch(key).consumed, "Release of a key held before typing was captured");
    key.type = SDL_EVENT_KEY_DOWN;
    require(dispatch(key).consumed, "Typing reached gameplay");
    key.type = SDL_EVENT_KEY_UP;
    require(dispatch(key).consumed, "Typed key release reached gameplay");
    SDL_Event world{};
    world.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    world.button.windowID = window_id;
    world.button.button = SDL_BUTTON_LEFT;
    world.button.x = 600 * logical_scale;
    world.button.y = 420 * logical_scale;
    require(!dispatch(world).consumed, "World click was captured");
    world.type = SDL_EVENT_MOUSE_BUTTON_UP;
    (void)dispatch(world);
    ui.update();
    require(!ui.input_state().keyboard && !SDL_TextInputActive(window.get()), "World click left text capture");
    key.type = SDL_EVENT_KEY_DOWN;
    require(!dispatch(key).consumed, "Gameplay keys stayed captured after a world click");
    key.type = SDL_EVENT_KEY_UP;
    (void)dispatch(key);
    // Text input that the application starts is the application's to stop: events and updates leave it
    // running while no control edits text, and so does a text field that takes focus and loses it.
    require(SDL_StartTextInput(window.get()), "Application text input did not start");
    (void)dispatch(motion);
    ui.update();
    require(SDL_TextInputActive(window.get()) && !ui.input_state().text,
            "The UI stopped text input that the application started");
    require(click(input), "Text field click during application text input failed");
    ui.update();
    require(ui.input_state().text, "A focused text field did not report the active text input");
    world.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    (void)dispatch(world);
    world.type = SDL_EVENT_MOUSE_BUTTON_UP;
    (void)dispatch(world);
    ui.update();
    require(!ui.input_state().keyboard && SDL_TextInputActive(window.get()),
            "Blurring a text field stopped text input that the application started");
    require(SDL_StopTextInput(window.get()), "Application text input did not stop");
    require(click(input), "Focus before focus-loss check failed");
    ui.update();
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    require(dispatch(focus).focus_lost && !ui.input_state().keyboard && !SDL_TextInputActive(window.get()),
            "Focus loss did not release text/keyboard");
    malformed = motion;
    malformed.motion.x = not_a_number;
    rejects(malformed, invalid_coordinates, "Non-finite motion was accepted while the window lacked focus");
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    (void)dispatch(focus);
    // A mouse capture that the application requests is the application's to release. SDL shows a capture in
    // the window flags only while the window has keyboard focus to request it and mouse focus to hold it, and
    // mouse focus needs the cursor inside the window, so these checks run only when a capture shows.
    const auto mouse_captured = [&] { return (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MOUSE_CAPTURE) != 0; };
    const auto skip_capture = [&](const char *checks) {
        std::cout << "SKIP " << checks << ": keyboard focus " << (SDL_GetKeyboardFocus() == window.get() ? "yes" : "no")
                  << ", mouse focus " << (SDL_GetMouseFocus() == window.get() ? "yes" : "no") << '\n';
    };
    const bool capture_shows = SDL_CaptureMouse(true) && mouse_captured();
    if (capture_shows) {
        focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        (void)dispatch(focus);
        require(mouse_captured(), "UI focus loss released the application's mouse capture");
        focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        (void)dispatch(focus);
        require(click(doc.GetElementById("status")) && mouse_captured(),
                "A UI press released the application's mouse capture");
    } else {
        skip_capture("UI focus loss keeps the application's mouse capture");
        skip_capture("a UI press keeps the application's mouse capture");
    }
    (void)SDL_CaptureMouse(false);
    auto mesh = std::make_shared<anima::MeshSnapshot>();
    mesh->vertices = {{{-1, -1, .4F}, {0, 0, 1}, {.1F, .3F, .8F}, {}},
                      {{1, -1, .4F}, {0, 0, 1}, {.1F, .3F, .8F}, {}},
                      {{0, 1, .4F}, {0, 0, 1}, {.1F, .3F, .8F}, {}}};
    anima::Primitive primitive;
    primitive.vertex_count = 3;
    mesh->primitives.push_back(primitive);
    renderer.set_scenes({reference_test::scene(*mesh)});
    renderer.request_capture();
    while (!renderer.draw())
        SDL_Delay(5);
    images.add("mesh-no-ui", gpu_check::take(renderer));
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
    require(ui_stats.rendered_frames == ui_frames, "UI frame count differs from the frames its renders presented");
    const bool capture_held = SDL_CaptureMouse(true) && mouse_captured();
    if (!capture_held)
        skip_capture("UI shutdown keeps the application's mouse capture");
    ui.shutdown();
    ui.shutdown();
    require(!SDL_TextInputActive(window.get()), "UI shutdown left text input active");
    require(!capture_held || mouse_captured(), "UI shutdown released the application's mouse capture");
    (void)SDL_CaptureMouse(false);
    renderer.request_capture();
    while (!renderer.draw())
        SDL_Delay(5);
    images.add("after-ui-shutdown", gpu_check::take(renderer));
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
    { // As in RmlUi, an image that cannot be loaded is a warning, and the frames after it still render.
        constexpr unsigned unloadable_images = 2;  // A missing file and a file that is not PNG or JPEG.
        constexpr unsigned warnings_per_image = 2; // The reason, then RmlUi's own report of the texture.
        constexpr unsigned checked_frames = 2;
        // The unreadable file lives only as long as this check.
        struct Fixture {
            std::filesystem::path directory;
            ~Fixture() {
                std::error_code ignored;
                std::filesystem::remove_all(directory, ignored);
            }
        } fixture{output / "unloadable-images"};
        std::filesystem::create_directories(fixture.directory);
        std::ofstream(fixture.directory / "unsupported.tga", std::ios::binary) << "not a PNG or JPEG image";
        anima::UiContext unloadable_context(window.get(), renderer);
        // RmlUi resolves decorator images against their style sheet, which an inline style lacks.
        auto unloadable = unloadable_context.documents().from_memory(
            "<rml><head><style>#decorated { display: block; width: 32px; height: 32px; "
            "decorator: image(unsupported.tga); }</style></head><body>"
            "<img src='missing.png' style='width:32px;height:32px;'/><div id='decorated'></div></body></rml>",
            (fixture.directory / "unloadable-images.rml").string());
        unloadable.show();
        for (unsigned frame_index = 0; frame_index < checked_frames; ++frame_index) {
            bool presented = false;
            try {
                unloadable_context.update();
                while (!(presented = unloadable_context.render())) {
                    SDL_Delay(5);
                    unloadable_context.update();
                }
            } catch (const anima::UiUnsupportedFeature &error) {
                std::cerr << "Frame " << frame_index << " threw: " << error.what() << '\n';
            }
            require(presented, "An image that could not be loaded stopped UI rendering");
            ++frames;
        }
        require(unloadable_context.stats().log_warnings == unloadable_images * warnings_per_image &&
                    !unloadable_context.stats().log_errors,
                "An image that could not be loaded was not reported as a warning");
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
    { // Once the renderer is fatal, render() reports the renderer's own failure, as VulkanRenderer::draw does.
        anima::UiContext failing(window.get(), renderer);
        failing.load_font(assets / "LatoLatin-Regular.ttf");
        auto controls = failing.open_document(assets / "controls.rml");
        controls.show();
        failing.update();
        anima::SceneReplacementOptions device_loss;
        device_loss.fail_after = anima::RendererFailureStage::device_lost;
        bool fatal = false;
        try {
            renderer.set_scenes({reference_test::scene(*mesh)}, device_loss);
        } catch (const anima::RendererFatalError &) {
            fatal = true;
        }
        require(fatal, "Injected device loss did not make the renderer fatal");
        std::string drawn, rendered;
        try {
            (void)renderer.draw();
        } catch (const anima::RendererFatalError &error) {
            drawn = error.what();
        }
        try {
            (void)failing.render();
        } catch (const anima::RendererFatalError &error) {
            rendered = error.what();
        }
        if (rendered != drawn)
            std::cerr << "draw(): " << drawn << "\nrender(): " << rendered << '\n';
        require(!drawn.empty() && rendered == drawn, "UI rendering replaced the renderer's fatal failure");
        // A fatal renderer stays fatal after shutdown, when both report the shutdown instead.
        (void)renderer.shutdown();
        const auto shutdown_error = [](const auto &call) {
            try {
                (void)call();
            } catch (const std::logic_error &error) {
                return std::string(error.what());
            } catch (const std::exception &error) {
                return std::string("not a std::logic_error: ") + error.what();
            }
            return std::string("no exception");
        };
        drawn = shutdown_error([&] { return renderer.draw(); });
        rendered = shutdown_error([&] { return failing.render(); });
        if (rendered != drawn)
            std::cerr << "draw(): " << drawn << "\nrender(): " << rendered << '\n';
        require(rendered == drawn && drawn == "Renderer is shut down",
                "UI rendering replaced the shutdown error of a failed renderer");
    }
    const auto stats = renderer.shutdown();
    require(stats.presented_frames == frames && stats.capture_count == captures && !stats.validation_errors &&
                !stats.validation_warnings,
            "UI renderer counters or validation failed");
    // Construction checks the renderer's surface format, so a shut-down renderer is rejected there.
    bool shut_down_rejected = false;
    try {
        anima::UiContext late(window.get(), renderer);
    } catch (const std::logic_error &error) {
        shut_down_rejected = std::string_view(error.what()) == "Renderer is shut down";
    }
    require(shut_down_rejected, "A UI context was created over a shut-down renderer");
    constexpr unsigned expected_captures = 9, expected_rejected_features = 2;
    require(captures == expected_captures && images.size() == captures &&
                rejected_features == expected_rejected_features,
            "The UI check took unexpected captures or rejected unexpected features");
    check_images(images, SDL_GetWindowDisplayScale(window.get()));
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
