#pragma once
#include <anima/input.hpp>
#include <stdexcept>
#ifdef CONSUMER_ASSETS
#include <anima/input_scene.hpp>
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
#include <array>
#endif
#ifdef CONSUMER_INPUT_SDL
#include <SDL3/SDL_events.h>
#include <anima/input_sdl.hpp>
#endif
inline void consume_input() {
    namespace i = anima::input;
    i::Map map{{"interact", i::ActionType::button, {{{i::ControlKind::key, 44}}}}};
    i::Binding save{{i::ControlKind::key, 22}};
    save.modifiers = {{i::ControlKind::key, 224}};
    map.push_back({"save", i::ActionType::button, {save}});
    i::Context actions(map);
    actions.begin_frame();
    actions.process({i::EventType::control, {i::ControlKind::key, 44, 0}, 1});
    actions.process({i::EventType::control, {i::ControlKind::key, 44, 0}, 0});
    if (!actions.state("interact").pressed || !actions.state("interact").released)
        throw std::runtime_error("Independent input consumer lost a tap");
    actions.process({i::EventType::control, {i::ControlKind::key, 22, 3}, 1});
    actions.process({i::EventType::control, {i::ControlKind::key, 224, 4}, 1});
    if (actions.state("save").active)
        throw std::runtime_error("Independent input chord combined separate keyboards");
    actions.process({i::EventType::control, {i::ControlKind::key, 224, 3}, 1});
    if (!actions.state("save").pressed)
        throw std::runtime_error("Independent input chord failed to activate");
    actions.begin_frame();
    actions.process({i::EventType::control, {i::ControlKind::key, 224, 3}, 0});
    if (!actions.state("save").released || actions.state("save").canceled)
        throw std::runtime_error("Independent input modifier release did not release its chord");
    i::Context precedence({{"back", i::ActionType::button, {{{i::ControlKind::key, 22}}}}, map[1]});
    precedence.process({i::EventType::control, {i::ControlKind::key, 22, 0}, 1});
    if (!precedence.state("back").active)
        throw std::runtime_error("Independent input plain binding failed to activate");
    precedence.process({i::EventType::control, {i::ControlKind::key, 224, 0}, 1});
    if (!precedence.state("save").active || precedence.state("back").active || precedence.state("back").canceled)
        throw std::runtime_error("Independent input chord did not outrank the plain binding of its primary");
    // A binding that selects a gamepad by identity follows it to the new ID it has after reconnecting.
    i::DeviceIdentity pad{};
    pad[0] = 3;
    i::Binding jump{{i::ControlKind::gamepad_button, 0}};
    jump.control.identity = pad;
    i::Context identified({{"jump", i::ActionType::button, {jump}}});
    identified.process({i::EventType::control, {i::ControlKind::gamepad_button, 0, 7, pad}, 1});
    identified.process({i::EventType::disconnect, {i::ControlKind::gamepad_button, 0, 7}});
    identified.process({i::EventType::control, {i::ControlKind::gamepad_button, 0, 8}, 1});
    if (identified.state("jump").active)
        throw std::runtime_error("Independent input identity matched a gamepad that reported none");
    identified.process({i::EventType::control, {i::ControlKind::gamepad_button, 0, 9, pad}, 1});
    if (!identified.state("jump").active)
        throw std::runtime_error("Independent input identity did not follow its reconnected gamepad");
    // Pointer look and the wheel are delta controls: a frame's increments add up, scaled and unclamped, and the
    // next frame starts from zero. Look pitch takes a negative scale, since motion y grows downward.
    const i::Map pointer{{"look",
                          i::ActionType::vector2,
                          {{{i::ControlKind::mouse_motion, 0}, i::Channel::x, .5F},
                           {{i::ControlKind::mouse_motion, 1}, i::Channel::y, -.5F}}},
                         {"zoom", i::ActionType::axis, {{{i::ControlKind::mouse_wheel, 1}}}}};
    const auto pointed = [](const i::Context &context, float x, float y, float zoom) {
        const auto look = context.state("look").value;
        return look.x == x && look.y == y && context.state("zoom").value.x == zoom;
    };
    i::Context pointing(pointer);
    pointing.begin_frame();
    pointing.process({i::EventType::control, {i::ControlKind::mouse_motion, 0, 0}, 30});
    pointing.process({i::EventType::control, {i::ControlKind::mouse_motion, 1, 0}, 10});
    pointing.process({i::EventType::control, {i::ControlKind::mouse_motion, 0, 0}, 10});
    pointing.process({i::EventType::control, {i::ControlKind::mouse_wheel, 1, 0}, 2});
    if (!pointed(pointing, 20, -5, 2))
        throw std::runtime_error("Independent pointer actions did not sum and scale their increments");
    pointing.begin_frame();
    if (!pointed(pointing, 0, 0, 0) || !pointing.state("look").released)
        throw std::runtime_error("Independent pointer actions outlived their frame");
#ifdef CONSUMER_ASSETS
    // Documents persist a binding's device identity, which matches its gamepad under a new ID, and never an ID.
    i::Context reloaded(i::deserialize_map(i::serialize_map({{"jump", i::ActionType::button, {jump}}})));
    reloaded.process({i::EventType::control, {i::ControlKind::gamepad_button, 0, 11, pad}, 1});
    if (!reloaded.state("jump").active)
        throw std::runtime_error("Independent persisted identity did not match its gamepad");
    auto session_jump = jump;
    session_jump.control.device = 11;
    bool rejected = false;
    try {
        (void)i::serialize_map({{"jump", i::ActionType::button, {session_jump}}});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("Independent input configuration persisted a device ID");
    anima::SceneSet scenes;
    auto persistent = scenes.create("persistent"), level = scenes.create("level");
    auto object = persistent->create("independent action input");
    object.add_component<i::ActionInput>(i::deserialize_map(i::serialize_map(map)));
    anima::ComponentCodecs codecs;
    i::add_component_codec(codecs);
    auto restored = anima::Prefab::capture(object, codecs).instantiate(level.get());
    auto original_input = object.get_component<i::ActionInput>(),
         restored_input = restored.get_component<i::ActionInput>();
    i::begin_frame(scenes);
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::key, 44, 0}, 1});
    if (!original_input->context().state("interact").pressed || !restored_input->context().state("interact").pressed)
        throw std::runtime_error("Independent input scene set/prefab failed");
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::key, 22, 5}, 1});
    if (original_input->context().state("save").active || restored_input->context().state("save").active)
        throw std::runtime_error("Independent persisted chord lost its modifier requirement");
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::key, 224, 5}, 1});
    if (!original_input->context().state("save").pressed || !restored_input->context().state("save").pressed)
        throw std::runtime_error("Independent persisted chord failed across scene-set dispatch");
    restored.set_active(false);
    i::begin_frame(scenes);
    if (restored_input->context().state("interact").active || !original_input->context().state("interact").active)
        throw std::runtime_error("Independent input scene activation leaked across scenes");
    restored.set_active(true);
    i::begin_frame(scenes);
    if (restored_input->context().state("interact").active)
        throw std::runtime_error("Independent input reactivation restored stale physical state");
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::key, 44, 0}, 0});
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::key, 44, 0}, 1});
    if (!restored_input->context().state("interact").pressed)
        throw std::runtime_error("Independent input reactivation lost fresh input");
    // Pointer bindings persist like any other, and a frame's increments reach every scene's copy.
    auto pointer_object = persistent->create("independent pointer input");
    pointer_object.add_component<i::ActionInput>(pointer);
    auto pointer_copy = anima::Prefab::capture(pointer_object, codecs).instantiate(level.get());
    const std::array pointer_inputs{pointer_object.get_component<i::ActionInput>(),
                                    pointer_copy.get_component<i::ActionInput>()};
    i::begin_frame(scenes);
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::mouse_motion, 0, 0}, 8});
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::mouse_motion, 1, 0}, -6});
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::mouse_wheel, 1, 0}, -1});
    for (const auto &input : pointer_inputs)
        if (!pointed(input->context(), 4, 3, -1))
            throw std::runtime_error("Independent restored pointer actions missed their increments");
    i::begin_frame(scenes);
    for (const auto &input : pointer_inputs)
        if (!pointed(input->context(), 0, 0, 0))
            throw std::runtime_error("Independent restored pointer actions outlived their frame");
    scenes.unload(level);
    i::begin_frame(scenes);
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::key, 44, 0}, 0});
    if (restored_input || !original_input->context().state("interact").released)
        throw std::runtime_error("Independent input scene unload retained an attachment");
