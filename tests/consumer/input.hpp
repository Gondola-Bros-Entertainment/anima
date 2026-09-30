#pragma once
#include <anima/input.hpp>
#include <stdexcept>
#ifdef CONSUMER_ASSETS
#include "rejection.hpp"
#include <anima/input_scene.hpp>
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
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
#ifdef CONSUMER_ASSETS
    // Documents persist a binding's device identity, which matches its gamepad under a new ID, and never an ID.
    i::Context reloaded(i::deserialize_map(i::serialize_map({{"jump", i::ActionType::button, {jump}}})));
    reloaded.process({i::EventType::control, {i::ControlKind::gamepad_button, 0, 11, pad}, 1});
    if (!reloaded.state("jump").active)
        throw std::runtime_error("Independent persisted identity did not match its gamepad");
    auto session_jump = jump;
    session_jump.control.device = 11;
    rejection::rejects<std::invalid_argument>(
        [&] { (void)i::serialize_map({{"jump", i::ActionType::button, {session_jump}}}); },
        "Input configuration cannot persist a device ID");
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
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = 1;
    event.key.scancode = SDL_SCANCODE_SPACE;
    actions.begin_frame();
    actions.process(*i::from_sdl(event, 1, gamepad_guid));
    if (!actions.state("interact").pressed)
        throw std::runtime_error("Independent SDL input converter failed");
    actions.cancel();
    actions.begin_frame();
    event.key.which = 3;
    event.key.scancode = SDL_SCANCODE_S;
    actions.process(*i::from_sdl(event, 1, gamepad_guid));
    if (actions.state("save").active)
        throw std::runtime_error("Independent SDL chord activated without its modifier");
    event.key.scancode = SDL_SCANCODE_LCTRL;
    actions.process(*i::from_sdl(event, 1, gamepad_guid));
    if (!actions.state("save").pressed)
        throw std::runtime_error("Independent SDL Ctrl+S events failed to activate the chord");
    actions.begin_frame();
    event.type = SDL_EVENT_KEY_UP;
    actions.process(*i::from_sdl(event, 1, gamepad_guid));
    if (!actions.state("save").released || actions.state("save").canceled)
        throw std::runtime_error("Independent SDL Ctrl release failed to release the chord");
    // SDL keeps one key state for all keyboards, so keyboard 4's Ctrl completes keyboard 3's chord, and removing
    // any keyboard releases every key, since SDL sends no key releases for it.
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.which = 4;
    actions.process(*i::from_sdl(event, 1, gamepad_guid));
    if (!actions.state("save").active)
        throw std::runtime_error("Independent SDL keyboards did not share their key state");
    event = {};
    event.type = SDL_EVENT_KEYBOARD_REMOVED;
    event.kdevice.which = 4;
    actions.process(*i::from_sdl(event, 1, gamepad_guid));
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
    identified.process(*i::from_sdl(event, 1, gamepad_guid));
    if (!identified.state("jump").pressed)
        throw std::runtime_error("Independent SDL gamepad GUID did not reach its binding by identity");
#endif
}
