#pragma once
// SPIR-V modules assembled word by word, so that a test of shader validation states exactly the interface it checks and
// needs no shader compiler: custom shaders' stages (custom_material_tests.cpp) and sprite shaders (sprite_tests.cpp).
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spirv_test {
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
} // namespace spirv_test
