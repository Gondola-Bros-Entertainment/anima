// Custom materials without a GPU: SPIR-V validation against the documented interface, assignment to scene objects,
// and persistence by name in scene, prefab, prefab variant and scene set documents, staged ones included. The
// modules are assembled here word by word, so each test states exactly the interface it checks and no shader
// compiler is needed.
#include <anima/custom_material.hpp>
#include <anima/prefab.hpp>
#include <anima/prefab_variant.hpp>
#include <anima/scene.hpp>
#include <anima/scene_set.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace anima;
namespace {
// SPIR-V opcodes and enumerants that the assembled modules use (SPIR-V specification, unified1).
namespace op {
constexpr std::uint32_t ext_inst_import = 11, memory_model = 14, entry_point = 15, execution_mode = 16, capability = 17,
                        type_void = 19, type_int = 21, type_float = 22, type_vector = 23, type_matrix = 24,
                        type_image = 25, type_sampled_image = 27, type_runtime_array = 29, type_struct = 30,
                        type_pointer = 32, type_function = 33, function = 54, function_end = 56, variable = 59,
                        decorate = 71, member_decorate = 72, label = 248, return_ = 253;
}
constexpr std::uint32_t uniform_constant = 0, input = 1, uniform = 2, output = 3, push_constant = 9,
                        storage_buffer = 12;
constexpr std::uint32_t block = 2, col_major = 5, array_stride = 6, matrix_stride = 7, non_writable = 24, location = 30,
                        binding = 33, descriptor_set = 34, offset = 35;
constexpr std::uint32_t vertex_model = 0, fragment_model = 4;

// Assembles one shader module: an entry point `main` that returns at once, with the declarations that the test
// adds. Input and output variables join the entry point's interface.
class Module {
  public:
    explicit Module(std::uint32_t model) : model_(model) {
        void_ = type(op::type_void, {});
        function_type_ = type(op::type_function, {void_});
        float_ = type(op::type_float, {32});
        uint_ = type(op::type_int, {32, 0});
        main_ = id();
    }
    std::uint32_t id() { return bound_++; }
    // A type or constant defined by @p opcode with @p operands after its result id.
    std::uint32_t type(std::uint32_t opcode, std::vector<std::uint32_t> operands) {
        const auto result = id();
        operands.insert(operands.begin(), result);
        emit(types_, opcode, operands);
        return result;
    }
    std::uint32_t vector(std::uint32_t count, bool unsigned_integer = false) {
        return type(op::type_vector, {unsigned_integer ? uint_ : float_, count});
    }
    std::uint32_t mat4() { return type(op::type_matrix, {vector(4), 4}); }
    std::uint32_t scalar() const { return float_; }
    std::uint32_t unsigned_scalar() const { return uint_; }
    // A variable of type @p pointee in @p storage.
    std::uint32_t variable(std::uint32_t storage, std::uint32_t pointee) {
        const auto pointer = type(op::type_pointer, {storage, pointee});
        const auto result = id();
        emit(types_, op::variable, {pointer, result, storage});
        if (storage == input || storage == output)
            interface_.push_back(result);
        return result;
    }
    void decorate(std::uint32_t target, std::vector<std::uint32_t> decoration) {
        decoration.insert(decoration.begin(), target);
        emit(decorations_, op::decorate, decoration);
    }
    void decorate_member(std::uint32_t target, std::uint32_t member, std::vector<std::uint32_t> decoration) {
        decoration.insert(decoration.begin(), {target, member});
        emit(decorations_, op::member_decorate, decoration);
    }
    std::uint32_t input_at(std::uint32_t where, std::uint32_t type_id) {
        const auto result = variable(input, type_id);
        decorate(result, {location, where});
        return result;
    }
    std::uint32_t output_at(std::uint32_t where, std::uint32_t type_id) {
        const auto result = variable(output, type_id);
        decorate(result, {location, where});
        return result;
    }
    // A block of @p members, each a type and an offset, laid out column-major.
    std::uint32_t structure(const std::vector<std::pair<std::uint32_t, std::uint32_t>> &members,
                            const std::vector<std::uint32_t> &matrices = {}) {
        std::vector<std::uint32_t> types;
        for (const auto &member : members)
            types.push_back(member.first);
        const auto result = type(op::type_struct, types);
        decorate(result, {block});
        for (std::uint32_t i = 0; i < members.size(); ++i)
            decorate_member(result, i, {offset, members[i].second});
        for (const auto index : matrices) {
            decorate_member(result, index, {col_major});
            decorate_member(result, index, {matrix_stride, 16});
        }
        return result;
    }
    // A resource of type @p pointee in @p storage at @p set and @p where.
    std::uint32_t resource(std::uint32_t storage, std::uint32_t pointee, std::uint32_t set, std::uint32_t where) {
        const auto result = variable(storage, pointee);
        decorate(result, {descriptor_set, set});
        decorate(result, {binding, where});
        return result;
    }
    // A `sampler2D`, or with @p shadow a `sampler2DShadow`, at @p set and @p where.
    std::uint32_t sampler(std::uint32_t set, std::uint32_t where, bool shadow = false) {
        const auto image = type(op::type_image, {float_, 1, shadow ? 1U : 0U, 0, 0, 1, 0});
        return resource(uniform_constant, type(op::type_sampled_image, {image}), set, where);
    }
    // The pose buffer, readonly unless @p writable.
    std::uint32_t poses(bool writable = false) {
        const auto matrices = type(op::type_runtime_array, {mat4()});
        decorate(matrices, {array_stride, 64});
        const auto buffer = structure({{matrices, 0}}, {0});
        if (!writable)
            decorate_member(buffer, 0, {non_writable});
        return resource(storage_buffer, buffer, 1, 0);
    }
    void capability(std::uint32_t value) { extra_capabilities_.push_back(value); }
    void version(std::uint32_t value) { version_ = value; }
    [[nodiscard]] std::vector<std::uint32_t> words() const {
        std::vector<std::uint32_t> result{0x07230203, version_, 0, bound_, 0};
        emit(result, op::capability, {1});
        for (const auto value : extra_capabilities_)
            emit(result, op::capability, {value});
        std::vector<std::uint32_t> glsl{import_id_};
        append_string(glsl, "GLSL.std.450");
        emit(result, op::ext_inst_import, glsl);
        emit(result, op::memory_model, {0, 1});
        std::vector<std::uint32_t> entry{model_, main_};
        append_string(entry, "main");
        entry.insert(entry.end(), interface_.begin(), interface_.end());
        emit(result, op::entry_point, entry);
        if (model_ == fragment_model)
            emit(result, op::execution_mode, {main_, 7}); // OriginUpperLeft
        result.insert(result.end(), decorations_.begin(), decorations_.end());
        result.insert(result.end(), types_.begin(), types_.end());
        emit(result, op::function, {void_, main_, 0, function_type_});
        emit(result, op::label, {label_});
        emit(result, op::return_, {});
        emit(result, op::function_end, {});
        return result;
    }

