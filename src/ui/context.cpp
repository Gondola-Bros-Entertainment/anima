#include "ui_draw.hpp"
#include <RmlUi_Platform_SDL.h>
#include <algorithm>
#include <anima/ui/context.hpp>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stb_image.h>
#include <string_view>

namespace anima {
namespace {
constexpr std::size_t maximum_frame_vertices = 2'000'000;
constexpr int maximum_texture_dimension = 8192; // Matches the shared stb decoder's dimension bound.
constexpr std::streamoff maximum_encoded_texture_bytes = 64 * 1024 * 1024;
// Largest framebuffer pointer coordinate magnitude that still rounds into RmlUi's int coordinates.
constexpr double maximum_pointer_coordinate = std::numeric_limits<int>::max() - 1.0;
// RML controls that edit a value with keys, and the input types that only activate.
constexpr std::string_view textarea_tag = "textarea";
constexpr std::string_view select_tag = "select";
constexpr std::string_view input_tag = "input";
constexpr std::string_view input_type_attribute = "type";
constexpr std::string_view default_input_type = "text";
constexpr std::array<std::string_view, 4> activation_input_types{"button", "submit", "checkbox", "radio"};
bool ui_active{}; // RmlUi process globals; all access is on the UI/render thread.
std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
std::array<float, 16> identity_matrix() {
    std::array<float, 16> result{};
    result[0] = result[5] = result[10] = result[15] = 1;
    return result;
}
class Files final : public Rml::FileInterface {
  public:
    Rml::FileHandle Open(const Rml::String &path) override {
        auto stream = std::make_unique<std::ifstream>(
            std::filesystem::path(reinterpret_cast<const char8_t *>(path.c_str())), std::ios::binary);
        return *stream ? reinterpret_cast<Rml::FileHandle>(stream.release()) : 0;
    }
    void Close(Rml::FileHandle handle) override { delete reinterpret_cast<std::ifstream *>(handle); }
    size_t Read(void *buffer, size_t size, Rml::FileHandle handle) override {
        if (size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
            return 0;
        auto &stream = *reinterpret_cast<std::ifstream *>(handle);
        stream.read(static_cast<char *>(buffer), static_cast<std::streamsize>(size));
        return static_cast<size_t>(stream.gcount());
    }
    bool Seek(Rml::FileHandle handle, long offset, int origin) override {
        auto &stream = *reinterpret_cast<std::ifstream *>(handle);
        stream.clear();
        stream.seekg(offset, origin == SEEK_SET ? std::ios::beg : origin == SEEK_END ? std::ios::end : std::ios::cur);
        return bool(stream);
    }
    size_t Tell(Rml::FileHandle handle) override {
        auto &stream = *reinterpret_cast<std::ifstream *>(handle);
        stream.clear();
        const auto result = stream.tellg();
        return result < 0 ? 0 : static_cast<size_t>(result);
    }
};
class System final : public SystemInterface_SDL {
    SDL_Window *window;
    UiStats &stats;
    // Set when this interface turned SDL text input on. SDL keeps one text input state per window, so text
    // input that was already active belongs to the application, and only started input is stopped here.
    bool started_text_input{};

