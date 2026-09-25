#pragma once
#include <anima/input.hpp>
#include <stdexcept>
#ifdef CONSUMER_ASSETS
#include <anima/input_scene.hpp>
#include <anima/prefab.hpp>
#endif
#ifdef CONSUMER_INPUT_SDL
#include <SDL3/SDL_events.h>
#include <anima/input_sdl.hpp>
#endif
inline void consume_input() {
    namespace i = anima::input;
    i::Map map{{"interact", i::ActionType::button, {{{i::ControlKind::key, 44}}}}};
    i::Context actions(map);
    actions.begin_frame();
    actions.process({i::EventType::control, {i::ControlKind::key, 44, 0}, 1});
    actions.process({i::EventType::control, {i::ControlKind::key, 44, 0}, 0});
    if (!actions.state("interact").pressed || !actions.state("interact").released)
        throw std::runtime_error("Independent input consumer lost a tap");
#ifdef CONSUMER_ASSETS
    anima::Scene scene;
    auto object = scene.create("independent action input");
    object.add_component<i::ActionInput>(i::deserialize_map(i::serialize_map(map)));
    anima::ComponentCodecs codecs;
    i::add_component_codec(codecs);
    auto restored = anima::Prefab::capture(object, codecs).instantiate(scene);
    i::begin_frame(scene);
    i::dispatch(scene, {i::EventType::control, {i::ControlKind::key, 44, 0}, 1});
    if (!restored.get_component<i::ActionInput>()->context().state("interact").pressed)
        throw std::runtime_error("Independent input scene/prefab failed");
#endif
#ifdef CONSUMER_INPUT_SDL
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = 1;
    event.key.scancode = SDL_SCANCODE_SPACE;
    actions.begin_frame();
    actions.process(*i::from_sdl(event, 1));
    if (!actions.state("interact").pressed)
        throw std::runtime_error("Independent SDL input converter failed");
#endif
}