  private:
    static void emit(std::vector<std::uint32_t> &words, std::uint32_t opcode,
                     const std::vector<std::uint32_t> &operands) {
        words.push_back(static_cast<std::uint32_t>(operands.size() + 1) << 16 | opcode);
        words.insert(words.end(), operands.begin(), operands.end());
    }
    static void append_string(std::vector<std::uint32_t> &words, std::string_view text) {
        std::uint32_t word = 0;
        unsigned shift = 0;
        for (const char c : std::string(text) + '\0') {
            word |= std::uint32_t(static_cast<unsigned char>(c)) << shift;
            shift += 8;
            if (shift == 32) {
                words.push_back(word);
                word = 0;
                shift = 0;
            }
        }
        if (shift)
            words.push_back(word);
    }
    std::uint32_t model_, version_ = 0x00010300, bound_ = 1;
    std::uint32_t import_id_ = bound_++, label_ = bound_++;
    std::uint32_t void_{}, function_type_{}, float_{}, uint_{}, main_{};
    std::vector<std::uint32_t> extra_capabilities_, interface_, decorations_, types_;
};

// A vertex shader that reads the position and writes a vec2 at location 0.
Module vertex_module() {
    Module shader(vertex_model);
    (void)shader.input_at(0, shader.vector(3));
    (void)shader.output_at(0, shader.vector(2));
    return shader;
}
// A fragment shader that reads a vec2 at location 0 and writes a color.
Module fragment_module() {
    Module shader(fragment_model);
    (void)shader.input_at(0, shader.vector(2));
    (void)shader.output_at(0, shader.vector(4));
    return shader;
}
CustomMaterialDefinition definition(const Module &vertex = vertex_module(),
                                    const Module &fragment = fragment_module()) {
    CustomMaterialDefinition result;
    result.name = "surface";
    result.vertex_shader = vertex.words();
    result.fragment_shader = fragment.words();
    return result;
}
CustomMaterialDefinition blended_definition(const Module &fragment) {
    auto result = definition(vertex_module(), fragment);
    result.blend = CustomBlend::blended;
    return result;
}
Texture texture() { return {std::make_shared<Image>(Image{1, 1, {255, 255, 255, 255}}), {}, TextureEncoding::srgb}; }
void rejects(const CustomMaterialDefinition &value, const std::string &message) {
    CHECK_THROWS_WITH_AS((void)CustomMaterial(value), message.c_str(), std::invalid_argument);
}
// The bytes of @p words as stored in host byte order, or with each word's bytes reversed.
std::vector<std::byte> module_bytes(const std::vector<std::uint32_t> &words, bool swapped = false) {
    std::vector<std::byte> bytes(words.size() * sizeof(std::uint32_t));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    if (swapped)
        for (auto word = bytes.begin(); word != bytes.end(); word += sizeof(std::uint32_t))
            std::reverse(word, word + sizeof(std::uint32_t));
    return bytes;
}
// A file in the temporary directory, named uniquely within the process and removed on destruction.
struct TempFile {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("anima-custom-material-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(count++) + ".spv");
    static inline std::atomic<unsigned> count;
    explicit TempFile(const std::vector<std::byte> &bytes) {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;
    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};
} // namespace

TEST_CASE("A custom material records the interface that its shaders declare") {
    auto vertex = vertex_module();
    (void)vertex.input_at(4, vertex.vector(4, true));
    (void)vertex.poses();
    const auto mat4 = vertex.mat4();
    (void)vertex.resource(uniform, vertex.structure({{mat4, 0}, {vertex.scalar(), 288}}, {0}), 0, 0);
    const auto draw =
        vertex.structure({{vertex.mat4(), 0}, {vertex.unsigned_scalar(), 68}, {vertex.vector(4), 80}}, {0});
    (void)vertex.variable(push_constant, draw);
    auto fragment = fragment_module();
    (void)fragment.sampler(0, 1);
    (void)fragment.sampler(0, 2);
    (void)fragment.resource(uniform, fragment.structure({{fragment.vector(4), 0}, {fragment.vector(3), 16}}), 2, 0);
    (void)fragment.sampler(2, 1);
    auto shadow = vertex_module();
    (void)shadow.input_at(7, shadow.scalar());
    auto value = blended_definition(fragment);
    value.vertex_shader = vertex.words();
    value.shadow_vertex_shader = shadow.words();
    value.parameters.resize(28);
    value.textures = {texture()};
    const CustomMaterial custom(value);
    CHECK(custom.name() == "surface");
    CHECK(custom.blend() == CustomBlend::blended);
    CHECK(custom.vertex_attributes() == 0b10001U);
    CHECK(custom.shadow_vertex_attributes() == 0b10000001U);
    CHECK(custom.casts_shadows());
    CHECK(custom.reads_opaque_depth());
    CHECK(custom.reads_opaque_color());
    CHECK_FALSE(custom.reads_placements());
    const CustomMaterial plain(definition());
    CHECK_FALSE(plain.casts_shadows());
    CHECK_FALSE(plain.reads_opaque_depth());
    CHECK_FALSE(plain.reads_opaque_color());
    CHECK_FALSE(plain.reads_placements());

    // Drawing placements takes all three rows in every vertex shader the material has.
    const auto placed = [](std::initializer_list<std::uint32_t> rows) {
        auto module = vertex_module();
        for (const auto row : rows)
            (void)module.input_at(row, module.vector(4));
        return module.words();
    };
    auto rows = definition();
    rows.vertex_shader = placed({8, 9, 10});
    CHECK(CustomMaterial(rows).vertex_attributes() == 0x701U);
    CHECK(CustomMaterial(rows).reads_placements());
    rows.vertex_shader = placed({8, 10});
    CHECK_FALSE(CustomMaterial(rows).reads_placements());
    rows.vertex_shader = placed({8, 9, 10});
    rows.shadow_vertex_shader = placed({8, 9});
    CHECK_FALSE(CustomMaterial(rows).reads_placements());
    rows.shadow_vertex_shader = placed({8, 9, 10});
    CHECK(CustomMaterial(rows).reads_placements());
}

TEST_CASE("Custom material definitions outside the documented limits are rejected") {
    auto value = definition();
    value.name.clear();
    rejects(value, "Custom material name must be 1 to 4096 bytes");
    value.name.assign(CustomMaterial::max_name_bytes + 1, 'n');
    rejects(value, "Custom material name must be 1 to 4096 bytes");
    value = definition();
    value.blend = static_cast<CustomBlend>(3);
    rejects(value, "Unknown custom material blend mode");
    value = definition();
    value.parameters.resize(CustomMaterial::max_parameter_bytes + 1);
    rejects(value, "Custom material parameters exceed 256 bytes");
    value.parameters.resize(CustomMaterial::max_parameter_bytes);
    CHECK_NOTHROW((void)CustomMaterial(value));
    value.textures.assign(CustomMaterial::max_textures + 1, texture());
    rejects(value, "Custom material has more than 4 textures");
    value.textures = {Texture{}};
    rejects(value, "Invalid scene texture dimensions");
    value.textures = {Texture{std::make_shared<Image>(Image{1, 1, {}}), {}}};
    rejects(value, "Texture image has no texels");
    value = definition();
    value.texel_retention = static_cast<TexelRetention>(2);
    rejects(value, "Unknown custom material texel retention");
    value = definition();
    value.fragment_shader.clear();
    rejects(value, "Custom material needs a vertex and a fragment shader");
    value = definition();
    value.shadow_fragment_shader = fragment_module().words();
    rejects(value, "Custom material has a shadow fragment shader without a shadow vertex shader");
}

TEST_CASE("A custom material compiled until upload holds its textures' images until it is released") {
    auto value = definition();
    value.textures = {texture(), texture()};
    value.textures.push_back(value.textures[0]); // A third texture sharing the first one's image.
    const std::weak_ptr<const Image> first = value.textures[0].image, second = value.textures[1].image;
    value.texel_retention = TexelRetention::until_upload;
    const auto material = std::make_shared<const CustomMaterial>(std::move(value));
    const auto &textures = material->definition().textures;
    REQUIRE(textures.size() == 3);
    CHECK(textures[0].image->rgba.empty());
    CHECK(textures[0].image->width == 1);
    CHECK(textures[2].image == textures[0].image);
    CHECK(textures[1].image != textures[0].image);
    {
        const auto images = material->texel_images();
        REQUIRE(images.size() == 3);
        CHECK(images[0] == first.lock());
        CHECK(images[1] == second.lock());
        CHECK(images[2] == first.lock());
    }
    material->release_texels();
    CHECK(first.expired());
    CHECK(second.expired());
    CHECK_THROWS_WITH_AS((void)material->texel_images(), "Custom material texture texels were released after upload",
                         std::logic_error);
    // With TexelRetention::keep the definition's textures hold the images, and releasing does nothing.
    auto kept = definition();
    kept.textures = {texture()};
    const std::weak_ptr<const Image> held = kept.textures[0].image;
    const CustomMaterial retained(std::move(kept));
    retained.release_texels();
    REQUIRE_FALSE(held.expired());
    CHECK(retained.texel_images().at(0) == held.lock());
    CHECK(retained.definition().textures[0].image == held.lock());
}

TEST_CASE("Words that are not a well-framed SPIR-V module are rejected") {
    auto value = definition();
    value.vertex_shader = {0xDEADBEEF, 0x00010300, 0, 1, 0};
    rejects(value, "The vertex shader is not SPIR-V: its first word is not the SPIR-V magic number");
    value.vertex_shader = {0x03022307, 0x00010300, 0, 1, 0};
    rejects(value, "The vertex shader is not SPIR-V in host byte order: its magic number is byte-swapped");
    value = definition();
    value.fragment_shader = {0x07230203, 0x00010300};
    rejects(value, "The fragment shader is not SPIR-V: it is shorter than the five-word module header");
    value = definition();
    const auto size = value.vertex_shader.size();
    value.vertex_shader.push_back(5U << 16 | 17U);
    rejects(value, "The vertex shader is invalid SPIR-V: the instruction at word " + std::to_string(size) +
                       " runs past the end of the module");
    value.vertex_shader.back() = 0;
    rejects(value, "The vertex shader is invalid SPIR-V: the instruction at word " + std::to_string(size) +
                       " has a word count of 0");
    auto later = vertex_module();
    later.version(0x00010500);
    value = definition(later);
    rejects(value, "The vertex shader is SPIR-V 1.5, but custom materials accept SPIR-V 1.0 to 1.3");
    auto doubles = fragment_module();
    doubles.capability(10);
    value = definition(vertex_module(), doubles);
    rejects(value, "The fragment shader requires SPIR-V capability 10, which custom materials do not support");
    value = definition(fragment_module(), fragment_module());
    rejects(value, "The vertex shader has no vertex entry point named main");
    // Types refer only to earlier types, so they cannot form a cycle, and nest a bounded depth.
    auto forward = vertex_module();
    const auto undefined = forward.id();
    const auto early = forward.type(op::type_vector, {undefined, 3});
    rejects(definition(forward), "The vertex shader is invalid SPIR-V: type " + std::to_string(early) +
                                     " refers to type " + std::to_string(undefined) + " before defining it");
    auto nested = vertex_module();
    auto innermost = nested.scalar();
    for (int level = 0; level < 64; ++level)
        innermost = nested.type(op::type_runtime_array, {innermost});
    rejects(definition(nested), "The vertex shader nests types more than 64 deep");
}

TEST_CASE("SPIR-V modules load from bytes and files in either byte order") {
    const auto vertex = vertex_module().words(), fragment = fragment_module().words();
    CHECK(spirv_words(module_bytes(vertex)) == vertex);
    CHECK(spirv_words(module_bytes(vertex, true)) == vertex);
    const TempFile vertex_file(module_bytes(vertex, true)), fragment_file(module_bytes(fragment));
    auto value = definition();
    value.vertex_shader = load_spirv(vertex_file.path);
    value.fragment_shader = load_spirv(fragment_file.path);
    CHECK(value.vertex_shader == vertex);
    CHECK(value.fragment_shader == fragment);
    CHECK_NOTHROW((void)CustomMaterial(value));
}

TEST_CASE("Bytes and files that cannot hold a SPIR-V module are rejected") {
    const auto rejects_bytes = [](const std::vector<std::byte> &bytes, const char *message) {
        CHECK_THROWS_WITH_AS((void)spirv_words(bytes), message, std::runtime_error);
    };
    const auto size = "SPIR-V module must be between 1 byte and 16 MiB";
    rejects_bytes({}, size);
    rejects_bytes(std::vector<std::byte>(CustomMaterial::max_shader_bytes + 4), size);
    auto bytes = module_bytes(vertex_module().words());
    bytes.pop_back();
    rejects_bytes(bytes, "SPIR-V module size must be a multiple of 4 bytes");
    rejects_bytes(module_bytes({0xDEADBEEF, 0x00010300, 0, 1, 0}),
                  "SPIR-V module does not start with the SPIR-V magic number");

    const TempFile absent({});
    std::filesystem::remove(absent.path);
    CHECK_THROWS_WITH_AS((void)load_spirv(absent.path), "Cannot open SPIR-V file", std::runtime_error);
    const TempFile empty({});
    CHECK_THROWS_WITH_AS((void)load_spirv(empty.path), size, std::runtime_error);
    const TempFile large(module_bytes(vertex_module().words()));
    std::filesystem::resize_file(large.path, CustomMaterial::max_shader_bytes + 4);
    CHECK_THROWS_WITH_AS((void)load_spirv(large.path), size, std::runtime_error);
    const TempFile odd(bytes);
    CHECK_THROWS_WITH_AS((void)load_spirv(odd.path), "SPIR-V module size must be a multiple of 4 bytes",
                         std::runtime_error);
}

TEST_CASE("Shader interfaces that differ from the documented one are rejected") {
    auto vertex = Module(vertex_model);
    (void)vertex.input_at(0, vertex.vector(4));
    (void)vertex.output_at(0, vertex.vector(2));
    rejects(definition(vertex),
            "The vertex shader reads vertex attribute 0 as vec4, but the custom material interface provides a vec3 "
            "there");
    vertex = vertex_module();
    (void)vertex.input_at(8, vertex.scalar());
    rejects(definition(vertex),
            "The vertex shader reads vertex attribute 8 as float, but the custom material interface provides a vec4 "
            "there");
    vertex = vertex_module();
    (void)vertex.input_at(11, vertex.vector(4));
    rejects(definition(vertex),
            "The vertex shader reads vertex attribute 11, which the custom material interface does not provide");

    auto fragment = fragment_module();
    (void)fragment.input_at(1, fragment.vector(3));
    rejects(definition(vertex_module(), fragment),
            "The fragment shader reads location 1 as vec3, which the vertex shader does not write");
    fragment = Module(fragment_model);
    (void)fragment.input_at(0, fragment.vector(3));
    (void)fragment.output_at(0, fragment.vector(4));
    rejects(definition(vertex_module(), fragment),
            "The fragment shader reads location 0 as vec3, but the vertex shader writes a vec2 there");
    fragment = Module(fragment_model);
    (void)fragment.output_at(0, fragment.vector(3));
    rejects(definition(vertex_module(), fragment),
            "The fragment shader writes location 0 as vec3, but custom materials write a vec4 there");
    fragment = Module(fragment_model);
    rejects(definition(vertex_module(), fragment), "The fragment shader writes no color at location 0");

    fragment = fragment_module();
    (void)fragment.sampler(3, 0);
    rejects(definition(vertex_module(), fragment),
            "The fragment shader declares set 3 binding 0, which the custom material interface does not provide");
    fragment = fragment_module();
    (void)fragment.sampler(2, 1, true);
    auto value = definition(vertex_module(), fragment);
    value.textures = {texture()};
    rejects(value, "The fragment shader declares set 2 binding 1 as sampler2DShadow, but the custom material "
                   "interface provides a sampler2D there");
    fragment = fragment_module();
    (void)fragment.sampler(2, 2);
    value = definition(vertex_module(), fragment);
    value.textures = {texture()};
    rejects(value, "The fragment shader declares texture 1 (set 2, binding 2), but the material supplies 1 texture");

    vertex = vertex_module();
    (void)vertex.resource(uniform, vertex.structure({{vertex.vector(4), 0}}), 0, 0);
    rejects(definition(vertex), "The vertex shader declares the frame block member at offset 0 as vec4, but the "
                                "custom material interface has a mat4 there");
    vertex = vertex_module();
    (void)vertex.resource(uniform, vertex.structure({{vertex.scalar(), 292}}), 0, 0);
    rejects(definition(vertex), "The vertex shader declares the frame block member at offset 292 as float, but the "
                                "custom material interface has no member there");
    // The fog's shape and sunlight follow the time, and nothing follows them.
    vertex = vertex_module();
    (void)vertex.resource(uniform, vertex.structure({{vertex.vector(4), 304}, {vertex.vector(4), 320}}), 0, 0);
    CHECK_NOTHROW((void)CustomMaterial(definition(vertex)));
    vertex = vertex_module();
    (void)vertex.resource(uniform, vertex.structure({{vertex.scalar(), 304}}), 0, 0);
    rejects(definition(vertex), "The vertex shader declares the frame block member at offset 304 as float, but the "
                                "custom material interface has a vec4 there");
    vertex = vertex_module();
    (void)vertex.resource(uniform, vertex.structure({{vertex.vector(4), 336}}), 0, 0);
    rejects(definition(vertex), "The vertex shader declares the frame block member at offset 336 as vec4, but the "
                                "custom material interface has no member there");
    vertex = vertex_module();
    (void)vertex.variable(push_constant, vertex.structure({{vertex.scalar(), 72}}));
    rejects(definition(vertex), "The vertex shader declares the draw push constant member at offset 72 as float, "
                                "but the custom material interface has a uint there");
    vertex = vertex_module();
    (void)vertex.variable(push_constant, vertex.structure({{vertex.scalar(), 88}}));
    rejects(definition(vertex), "The vertex shader declares the draw push constant member at offset 88 as float, "
                                "but the custom material interface has no member there");

    vertex = vertex_module();
    (void)vertex.poses(true);
    rejects(definition(vertex), "The vertex shader declares the pose buffer (set 1, binding 0) without readonly");
    fragment = fragment_module();
    (void)fragment.poses();
    rejects(definition(vertex_module(), fragment),
            "The fragment shader declares the pose buffer (set 1, binding 0), which only vertex shaders can read");
    vertex = vertex_module();
    (void)vertex.sampler(0, 1);
    value = blended_definition(fragment_module());
    value.vertex_shader = vertex.words();
    rejects(value, "The vertex shader declares opaque depth (set 0, binding 1), which only fragment shaders can read");
    fragment = fragment_module();
    (void)fragment.sampler(0, 1);
    rejects(definition(vertex_module(), fragment), "The fragment shader declares opaque depth (set 0, binding 1), but "
                                                   "only blended and additive custom materials can read it");
    auto shadow_fragment = Module(fragment_model);
    (void)shadow_fragment.sampler(0, 2);
    value = blended_definition(fragment_module());
    value.shadow_vertex_shader = Module(vertex_model).words();
    value.shadow_fragment_shader = shadow_fragment.words();
    rejects(value,
            "The shadow fragment shader declares opaque color (set 0, binding 2), which the depth-only variant cannot "
            "read");
    shadow_fragment = Module(fragment_model);
    (void)shadow_fragment.output_at(0, shadow_fragment.vector(4));
    value.shadow_fragment_shader = shadow_fragment.words();
    rejects(value, "The shadow fragment shader writes color at location 0, but the depth-only variant has no color "
                   "target");
}

TEST_CASE("A parameter block larger than the supplied parameters is rejected") {
    auto fragment = fragment_module();
    (void)fragment.resource(uniform, fragment.structure({{fragment.vector(4), 0}, {fragment.vector(4), 16}}), 2, 0);
    auto value = definition(vertex_module(), fragment);
    value.parameters.resize(16);
    rejects(value, "The fragment shader declares a parameter block of 32 bytes, but the material supplies 16");
    value.parameters.resize(32);
    CHECK_NOTHROW((void)CustomMaterial(value));
}

namespace {
// A mesh of one triangle and @p materials materials.
std::shared_ptr<const Mesh> mesh(std::size_t materials = 1) {
    Asset asset;
    asset.nodes.resize(1);
    asset.materials.resize(materials);
    SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto corner : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return Mesh::compile(asset);
}
std::shared_ptr<const CustomMaterial> material(std::string name) {
    auto value = definition();
    value.name = std::move(name);
    return std::make_shared<const CustomMaterial>(std::move(value));
}
// Resolves the one mesh, and custom materials by name from @p registered.
struct Library {
    std::shared_ptr<const Mesh> shape = mesh(2);
    std::map<std::string, std::shared_ptr<const CustomMaterial>, std::less<>> registered;
    [[nodiscard]] MeshName names() const {
        return [](const std::shared_ptr<const Mesh> &) { return std::string("shape"); };
    }
    [[nodiscard]] MeshResolver meshes() const {
        return [this](std::string_view) { return shape; };
    }
    [[nodiscard]] CustomMaterialResolver materials() const {
        return [this](std::string_view name) -> std::shared_ptr<const CustomMaterial> {
            const auto found = registered.find(name);
            return found == registered.end() ? nullptr : found->second;
        };
    }
};
} // namespace

TEST_CASE("Scene objects assign custom materials per mesh material") {
    Scene scene;
    const auto water = material("water");
    auto object = scene.create("pool", mesh(2));
    CHECK(scene.instance(object.id()).custom_materials == std::vector<std::shared_ptr<const CustomMaterial>>(2));
    object.renderer().set_custom_material(1, water);
    CHECK(scene.instance(object.id()).custom_materials[1] == water);
    CHECK_THROWS_WITH_AS(object.renderer().set_custom_material(2, water),
                         "Custom material slot is outside the mesh's materials", std::out_of_range);
    scene.set_custom_material(object.id(), 1, nullptr);
    CHECK_FALSE(scene.instance(object.id()).custom_materials[1]);
    object.renderer().set_custom_material(0, water);
    object.renderer().set_mesh(mesh(3));
    CHECK(scene.instance(object.id()).custom_materials == std::vector<std::shared_ptr<const CustomMaterial>>(3));
    auto empty = scene.create("empty");
    CHECK_THROWS_WITH_AS(scene.set_custom_material(empty.id(), 0, water), "GameObject has no MeshRenderer",
                         std::logic_error);
}

TEST_CASE("Only custom materials that read placements draw them") {
    // Water reads no placement rows; grass reads all three.
    const auto water = material("water");
    auto rows = vertex_module();
    for (const std::uint32_t row : {8U, 9U, 10U})
        (void)rows.input_at(row, rows.vector(4));
    auto value = definition(rows);
    value.name = "grass";
    const auto grass = std::make_shared<const CustomMaterial>(std::move(value));
    REQUIRE(grass->reads_placements());

    Scene scene;
    const auto shape = mesh(2);
    const auto placements = MeshPlacements::create(shape, std::vector<Mat4>{identity(), identity()});
    auto object = scene.create("meadow", shape);
    auto renderer = object.renderer();
    renderer.set_custom_material(0, water);
    CHECK_THROWS_WITH_AS(renderer.set_placements(placements), "A custom material that draws placements must read them",
                         std::invalid_argument);
    CHECK_FALSE(renderer.placements());
    renderer.set_custom_material(0, grass);
    renderer.set_placements(placements);
    CHECK_THROWS_WITH_AS(renderer.set_custom_material(1, water),
                         "A custom material that draws placements must read them", std::invalid_argument);
    CHECK_FALSE(scene.instance(object.id()).custom_materials[1]);
    renderer.set_custom_material(1, grass);
    renderer.set_custom_material(0, nullptr);
    CHECK(scene.instance(object.id()).placements == placements);
}

TEST_CASE("Documents store custom materials by name and resolve them through the application") {
    Library library;
    const auto water = material("water");
    library.registered.emplace("water", water);
    Scene scene;
    auto object = scene.create("pool", library.shape);
    object.renderer().set_custom_material(1, water);
    (void)scene.create("plain", library.shape);
    const auto document = serialize_scene(scene, library.names());
    CHECK(document.find(R"("custom_materials": [
        null,
        "water"
      ])") != std::string::npos);
    CHECK(document.find(R"("custom_materials": [])") != std::string::npos);
    const auto loaded = load_scene(document, library.meshes(), {}, library.materials());
    const auto pool = loaded->find(object.key());
    REQUIRE(pool.valid());
    CHECK(loaded->instance(pool.id()).custom_materials ==
          std::vector<std::shared_ptr<const CustomMaterial>>{nullptr, water});
    CHECK(serialize_scene(*loaded, library.names()) == document);

    // An omitted setting keeps the mesh's own materials.
    const auto omitted =
        load_scene(R"({"version":3,"kind":"anima.scene","next_key":"2","objects":[{"key":"1","name":"a","parent":null,)"
                   R"("local":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"mesh":"shape"}]})",
                   library.meshes());
    CHECK(omitted->instance(omitted->find(ObjectKey{1}).id()).custom_materials ==
          std::vector<std::shared_ptr<const CustomMaterial>>(2));

    const auto prefab = Prefab::capture(object);
    REQUIRE(prefab.nodes().front().renderer.custom_materials.size() == 2);
    const auto restored =
        Prefab::deserialize(prefab.serialize(library.names()), library.meshes(), {}, library.materials());
    CHECK(restored.nodes().front().renderer.custom_materials[1] == water);
    CHECK(Prefab::capture(scene.find(ObjectKey{2})).nodes().front().renderer.custom_materials.empty());

    PrefabVariant::Override change;
    change.key = ObjectKey{1};
    change.renderer.emplace();
    change.renderer->mesh = library.shape;
    change.renderer->custom_materials = {water, nullptr};
    const PrefabVariant variant("pool", {change});
    const auto variant_document = variant.serialize(library.names());
    const auto variant_loaded = PrefabVariant::deserialize(variant_document, library.meshes(), library.materials());
    CHECK(variant_loaded.overrides().front().renderer->custom_materials[0] == water);
    const auto resolved =
        variant_loaded.resolve([&](std::string_view) { return std::make_shared<const Prefab>(prefab); }, {});
    CHECK(resolved.nodes().front().renderer.custom_materials[0] == water);

    SceneSet set;
    (void)set.load("pool", document, library.meshes(), {}, library.materials());
    const auto set_document = set.serialize(library.names());
    SceneSet copy;
    copy.restore(set_document, library.meshes(), {}, library.materials());
    const auto member = copy.scenes().front();
    CHECK(member->instance(member->find(object.key()).id()).custom_materials[1] == water);
}

TEST_CASE("A document naming an unregistered custom material is rejected") {
    Library library;
    Scene scene;
    auto object = scene.create("pool", library.shape);
    object.renderer().set_custom_material(0, material("water"));
    const auto document = serialize_scene(scene, library.names());
    CHECK_THROWS_WITH_AS((void)load_scene(document, library.meshes(), {}, library.materials()),
                         "Scene custom material name could not be resolved", std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)load_scene(document, library.meshes()),
                         "Scene custom material name could not be resolved", std::invalid_argument);
    library.registered.emplace("water", material("lava"));
    CHECK_THROWS_WITH_AS((void)load_scene(document, library.meshes(), {}, library.materials()),
                         "Scene custom material resolved to a material of another name", std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)Prefab::deserialize(Prefab::capture(object).serialize(library.names()), library.meshes(),
                                                   {}, [](std::string_view) { return nullptr; }),
                         "Scene custom material name could not be resolved", std::invalid_argument);
    SceneSet set;
    CHECK_THROWS_WITH_AS((void)set.load("pool", document, library.meshes()),
                         "Scene custom material name could not be resolved", std::invalid_argument);
    CHECK(set.size() == 0);