  public:
    float pixel_density = 1;
    bool focused = true;
    System(SDL_Window *value, UiStats &counters) : SystemInterface_SDL(value), window(value), stats(counters) {}
    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override {
        if (type == Rml::Log::LT_WARNING)
            ++stats.log_warnings;
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT)
            ++stats.log_errors;
        std::cerr << "[RmlUi] " << message << '\n';
        return true;
    }
    void ActivateKeyboard(Rml::Vector2f caret, float height) override {
        if (!focused)
            return;
        const SDL_Rect area{int(std::floor(caret.x / pixel_density)), int(std::floor(caret.y / pixel_density)), 1,
                            std::max(1, int(std::ceil(height / pixel_density)))};
        // Starting text input again would drop the properties it runs with, such as a password or number type that
        // the application chose, so only inactive text input is started here.
        const bool inactive = !SDL_TextInputActive(window);
        if (!SDL_SetTextInputArea(window, &area, 0) || (inactive && !SDL_StartTextInput(window))) {
            LogMessage(Rml::Log::LT_ERROR, std::string("SDL text input: ") + SDL_GetError());
            return;
        }
        started_text_input = started_text_input || inactive;
    }
    // RmlUi calls this when a text control loses focus, and the context whenever nothing captures the keyboard.
    void DeactivateKeyboard() override {
        if (!started_text_input)
            return;
        started_text_input = false;
        if (!SDL_StopTextInput(window))
            LogMessage(Rml::Log::LT_ERROR, std::string("SDL text input: ") + SDL_GetError());
    }
};
class Render final : public Rml::RenderInterface {
    std::map<Rml::CompiledGeometryHandle, std::vector<detail::UiVertex>> geometries;
    std::map<Rml::TextureHandle, std::shared_ptr<const detail::UiTexture>> textures;
    std::uintptr_t next_geometry = 1, next_texture = 1;
    std::array<float, 16> transform = identity_matrix();
    bool clipped{};
    std::array<int, 4> clip{};
    void unsupported(const char *feature) {
        if (failure.empty())
            failure = std::string("Unsupported RmlUi render feature: ") + feature;
    }

