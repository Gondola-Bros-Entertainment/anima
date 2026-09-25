#include <SDL3/SDL_events.h>
#include <anima/input_sdl.hpp>
#include <iostream>
namespace i = anima::input;
namespace {
void check(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
void run_chord() {
    i::Binding save{{i::ControlKind::key, SDL_SCANCODE_S}};
    save.modifiers = {{i::ControlKind::key, SDL_SCANCODE_LCTRL}};
    i::Context shortcuts({{"save", i::ActionType::button, {save}}});
    const auto key = [&](SDL_Scancode code, bool down, SDL_KeyboardID device) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = 8;
        event.key.which = device;
        event.key.scancode = code;
        const auto converted = i::from_sdl(event, 8);
        check(bool(converted), "SDL chord key event was not converted");
        shortcuts.process(*converted);
    };
    key(SDL_SCANCODE_S, true, 3);
    check(!shortcuts.state("save").active, "SDL shortcut ignored its modifier");
    key(SDL_SCANCODE_LCTRL, true, 4);
    check(!shortcuts.state("save").active, "SDL shortcut combined separate keyboards");
    key(SDL_SCANCODE_LCTRL, true, 3);
    check(shortcuts.state("save").pressed, "SDL modifier did not activate an already held primary");
    shortcuts.begin_frame();
    key(SDL_SCANCODE_LCTRL, false, 3);
    check(shortcuts.state("save").released && !shortcuts.state("save").canceled,
          "SDL modifier release did not release its chord");
    key(SDL_SCANCODE_S, true, 4);
    check(shortcuts.state("save").active && shortcuts.state("save").pressed && shortcuts.state("save").released,
          "SDL chord lost a second keyboard or latched edges");
    shortcuts.begin_frame();
    SDL_Event event{};
    event.type = SDL_EVENT_KEYBOARD_REMOVED;
    event.kdevice.which = 4;
    shortcuts.process(*i::from_sdl(event, 8));
    check(!shortcuts.state("save").active && shortcuts.state("save").released,
          "SDL keyboard removal retained a complete chord");
    key(SDL_SCANCODE_LCTRL, true, 3);
    check(shortcuts.state("save").active, "SDL keyboard removal cleared another keyboard's primary");
    event = {};
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    event.window.windowID = 8;
    shortcuts.process(*i::from_sdl(event, 8));
    check(shortcuts.state("save").canceled && !shortcuts.state("save").pressed,
          "SDL focus loss retained pending chord input");
    event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    shortcuts.process(*i::from_sdl(event, 8));
    shortcuts.begin_frame();
    key(SDL_SCANCODE_S, true, 3);
    check(!shortcuts.state("save").active, "SDL focus gain replayed the old chord modifier");
}
void run() {
    i::Context c({{"accept", i::ActionType::button, {{{i::ControlKind::key, SDL_SCANCODE_SPACE}}}},
                  {"stick", i::ActionType::axis, {{{i::ControlKind::gamepad_axis, SDL_GAMEPAD_AXIS_LEFTX}}}}});
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = 8;
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.which = 3;
    check(!i::from_sdl(e, 7), "Foreign window event escaped filter");
    auto converted = i::from_sdl(e, 8);
    check(bool(converted), "Keyboard event conversion missing");
    c.process(*converted);
    check(c.state("accept").pressed, "Converted keyboard did not drive action");
    e.key.repeat = true;
    check(!i::from_sdl(e, 8), "Keyboard repeat was forwarded");
    e = {};
    e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    e.window.windowID = 8;
    c.process(*i::from_sdl(e, 8));
    check(c.state("accept").canceled, "SDL focus loss retained input");
    e.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    c.process(*i::from_sdl(e, 8));
    e = {};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = 12;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    e.gaxis.value = -32768;
    c.process(*i::from_sdl(e, 8));
    check(c.state("stick").value.x == -1, "Negative SDL axis endpoint incorrect");
    e.gaxis.value = 32767;
    c.process(*i::from_sdl(e, 8));
    check(c.state("stick").value.x == 1, "Positive SDL axis endpoint incorrect");
    e = {};
    e.type = SDL_EVENT_GAMEPAD_REMOVED;
    e.gdevice.which = 12;
    c.process(*i::from_sdl(e, 8));
    check(c.state("stick").value.x == 0, "SDL disconnect retained input");
    e = {};
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = 8;
    e.button.which = 0;
    e.button.button = SDL_BUTTON_LEFT;
    converted = i::from_sdl(e, 8);
    check(converted && converted->source.kind == i::ControlKind::mouse_button && converted->value == 1,
          "SDL mouse conversion failed");
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    check(i::from_sdl(e, 8)->value == 0, "SDL mouse release conversion failed");
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.which = SDL_TOUCH_MOUSEID;
    check(!i::from_sdl(e, 8), "Touch-emulated mouse was treated as an ordinary device");
    e = {};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = 8;
    e.key.scancode = SDL_SCANCODE_COUNT;
    bool invalid_code = false;
    try {
        (void)i::from_sdl(e, 8);
    } catch (const std::invalid_argument &) {
        invalid_code = true;
    }
    check(invalid_code, "SDL scancode count sentinel was accepted as a key");
    e = {};
    e.type = SDL_EVENT_TEXT_INPUT;
    check(!i::from_sdl(e, 8), "Text was treated as an action input");
    bool caught = false;
    try {
        (void)i::from_sdl(e, 0);
    } catch (const std::invalid_argument &) {
        caught = true;
    }
    check(caught, "Missing target window was accepted");
    run_chord();
}
} // namespace
int main() {
    try {
        run();
        std::cout << "PASS SDL input conversion without SDL runtime/window/device\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