    PrefabVariant::Override change;
    change.key = ObjectKey{1};
    change.renderer.emplace();
    change.renderer->mesh = library.shape;
    change.renderer->custom_materials = {material("water"), nullptr};
    const auto variant = PrefabVariant("pool", {change}).serialize(library.names());
    CHECK_THROWS_WITH_AS((void)PrefabVariant::deserialize(variant, library.meshes()),
                         "Prefab variant custom material name could not be resolved", std::invalid_argument);
}

TEST_CASE("Documents reject custom materials that do not match their names or meshes") {
    Library library;
    Scene scene;
    auto first = scene.create("first", library.shape);
    first.renderer().set_custom_material(0, material("water"));
    auto second = scene.create("second", library.shape);
    second.renderer().set_custom_material(0, material("water"));
    CHECK_THROWS_WITH_AS((void)serialize_scene(scene, library.names()),
                         "Different custom materials share a document name", std::invalid_argument);

    library.registered.emplace("water", material("water"));
    std::string document = R"({"version":3,"kind":"anima.scene","next_key":"2","objects":[{"key":"1","name":"a",)"
                           R"("parent":null,"local":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"mesh":"shape",)"
                           R"("custom_materials":["water"]}]})";
    CHECK_THROWS_WITH_AS((void)load_scene(document, library.meshes(), {}, library.materials()),
                         "Prefab custom materials do not match the mesh", std::invalid_argument);
    document = R"({"version":3,"kind":"anima.scene","next_key":"2","objects":[{"key":"1","name":"a",)"
               R"("parent":null,"local":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"mesh":"shape",)"
               R"("custom_materials":[null,""]}]})";
    CHECK_THROWS_WITH_AS((void)load_scene(document, library.meshes(), {}, library.materials()),
                         "Invalid scene custom material name", std::invalid_argument);
    document = R"({"version":3,"kind":"anima.scene","next_key":"2","objects":[{"key":"1","name":"a",)"
               R"("parent":null,"local":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"mesh":null,)"
               R"("custom_materials":[null]}]})";
    CHECK_THROWS_WITH_AS((void)load_scene(document, library.meshes(), {}, library.materials()),
                         "Empty scene object has renderer state", std::invalid_argument);
}