  public:
    detail::UiFrame frame;
    std::string failure;
    void begin(Rml::Vector2i dimensions) {
        frame.vertices.clear();
        frame.batches.clear();
        frame.width = dimensions.x;
        frame.height = dimensions.y;
        transform = identity_matrix();
        clipped = false;
    }
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        if (indices.empty() || indices.size() % 3 || indices.size() > maximum_frame_vertices) {
            unsupported("invalid or excessive triangle geometry");
            return 0;
        }
        std::vector<detail::UiVertex> expanded;
        expanded.reserve(indices.size());
        for (int index : indices) {
            if (index < 0 || static_cast<size_t>(index) >= vertices.size()) {
                unsupported("out-of-range geometry index");
                return 0;
            }
            const auto &v = vertices[static_cast<size_t>(index)];
            if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) || !std::isfinite(v.tex_coord.x) ||
                !std::isfinite(v.tex_coord.y)) {
                unsupported("non-finite geometry");
                return 0;
            }
            expanded.push_back({v.position.x,
                                v.position.y,
                                {v.colour.red, v.colour.green, v.colour.blue, v.colour.alpha},
                                v.tex_coord.x,
                                v.tex_coord.y});
        }
        const auto handle = next_geometry++;
        geometries.emplace(handle, std::move(expanded));
        return handle;
    }
    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        const auto &vertices = geometries.at(handle);
        if (vertices.size() > maximum_frame_vertices - frame.vertices.size() || !std::isfinite(translation.x) ||
            !std::isfinite(translation.y)) {
            unsupported("excessive or invalid frame geometry");
            return;
        }
        detail::UiBatch batch;
        batch.first = static_cast<std::uint32_t>(frame.vertices.size());
        batch.count = static_cast<std::uint32_t>(vertices.size());
        batch.transform = transform;
        batch.translation = {translation.x, translation.y};
        batch.clipped = clipped;
        batch.clip = clip;
        if (texture)
            batch.texture = textures.at(texture);
        frame.batches.push_back(std::move(batch));
        frame.vertices.insert(frame.vertices.end(), vertices.begin(), vertices.end());
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override { geometries.erase(handle); }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> bytes, Rml::Vector2i dimensions) override {
        if (dimensions.x <= 0 || dimensions.y <= 0 || dimensions.x > maximum_texture_dimension ||
            dimensions.y > maximum_texture_dimension ||
            bytes.size() != std::uint64_t(dimensions.x) * std::uint64_t(dimensions.y) * 4) {
            unsupported("invalid texture dimensions or bytes");
            return 0;
        }
        auto image = std::make_shared<detail::UiTexture>();
        image->width = static_cast<std::uint32_t>(dimensions.x);
        image->height = static_cast<std::uint32_t>(dimensions.y);
        image->rgba.assign(bytes.begin(), bytes.end());
        const auto handle = next_texture++;
        textures.emplace(handle, std::move(image));
        return handle;
    }
    // RmlUi handles a texture that cannot be loaded itself: it logs a warning, does not load that source again
    // and draws the element untextured. An image file is content, so its failure is reported the same way, with
    // the reason, and never latches a render failure.
    static Rml::TextureHandle unloadable(Rml::Vector2i &dimensions, const std::string &reason,
                                         const Rml::String &path) {
        dimensions = {};
        Rml::Log::Message(Rml::Log::LT_WARNING, "UI image %s: %s", reason.c_str(), path.c_str());
        return 0;
    }
    Rml::TextureHandle LoadTexture(Rml::Vector2i &dimensions, const Rml::String &path) override {
        std::ifstream file(std::filesystem::path(reinterpret_cast<const char8_t *>(path.c_str())),
                           std::ios::binary | std::ios::ate);
        if (!file)
            return unloadable(dimensions, "is missing or cannot be opened", path);
        const auto bytes = file.tellg();
        if (bytes <= 0 || bytes > maximum_encoded_texture_bytes)
            return unloadable(dimensions, "is empty or exceeds the encoded image size limit", path);
        std::vector<unsigned char> encoded(static_cast<size_t>(bytes));
        file.seekg(0);
        file.read(reinterpret_cast<char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        if (!file)
            return unloadable(dimensions, "could not be read", path);
        int channels = 0;
        std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels{
            stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &dimensions.x, &dimensions.y,
                                  &channels, 4),
            stbi_image_free};
        if (!pixels) {
            const char *reason = stbi_failure_reason();
            return unloadable(dimensions,
                              std::string("is not a PNG or JPEG that the decoder accepts (") +
                                  (reason ? reason : "no reason given") + ")",
                              path);
        }
        const auto count = size_t(dimensions.x) * size_t(dimensions.y) * 4;
        for (size_t i = 0; i < count; i += 4)
            for (size_t channel = 0; channel < 3; ++channel)
                pixels.get()[i + channel] =
                    static_cast<unsigned char>((unsigned(pixels.get()[i + channel]) * pixels.get()[i + 3] + 127) / 255);
        return GenerateTexture({pixels.get(), count}, dimensions);
    }
    void ReleaseTexture(Rml::TextureHandle handle) override { textures.erase(handle); }
    void EnableScissorRegion(bool enabled) override { clipped = enabled; }
    void SetScissorRegion(Rml::Rectanglei region) override {
        clip = {region.Left(), region.Top(), region.Right(), region.Bottom()};
    }
    void SetTransform(const Rml::Matrix4f *matrix) override {
        transform = identity_matrix();
        if (matrix)
            for (size_t c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) {
                    const float value = (*matrix)[c][r];
                    if (!std::isfinite(value)) {
                        unsupported("non-finite transform");
                        return;
                    }
                    transform[c * 4 + size_t(r)] = value;
                }
    }
    void EnableClipMask(bool enabled) override {
        if (enabled)
            unsupported("clip masks/rounded or transformed clipping");
    }
    void RenderToClipMask(Rml::ClipMaskOperation, Rml::CompiledGeometryHandle, Rml::Vector2f) override {
        unsupported("clip masks");
    }
    Rml::LayerHandle PushLayer() override {
        unsupported("render layers/filters/backdrop effects");
        return 0;
    }
    void CompositeLayers(Rml::LayerHandle, Rml::LayerHandle, Rml::BlendMode,
                         Rml::Span<const Rml::CompiledFilterHandle>) override {
        unsupported("layer compositing");
    }
    void PopLayer() override { unsupported("render layers"); }
    Rml::TextureHandle SaveLayerAsTexture() override {
        unsupported("layer textures");
        return 0;
    }
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override {
        unsupported("mask images");
        return 0;
    }
    Rml::CompiledFilterHandle CompileFilter(const Rml::String &, const Rml::Dictionary &) override {
        unsupported("filters");
        return 0;
    }
    void ReleaseFilter(Rml::CompiledFilterHandle) override {}
    Rml::CompiledShaderHandle CompileShader(const Rml::String &, const Rml::Dictionary &) override {
        unsupported("custom/gradient shaders");
        return 0;
    }
    void RenderShader(Rml::CompiledShaderHandle, Rml::CompiledGeometryHandle, Rml::Vector2f,
                      Rml::TextureHandle) override {
        unsupported("custom shaders");
    }
    void ReleaseShader(Rml::CompiledShaderHandle) override {}
};
int modifiers(SDL_Keymod value) {
    int result = 0;
    if (value & SDL_KMOD_CTRL)
        result |= Rml::Input::KM_CTRL;
    if (value & SDL_KMOD_SHIFT)
        result |= Rml::Input::KM_SHIFT;
    if (value & SDL_KMOD_ALT)
        result |= Rml::Input::KM_ALT;
    if (value & SDL_KMOD_GUI)
        result |= Rml::Input::KM_META;
    if (value & SDL_KMOD_CAPS)
        result |= Rml::Input::KM_CAPSLOCK;
    if (value & SDL_KMOD_NUM)
        result |= Rml::Input::KM_NUMLOCK;
    return result;
}
} // namespace