#endif
#ifdef CONSUMER_INPUT_SDL
    // Stands in for SDL_GetGamepadGUIDForID, since this consumer links no SDL library: every gamepad reports the
    // GUID whose first byte is 3, the identity `jump` selects.
    const auto gamepad_guid = +[](std::uint32_t) {
        SDL_GUID guid{};
        guid.data[0] = 3;
        return guid;
    };
    SDL_Event event{};
    // Processes every event that `event` converts into, in order.
    const auto apply = [&event, gamepad_guid](i::Context &context) {
        for (const auto &converted : i::from_sdl(event, 1, gamepad_guid))
            context.process(converted);
    };
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = 1;
    event.key.scancode = SDL_SCANCODE_SPACE;
    actions.begin_frame();
    apply(actions);
    if (!actions.state("interact").pressed)
        throw std::runtime_error("Independent SDL input converter failed");
    actions.cancel();
    actions.begin_frame();
    event.key.which = 3;
    event.key.scancode = SDL_SCANCODE_S;
    apply(actions);
    if (actions.state("save").active)
        throw std::runtime_error("Independent SDL chord activated without its modifier");
    event.key.scancode = SDL_SCANCODE_LCTRL;
    apply(actions);
    if (!actions.state("save").pressed)
        throw std::runtime_error("Independent SDL Ctrl+S events failed to activate the chord");
    actions.begin_frame();
    event.type = SDL_EVENT_KEY_UP;
    apply(actions);
    if (!actions.state("save").released || actions.state("save").canceled)
        throw std::runtime_error("Independent SDL Ctrl release failed to release the chord");
    // SDL keeps one key state for all keyboards, so keyboard 4's Ctrl completes keyboard 3's chord, and removing
    // any keyboard releases every key, since SDL sends no key releases for it.
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.which = 4;
    apply(actions);
    if (!actions.state("save").active)
        throw std::runtime_error("Independent SDL keyboards did not share their key state");
    event = {};
    event.type = SDL_EVENT_KEYBOARD_REMOVED;
    event.kdevice.which = 4;
    apply(actions);
    if (actions.state("save").active || actions.state("save").canceled)
        throw std::runtime_error("Independent SDL keyboard removal did not release its keys");
    // Gamepad events carry the GUID SDL reports, so a binding by identity matches whatever instance ID SDL assigns.
    identified.process({i::EventType::disconnect, {i::ControlKind::gamepad_button, 0, 9}});
    identified.begin_frame();
    event = {};
    event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    event.gbutton.which = 21;
    event.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    event.gbutton.down = true;
    apply(identified);
    if (!identified.state("jump").pressed)
        throw std::runtime_error("Independent SDL gamepad GUID did not reach its binding by identity");
    // One SDL motion event carries both axes, and becomes one increment per axis on the global mouse, whatever
    // mouse relative mode reports.
    event = {};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = 1;
    event.motion.which = 2;
    event.motion.xrel = 30;
    event.motion.yrel = 10;
    if (i::from_sdl(event, 1, gamepad_guid).size() != 2u)
        throw std::runtime_error("Independent SDL motion did not convert per axis");
    pointing.begin_frame();
    apply(pointing);
    if (!pointed(pointing, 15, -5, 0))
        throw std::runtime_error("Independent SDL motion missed its pointer action");
#endif
}