TEST_CASE("Staging resolves each custom material name once on the staging thread") {
    Library library;
    const auto water = material("water"), lava = material("lava");
    library.registered.emplace("water", water);
    library.registered.emplace("lava", lava);
    std::atomic<int> resolved{0};
    const CustomMaterialResolver counting = [&](std::string_view name) {
        ++resolved;
        return library.materials()(name);
    };
    Scene scene;
    auto pool = scene.create("pool", library.shape);
    pool.renderer().set_custom_material(0, lava);
    pool.renderer().set_custom_material(1, water);
    auto fountain = scene.create("fountain", library.shape);
    fountain.renderer().set_custom_material(1, water);
    const auto document = serialize_scene(scene, library.names());
    StagingProgress progress;
    const auto staged = std::async(std::launch::async, [&] {
                            return stage_scene(document, library.meshes(), counting, {{}, &progress});
                        }).get();
    // The parse, two objects, one mesh key and two custom material names.
    CHECK(resolved == 2);
    CHECK(progress.total() == 1 + 2 + 1 + 2);
    CHECK(progress.completed() == progress.total());
    const auto committed = load_scene(staged);
    CHECK(committed->instance(committed->find(pool.key()).id()).custom_materials ==
          std::vector<std::shared_ptr<const CustomMaterial>>{lava, water});
    CHECK(serialize_scene(*committed, library.names()) == document);
    CHECK(serialize_scene(*load_scene(document, library.meshes(), {}, library.materials()), library.names()) ==
          document);
    // Each object holds two custom material slots, and the materials themselves are shared.
    Scene plain;
    (void)plain.create("pool", library.shape);
    (void)plain.create("fountain", library.shape);
    const auto without = stage_scene(serialize_scene(plain, library.names()), library.meshes());
    CHECK(staged.retained_bytes() - without.retained_bytes() == 4 * sizeof(std::shared_ptr<const CustomMaterial>));

    SceneSet set;
    (void)set.load("pool", staged);
    const auto set_document = set.serialize(library.names());
    resolved = 0;
    const auto staged_set = stage_scene_set(set_document, library.meshes(), counting);
    CHECK(resolved == 2);
    SceneSet copy;
    copy.restore(staged_set);
    CHECK(copy.serialize(library.names()) == set_document);
}