struct UiContext::Impl {
    SDL_Window *window;
    VulkanRenderer &renderer;
    std::string name;
    UiStats stats;
    System system;
    Files files;
    Render render;
    TextInputMethodEditor_SDL ime;
    Rml::Context *context{};
    std::unique_ptr<UiDocuments> documents;
    bool initialized{}, stopped{};
    std::set<Rml::Input::KeyIdentifier> keys;
    std::set<int> buttons;
    std::set<int> world_buttons;
    std::set<int> cancelled_buttons;
    std::set<SDL_Scancode> gameplay_keys; // Pressed while unconsumed; their releases stay with gameplay.
    Rml::ObserverPtr<Rml::Element> pointer_owner;
    // Set when this context requested the SDL mouse capture that holds a document press. SDL keeps one capture
    // request for all windows, so a capture already in effect belongs to whoever took it, the application or
    // SDL's own capture while buttons are held, and only a capture requested here is released here.
    bool captured_mouse{};
    Impl(SDL_Window *w, VulkanRenderer &r, std::string n)
        : window(w), renderer(r), name(std::move(n)), system(w, stats) {}
    ~Impl() {
        // Destroying the host first shuts it down, or terminates when this runs inside one of its callbacks,
        // before RmlUi is shut down beneath the dispatch.
        documents.reset();
        shutdown();
    }
    void running() const {
        if (stopped)
            throw std::logic_error("UI context is shut down");
    }
    void capture_mouse() {
        if (captured_mouse || (SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_CAPTURE))
            return;
        captured_mouse = SDL_CaptureMouse(true);
    }
    void release_mouse() {
        if (!captured_mouse)
            return;
        captured_mouse = false;
        (void)SDL_CaptureMouse(false);
    }
    void initialize() {
        ui_active = true;
        Rml::SetSystemInterface(&system);
        Rml::SetFileInterface(&files);
        Rml::SetRenderInterface(&render);
        if (!Rml::Initialise())
            throw std::runtime_error("RmlUi initialization failed");
        initialized = true;
        context = Rml::CreateContext(name, {1, 1}, &render, &ime);
        if (!context)
            throw std::runtime_error("RmlUi context creation failed");
        system.focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        metrics();
    }
    void metrics() {
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height))
            throw std::runtime_error(SDL_GetError());
        const float density = SDL_GetWindowPixelDensity(window), scale = SDL_GetWindowDisplayScale(window);
        if (!std::isfinite(density) || density <= 0 || !std::isfinite(scale) || scale <= 0)
            throw std::runtime_error("Invalid SDL UI display metrics");
        system.pixel_density = density;
        context->SetDimensions({std::max(1, width), std::max(1, height)});
        context->SetDensityIndependentPixelRatio(scale);
    }
    // Controls that edit a value with keys capture the keyboard. Buttons, checkboxes, radios and
    // other elements keep focus for activation and navigation without taking gameplay keys.
    static bool edits_with_keyboard(const Rml::Element &element) {
        const auto &tag = element.GetTagName();
        if (tag == textarea_tag || tag == select_tag)
            return true;
        if (tag != input_tag)
            return false;
        const auto type =
            element.GetAttribute<Rml::String>(Rml::String(input_type_attribute), Rml::String(default_input_type));
        return std::ranges::find(activation_input_types, std::string_view(type)) == activation_input_types.end();
    }
    bool keyboard_capture() const {
        auto *focus = context->GetFocusElement();
        return system.focused && focus && focus->IsVisible(true) && focus->GetOwnerDocument() &&
               focus != focus->GetOwnerDocument() && edits_with_keyboard(*focus);
    }
    void synchronize_focus() {
        if (!buttons.empty() && (!pointer_owner || !pointer_owner->IsVisible(true))) {
            (void)context->ProcessMouseLeave();
            for (int button : buttons) {
                (void)context->ProcessMouseButtonUp(button, 0);
                cancelled_buttons.insert(button);
            }
            buttons.clear();
            pointer_owner.reset();
            release_mouse();
        }
        if (auto *focus = context->GetFocusElement(); focus && !focus->IsVisible(true))
            focus->Blur();
        if (!keyboard_capture())
            system.DeactivateKeyboard();
    }
    UiInputResult input_state() const {
        running();
        UiInputResult result;
        result.pointer =
            system.focused && (!buttons.empty() || (world_buttons.empty() && context->IsMouseInteracting()));
        result.keyboard = keyboard_capture();
        result.text = result.keyboard && SDL_TextInputActive(window);
        return result;
    }
    void focus_lost() {
        system.focused = false;
        (void)context->ProcessMouseLeave();
        for (auto button : buttons)
            (void)context->ProcessMouseButtonUp(button, 0);
        for (auto button : world_buttons)
            (void)context->ProcessMouseButtonUp(button, 0);
        for (auto key : keys)
            (void)context->ProcessKeyUp(key, 0);
        buttons.clear();
        world_buttons.clear();
        cancelled_buttons.clear();
        pointer_owner.reset();
        keys.clear();
        gameplay_keys.clear();
        if (auto *focus = context->GetFocusElement())
            focus->Blur();
        system.DeactivateKeyboard();
        release_mouse();
    }
    // Framebuffer pixels for SDL window coordinates, checked and rounded from the same scaled value.
    Rml::Vector2i framebuffer_position(float x, float y) const {
        const double scaled_x = double(x) * system.pixel_density, scaled_y = double(y) * system.pixel_density;
        if (!std::isfinite(scaled_x) || !std::isfinite(scaled_y) || std::abs(scaled_x) > maximum_pointer_coordinate ||
            std::abs(scaled_y) > maximum_pointer_coordinate)
            throw std::invalid_argument("Invalid UI pointer coordinates");
        return {int(std::lround(scaled_x)), int(std::lround(scaled_y))};
    }
    UiInputResult event(const SDL_Event &event) {
        running();
        const auto *target = SDL_GetWindowFromEvent(&event);
        if (target && target != window)
            return input_state();
        // Every pointer payload is checked before anything below changes state, whatever the focus or held
        // presses, so a rejected event changes nothing.
        Rml::Vector2i position;
        if (event.type == SDL_EVENT_MOUSE_MOTION)
            position = framebuffer_position(event.motion.x, event.motion.y);
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            position = framebuffer_position(event.button.x, event.button.y);
        else if (event.type == SDL_EVENT_MOUSE_WHEEL &&
                 (!std::isfinite(event.wheel.x) || !std::isfinite(event.wheel.y)))
            throw std::invalid_argument("Invalid UI wheel delta");
        synchronize_focus();
        const auto before = input_state();
        const bool owned_pointer = !buttons.empty();
        bool world_pointer = !world_buttons.empty();
        bool propagate = true, pointer_event = false, keyboard_event = false, lost = false;
        const auto mods = modifiers(SDL_GetModState());
        const auto move = [&] { return context->ProcessMouseMove(position.x, position.y, mods); };
        switch (event.type) {
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            focus_lost();
            lost = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            system.focused = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_RESTORED:
            metrics();
            renderer.request_resize();
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            propagate = context->ProcessMouseLeave();
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (system.focused) {
                pointer_event = true;
                if (world_pointer)
                    (void)context->ProcessMouseLeave();
                else
                    propagate = move();
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (system.focused) {
                pointer_event = true;
                const int button = RmlSDL::ConvertMouseButton(event.button.button);
                if (button < 0)
                    break;
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && cancelled_buttons.erase(button)) {
                    propagate = false;
                    break;
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                    cancelled_buttons.erase(button);
                if (world_pointer) {
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                        world_buttons.insert(button);
                    else {
                        world_buttons.erase(button);
                        (void)context->ProcessMouseLeave();
                        (void)context->ProcessMouseButtonUp(button, mods);
                        if (world_buttons.empty())
                            (void)move();
                    }
                    break;
                }
                (void)move();
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    auto *hover = context->GetHoverElement();
                    // Text controls replace anonymous children while editing. Own
                    // the screen's lifetime, not an ephemeral hit-test child.
                    auto *document = hover ? hover->GetOwnerDocument() : nullptr;
                    auto owner = document ? document->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>{};
                    propagate = context->ProcessMouseButtonDown(button, mods);
                    if (!propagate || owned_pointer) {
                        if (!owned_pointer)
                            pointer_owner = std::move(owner);
                        buttons.insert(button);
                        capture_mouse();
                    } else {
                        world_buttons.insert(button);
                        world_pointer = true;
                        // RmlUi keeps focus when a click misses every document; leave the control.
                        if (auto *focus = context->GetFocusElement(); focus && focus != focus->GetOwnerDocument())
                            focus->Blur();
                    }
                } else {
                    propagate = context->ProcessMouseButtonUp(button, mods);
                    buttons.erase(button);
                    if (owned_pointer && buttons.empty()) {
                        pointer_owner.reset();
                        release_mouse();
                    }
                }
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (system.focused) {
                pointer_event = true;
                if (world_pointer)
                    break;
                // SDL's x is positive to the right and its y positive for scrolling up; RmlUi scrolls right and
                // down for positive values, so only y flips. The values already follow the platform's natural
                // scrolling: the SDL backends that report SDL_MOUSEWHEEL_FLIPPED only record it there.
                propagate = context->ProcessMouseWheel({event.wheel.x, -event.wheel.y}, mods);
            }
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (system.focused) {
                keyboard_event = true;
                const auto key = RmlSDL::ConvertKey(static_cast<int>(event.key.key));
                if (event.type == SDL_EVENT_KEY_DOWN) {
                    keys.insert(key);
                    propagate = context->ProcessKeyDown(key, modifiers(event.key.mod));
                    if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
                        propagate = context->ProcessTextInput('\n') && propagate;
                } else {
                    keys.erase(key);
                    propagate = context->ProcessKeyUp(key, modifiers(event.key.mod));
                }
            }
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (before.text && event.text.text) {
                keyboard_event = true;
                propagate = context->ProcessTextInput(Rml::String(event.text.text));
            }
            break;
        case SDL_EVENT_TEXT_EDITING:
            if (before.text && event.edit.text) {
                keyboard_event = true;
                ime.HandleEdit(event.edit);
                propagate = false;
            }
            break;
        default:
            break;
        }
        synchronize_focus();
        auto result = input_state();
        result.consumed =
            !(pointer_event && world_pointer) && (!propagate || (pointer_event && (owned_pointer || result.pointer)) ||
                                                  (keyboard_event && (before.keyboard || result.keyboard)));
        if (keyboard_event && event.key.scancode != SDL_SCANCODE_UNKNOWN &&
            (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)) {
            // Like a pointer press, a key keeps its starting ownership: a release whose press reached
            // gameplay stays with gameplay even if a control took keyboard focus in between.
            if (event.type == SDL_EVENT_KEY_DOWN && !result.consumed)
                gameplay_keys.insert(event.key.scancode);
            else if (event.type == SDL_EVENT_KEY_UP && gameplay_keys.erase(event.key.scancode))
                result.consumed = false;
        }
        result.focus_lost = lost;
        return result;
    }
    void shutdown() {
        if (documents)
            documents->shutdown();
        if (stopped)
            return;
        stopped = true;
        system.DeactivateKeyboard();
        release_mouse();
        pointer_owner.reset(); // Observer pool belongs to RmlUi; release before Shutdown.
        if (initialized)
            Rml::Shutdown();
        context = nullptr;
        render.frame = {};
        Rml::SetRenderInterface(nullptr);
        Rml::SetFileInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
        ui_active = false;
    }
};
UiContext::UiContext(SDL_Window *window, VulkanRenderer &renderer, std::string name) {
    if (!window || name.empty())
        throw std::invalid_argument("UI needs a window and nonempty context name");
    if (ui_active)
        throw std::logic_error("Only one live Anima UI context is supported");
    // Rejected once here, before RmlUi starts, rather than by every render().
    if (!renderer.srgb_presentation())
        throw std::runtime_error(detail::srgb_presentation_required);
    impl_ = std::make_unique<Impl>(window, renderer, std::move(name));
    impl_->initialize();
    impl_->documents = std::make_unique<UiDocuments>(*impl_->context);
}
UiContext::~UiContext() = default;
Rml::Context &UiContext::context() {
    impl_->running();
    return *impl_->context;
}
UiDocuments &UiContext::documents() {
    impl_->running();
    return *impl_->documents;
}
UiDocument UiContext::open_document(const std::filesystem::path &path) { return documents().load(path); }
void UiContext::load_font(const std::filesystem::path &path, bool fallback) {
    impl_->running();
    if (!Rml::LoadFontFace(utf8(path), fallback))
        throw std::runtime_error("Unable to load UI font: " + utf8(path));
}
UiInputResult UiContext::process_event(const SDL_Event &event) {
    auto result = impl_->event(event);
    impl_->documents->check_events();
    return result;
}
UiInputResult UiContext::input_state() const { return impl_->input_state(); }
void UiContext::update() {
    impl_->running();
    impl_->metrics();
    impl_->synchronize_focus();
    impl_->context->Update();
    impl_->documents->check_events();
    impl_->synchronize_focus();
    ++impl_->stats.updates;
}
bool UiContext::render() {
    impl_->running();
    if (!impl_->render.failure.empty())
        throw UiUnsupportedFeature(impl_->render.failure);
    impl_->render.begin(impl_->context->GetDimensions());
    impl_->context->Render();
    if (!impl_->render.failure.empty())
        throw UiUnsupportedFeature(impl_->render.failure);
    // The renderer's own count decides, so a frame presented by a call that then throws is counted too.
    const auto presented_before = impl_->renderer.presented_frames();
    const auto count_presented = [&] {
        impl_->stats.rendered_frames += impl_->renderer.presented_frames() - presented_before;
    };
    bool presented = false;
    try {
        presented = impl_->renderer.draw_ui(impl_->render.frame);
    } catch (...) {
        count_presented();
        throw;
    }
    count_presented();
    return presented;
}
UiStats UiContext::stats() const { return impl_->stats; }
void UiContext::shutdown() { impl_->shutdown(); }
} // namespace anima