TEST_CASE("A staged document naming a missing custom material is rejected as a synchronous load is") {
    Library library;
    Scene scene;
    auto object = scene.create("pool", library.shape);
    object.renderer().set_custom_material(0, material("water"));
    const auto document = serialize_scene(scene, library.names());
    const auto staged_elsewhere = [&](const CustomMaterialResolver &materials) {
        return std::async(std::launch::async, [&] { return stage_scene(document, library.meshes(), materials); });
    };
    constexpr auto unresolved = "Scene custom material name could not be resolved";
    CHECK_THROWS_WITH_AS((void)load_scene(document, library.meshes(), {}, library.materials()), unresolved,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)staged_elsewhere(library.materials()).get(), unresolved, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)load_scene(document, library.meshes()), unresolved, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)staged_elsewhere({}).get(), unresolved, std::invalid_argument);
    library.registered.emplace("water", material("lava"));
    constexpr auto renamed = "Scene custom material resolved to a material of another name";
    CHECK_THROWS_WITH_AS((void)load_scene(document, library.meshes(), {}, library.materials()), renamed,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)staged_elsewhere(library.materials()).get(), renamed, std::invalid_argument);
}

TEST_CASE("A stop requested during custom material resolution cancels staging") {
    Library library;
    library.registered.emplace("water", material("water"));
    library.registered.emplace("lava", material("lava"));
    Scene scene;
    auto object = scene.create("pool", library.shape);
    object.renderer().set_custom_material(0, library.registered.at("lava"));
    object.renderer().set_custom_material(1, library.registered.at("water"));
    const auto document = serialize_scene(scene, library.names());
    for (const bool set_document : {false, true}) {
        CAPTURE(set_document);
        SceneSet set;
        if (set_document)
            (void)set.load("pool", document, library.meshes(), {}, library.materials());
        const auto text = set_document ? set.serialize(library.names()) : document;
        StopSource stop;
        std::atomic<int> resolved{0};
        const CustomMaterialResolver stopping = [&](std::string_view name) {
            ++resolved;
            stop.request_stop();
            return library.materials()(name);
        };
        const StagingOptions options{stop.get_token(), nullptr};
        const auto staging = [&] {
            if (set_document)
                (void)stage_scene_set(text, library.meshes(), stopping, options);
            else
                (void)stage_scene(text, library.meshes(), stopping, options);
        };
        CHECK_THROWS_WITH_AS(std::async(std::launch::async, staging).get(), "Staging was cancelled", StagingCancelled);
        // The first of the two names stopped the call before the second was resolved.
        CHECK(resolved == 1);
    }
}
