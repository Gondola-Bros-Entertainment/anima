#include "surface_validation.hpp"
#include "texel_hold.hpp"
#include <algorithm>
#include <anima/custom_material.hpp>
#include <array>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace anima {
namespace {
// Numbers from the SPIR-V specification (unified1) for the instructions and enumerants that the interface check
// reads. Everything else is framed and skipped.
namespace spv {
constexpr std::uint32_t magic = 0x07230203, swapped_magic = 0x03022307;
constexpr std::uint32_t op_extension = 10, op_ext_inst_import = 11, op_memory_model = 14, op_entry_point = 15,
                        op_capability = 17, op_type_void = 19, op_type_bool = 20, op_type_int = 21, op_type_float = 22,
                        op_type_vector = 23, op_type_matrix = 24, op_type_image = 25, op_type_sampler = 26,
                        op_type_sampled_image = 27, op_type_array = 28, op_type_runtime_array = 29, op_type_struct = 30,
                        op_type_pointer = 32, op_constant = 43, op_variable = 59, op_decorate = 71,
                        op_member_decorate = 72;
constexpr std::uint32_t uniform_constant = 0, input = 1, uniform = 2, output = 3, push_constant = 9,
                        storage_buffer = 12;
constexpr std::uint32_t block = 2, buffer_block = 3, row_major = 4, array_stride = 6, matrix_stride = 7, built_in = 11,
                        non_writable = 24, location = 30, component = 31, binding = 33, descriptor_set = 34,
                        offset = 35;
constexpr std::uint32_t capability_matrix = 0, capability_shader = 1, capability_image_query = 50,
                        capability_derivative_control = 51;
constexpr std::uint32_t vertex_model = 0, fragment_model = 4, logical_addressing = 0, glsl450_memory = 1;
} // namespace spv

enum class Stage { vertex, fragment, shadow_vertex, shadow_fragment };
bool vertex_stage(Stage stage) { return stage == Stage::vertex || stage == Stage::shadow_vertex; }
[[noreturn]] void fail(Stage stage, const std::string &message) {
    constexpr std::array<const char *, 4> names{"vertex shader", "fragment shader", "shadow vertex shader",
                                                "shadow fragment shader"};
    throw std::invalid_argument(std::string("The ") + names[static_cast<std::size_t>(stage)] + ' ' + message);
}
void require(bool accepted, const char *message) {
    if (!accepted)
        throw std::invalid_argument(message);
}
std::string number(std::uint64_t value) { return std::to_string(value); }

// The documented interface; custom_material.hpp and anima/custom_material.glsl describe the same members.
struct Member {
    std::uint32_t offset;
    std::string_view type;
};
constexpr std::array<Member, 13> frame_members{{{0, "mat4"},
                                                {64, "mat4"},
                                                {128, "vec4"},
                                                {144, "vec4"},
                                                {160, "vec4"},
                                                {176, "vec4"},
                                                {192, "vec4"},
                                                {208, "vec4"},
                                                {224, "vec4"},
                                                {240, "vec4"},
                                                {256, "vec4"},
                                                {272, "vec4"},
                                                {288, "float"}}};
constexpr std::array<Member, 7> draw_members{
    {{0, "mat4"}, {64, "uint"}, {68, "uint"}, {72, "uint"}, {76, "uint"}, {80, "vec4"}, {96, "uint"}}};
// The SourceVertex attributes, then the three placement rows.
constexpr std::array<std::string_view, 11> attribute_types{"vec3", "vec3",  "vec3", "vec2", "uvec4", "vec4",
                                                           "vec4", "float", "vec4", "vec4", "vec4"};
constexpr std::array<std::string_view, 12> varying_types{"float", "vec2",  "vec3", "vec4",  "int",   "ivec2",
                                                         "ivec3", "ivec4", "uint", "uvec2", "uvec3", "uvec4"};
constexpr std::uint32_t varying_locations = 16;
// Saturation bound for block sizes, far above any accepted parameter block.
constexpr std::uint64_t size_cap = std::uint64_t{1} << 40;
// Deepest nesting of types, which bounds the recursion that names and measures them.
constexpr std::uint32_t maximum_type_depth = 64;

struct Type {
    enum class Kind {
        other,
        void_type,
        boolean,
        integer,
        floating,
        vector,
        matrix,
        image,
        sampler,
        sampled_image,
        array,
        runtime_array,
        structure,
        pointer
    };
    Kind kind = Kind::other;
    // Scalar width and signedness; vector, matrix and array element (component, column, element, pointee or
    // image type) and count; pointer storage class; image dimension, depth, arrayed, multisampled and sampled.
    std::uint32_t width{}, signedness{}, element{}, count{}, storage{};
    std::uint32_t dimension{}, depth{}, arrayed{}, multisampled{}, sampled{};
    std::vector<std::uint32_t> members;
    // Types nested in this one, counting itself.
    std::uint32_t nesting = 1;
};
Type of(Type::Kind kind) {
    Type value;
    value.kind = kind;
    return value;
}
struct Decorations {
    std::optional<std::uint32_t> location, set, binding, offset, array_stride, matrix_stride;
    bool block{}, buffer_block{}, row_major{}, built_in{}, component{}, non_writable{};
    void add(std::uint32_t decoration, std::optional<std::uint32_t> literal) {
        switch (decoration) {
        case spv::block:
            block = true;
            break;
        case spv::buffer_block:
            buffer_block = true;
            break;
        case spv::row_major:
            row_major = true;
            break;
        case spv::built_in:
            built_in = true;
            break;
        case spv::component:
            component = true;
            break;
        case spv::non_writable:
            non_writable = true;
            break;
        case spv::location:
            location = literal;
            break;
        case spv::descriptor_set:
            set = literal;
            break;
        case spv::binding:
            binding = literal;
            break;
        case spv::offset:
            offset = literal;
            break;
        case spv::array_stride:
            array_stride = literal;
            break;
        case spv::matrix_stride:
            matrix_stride = literal;
            break;
        default:
            break;
        }
    }
};
struct Variable {
    std::uint32_t id{}, type{}, storage{};
};

// The declarations of one module that the interface check reads.
class Module {
  public:
    Module(std::span<const std::uint32_t> words, Stage stage) : stage_(stage) { parse(words); }
    [[nodiscard]] Stage stage() const { return stage_; }
    [[nodiscard]] const std::vector<Variable> &variables() const { return variables_; }
    [[nodiscard]] const std::vector<std::uint32_t> &interface() const { return interface_; }
    [[nodiscard]] const Decorations &decorations(std::uint32_t id) const {
        static const Decorations none;
        const auto found = decorations_.find(id);
        return found == decorations_.end() ? none : found->second;
    }
    [[nodiscard]] const Decorations &member(std::uint32_t structure, std::uint32_t index) const {
        static const Decorations none;
        const auto found = members_.find({structure, index});
        return found == members_.end() ? none : found->second;
    }
    [[nodiscard]] const Type &type(std::uint32_t id) const {
        const auto found = types_.find(id);
        if (found == types_.end())
            fail(stage_, "is invalid SPIR-V: it refers to type " + number(id) + ", which it does not define");
        return found->second;
    }
    // The type that pointer @p id points to.
    [[nodiscard]] std::uint32_t pointee(std::uint32_t id) const {
        const auto &pointer = type(id);
        if (pointer.kind != Type::Kind::pointer)
            fail(stage_, "is invalid SPIR-V: a variable's type " + number(id) + " is not a pointer");
        return pointer.element;
    }
    // GLSL's name for type @p id, such as `vec4`, `mat4`, `sampler2D` or `mat4[]`.
    [[nodiscard]] std::string name(std::uint32_t id) const {
        const auto &value = type(id);
        using Kind = Type::Kind;
        switch (value.kind) {
        case Kind::void_type:
            return "void";
        case Kind::boolean:
            return "bool";
        case Kind::integer:
            return std::string(value.signedness ? "int" : "uint") + (value.width == 32 ? "" : number(value.width));
        case Kind::floating:
            return value.width == 32 ? "float" : value.width == 64 ? "double" : "float" + number(value.width);
        case Kind::vector: {
            const auto &component = type(value.element);
            std::string letter;
            if (component.kind == Kind::integer && component.width == 32)
                letter = component.signedness ? "i" : "u";
            else if (component.kind == Kind::boolean)
                letter = "b";
            else if (component.kind != Kind::floating || component.width != 32)
                return "vector of " + number(value.count) + ' ' + name(value.element);
            return letter + "vec" + number(value.count);
        }
        case Kind::matrix: {
            const auto &column = type(value.element);
            if (column.kind != Kind::vector || name(value.element) != "vec" + number(column.count))
                return "matrix of " + number(value.count) + ' ' + name(value.element);
            return "mat" + number(value.count) + (value.count == column.count ? "" : "x" + number(column.count));
        }
        case Kind::image:
            return prefix(value) + (value.sampled == 2 ? "image" : "texture") + dimension(value);
        case Kind::sampler:
            return "sampler";
        case Kind::sampled_image: {
            const auto &image = type(value.element);
            if (image.kind != Kind::image)
                return "sampled image";
            return prefix(image) + "sampler" + dimension(image) + (image.depth == 1 ? "Shadow" : "");
        }
        case Kind::array: {
            const auto found = constants_.find(value.count);
            return name(value.element) + '[' + (found == constants_.end() ? "?" : number(found->second)) + ']';
        }
        case Kind::runtime_array:
            return name(value.element) + "[]";
        case Kind::structure:
            return "struct";
        case Kind::pointer:
            return name(value.element);
        case Kind::other:
            break;
        }
        return "opaque type";
    }
    // name() of member @p index of struct @p structure, marked `row_major` when the member is laid out so.
    [[nodiscard]] std::string member_name(std::uint32_t structure, std::uint32_t index) const {
        const auto type_name = name(type(structure).members.at(index));
        return member(structure, index).row_major ? "row_major " + type_name : type_name;
    }
    // Bytes that a block member of type @p id with decorations @p decorated spans, saturating at size_cap.
    [[nodiscard]] std::uint64_t size(std::uint32_t id, const Decorations &decorated) const {
        const auto &value = type(id);
        using Kind = Type::Kind;
        const auto product = [](std::uint64_t a, std::uint64_t b) {
            return a != 0 && b > size_cap / a ? size_cap : std::min(a * b, size_cap);
        };
        switch (value.kind) {
        case Kind::boolean:
            return 4;
        case Kind::integer:
        case Kind::floating:
            return value.width / 8;
        case Kind::vector:
            return product(value.count, size(value.element, {}));
        case Kind::matrix: {
            if (!decorated.matrix_stride)
                fail(stage_, "is invalid SPIR-V: a block's matrix member has no matrix stride");
            return product(decorated.row_major ? type(value.element).count : value.count, *decorated.matrix_stride);
        }
        case Kind::array: {
            const auto length = constants_.find(value.count);
            const auto &stride = decorations(id).array_stride;
            if (length == constants_.end() || !stride)
                fail(stage_, "declares a parameter block array without a constant length and stride");
            return product(length->second, *stride);
        }
        case Kind::structure:
            return extent(id);
        default:
            fail(stage_, "declares a parameter block member of type " + name(id) + ", which has no fixed size");
        }
    }
    // Bytes from the start of block or struct @p structure to the end of its last member, saturating at size_cap.
    [[nodiscard]] std::uint64_t extent(std::uint32_t structure) const {
        std::uint64_t result = 0;
        const auto &value = type(structure);
        for (std::uint32_t i = 0; i < value.members.size(); ++i) {
            const auto &decorated = member(structure, i);
            if (!decorated.offset)
                fail(stage_, "is invalid SPIR-V: a block member has no offset");
            result = std::max(result, std::min(size_cap, *decorated.offset + size(value.members[i], decorated)));
        }
        return result;
    }

  private:
    // Framing of one instruction, whose operands are read with bounds checks.
    struct Instruction {
        std::span<const std::uint32_t> words;
        std::size_t position;
        Stage stage;
        std::uint32_t operator[](std::size_t index) const {
            if (index >= words.size())
                fail(stage,
                     "is invalid SPIR-V: the instruction at word " + number(position) + " is too short for its opcode");
            return words[index];
        }
        // The literal string that starts at operand @p first; @p next receives the operand after it.
        std::string string(std::size_t first, std::size_t &next) const {
            std::string result;
            for (auto index = first; index < words.size(); ++index)
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    const auto byte = static_cast<char>((words[index] >> shift) & 0xFFU);
                    if (byte == '\0') {
                        next = index + 1;
                        return result;
                    }
                    result += byte;
                }
            fail(stage,
                 "is invalid SPIR-V: a string in the instruction at word " + number(position) + " has no terminator");
        }
    };
    void define(const Instruction &instruction, std::uint32_t id) const {
        if (id == 0 || id >= bound_)
            fail(stage_, "is invalid SPIR-V: the instruction at word " + number(instruction.position) + " defines id " +
                             number(id) + " outside the bound " + number(bound_));
    }
    // Records type @p value, whose operand types SPIR-V requires to be defined already, so that the types form an
    // acyclic graph no deeper than maximum_type_depth.
    void add_type(const Instruction &instruction, Type value) {
        const auto id = instruction[1];
        define(instruction, id);
        using Kind = Type::Kind;
        auto operands = value.members;
        if (value.kind == Kind::vector || value.kind == Kind::matrix || value.kind == Kind::image ||
            value.kind == Kind::sampled_image || value.kind == Kind::array || value.kind == Kind::runtime_array ||
            value.kind == Kind::pointer)
            operands.push_back(value.element);
        for (const auto operand : operands) {
            const auto found = types_.find(operand);
            if (found == types_.end())
                fail(stage_, "is invalid SPIR-V: type " + number(id) + " refers to type " + number(operand) +
                                 " before defining it");
            value.nesting = std::max(value.nesting, found->second.nesting + 1);
        }
        if (value.nesting > maximum_type_depth)
            fail(stage_, "nests types more than " + number(maximum_type_depth) + " deep");
        if (!types_.emplace(id, std::move(value)).second)
            fail(stage_, "is invalid SPIR-V: it defines type " + number(id) + " twice");
    }
    void parse(std::span<const std::uint32_t> words) {
        if (words.size() > CustomMaterial::max_shader_bytes / sizeof(std::uint32_t))
            fail(stage_, "is larger than " + number(CustomMaterial::max_shader_bytes) + " bytes");
        if (words.size() < 5)
            fail(stage_, "is not SPIR-V: it is shorter than the five-word module header");
        if (words[0] == spv::swapped_magic)
            fail(stage_, "is not SPIR-V in host byte order: its magic number is byte-swapped");
        if (words[0] != spv::magic)
            fail(stage_, "is not SPIR-V: its first word is not the SPIR-V magic number");
        const auto version_major = (words[1] >> 16) & 0xFFU, version_minor = (words[1] >> 8) & 0xFFU;
        if ((words[1] & 0xFF0000FFU) != 0 || version_major != 1 || version_minor > 3)
            fail(stage_, "is SPIR-V " + number(version_major) + '.' + number(version_minor) +
                             ", but custom materials accept SPIR-V 1.0 to 1.3");
        bound_ = words[3];
        const auto model = vertex_stage(stage_) ? spv::vertex_model : spv::fragment_model;
        bool entry = false, memory_model = false;
        using Kind = Type::Kind;
        for (std::size_t position = 5; position < words.size();) {
            const auto count = words[position] >> 16, opcode = words[position] & 0xFFFFU;
            if (count == 0)
                fail(stage_,
                     "is invalid SPIR-V: the instruction at word " + number(position) + " has a word count of 0");
            if (count > words.size() - position)
                fail(stage_, "is invalid SPIR-V: the instruction at word " + number(position) +
                                 " runs past the end of the module");
            const Instruction instruction{words.subspan(position, count), position, stage_};
            std::size_t next = 0;
            switch (opcode) {
            case spv::op_capability: {
                const auto capability = instruction[1];
                if (capability != spv::capability_matrix && capability != spv::capability_shader &&
                    capability != spv::capability_image_query && capability != spv::capability_derivative_control)
                    fail(stage_, "requires SPIR-V capability " + number(capability) +
                                     ", which custom materials do not support");
                break;
            }
            case spv::op_extension:
                fail(stage_, "requires the SPIR-V extension " + instruction.string(1, next) +
                                 ", which custom materials do not support");
            case spv::op_ext_inst_import: {
                const auto set = instruction.string(2, next);
                if (set != "GLSL.std.450")
                    fail(stage_,
                         "imports the extended instructions " + set + ", which custom materials do not support");
                break;
            }
            case spv::op_memory_model:
                if (instruction[1] != spv::logical_addressing || instruction[2] != spv::glsl450_memory)
                    fail(stage_, "does not use the Logical addressing and GLSL450 memory models");
                memory_model = true;
                break;
            case spv::op_entry_point: {
                const auto execution = instruction[1];
                const auto entry_name = instruction.string(3, next);
                if (execution == model && entry_name == "main") {
                    entry = true;
                    interface_.assign(instruction.words.begin() + static_cast<std::ptrdiff_t>(next),
                                      instruction.words.end());
                }
                break;
            }
            case spv::op_decorate: {
                const auto literal =
                    count > 3 ? std::optional<std::uint32_t>(instruction[3]) : std::optional<std::uint32_t>();
                decorations_[instruction[1]].add(instruction[2], literal);
                break;
            }
            case spv::op_member_decorate: {
                const auto literal =
                    count > 4 ? std::optional<std::uint32_t>(instruction[4]) : std::optional<std::uint32_t>();
                members_[{instruction[1], instruction[2]}].add(instruction[3], literal);
                break;
            }
            case spv::op_type_void:
                add_type(instruction, of(Kind::void_type));
                break;
            case spv::op_type_bool:
                add_type(instruction, of(Kind::boolean));
                break;
            case spv::op_type_int: {
                auto value = of(Kind::integer);
                value.width = instruction[2];
                value.signedness = instruction[3];
                add_type(instruction, std::move(value));
                break;
            }
            case spv::op_type_float: {
                auto value = of(Kind::floating);
                value.width = instruction[2];
                add_type(instruction, std::move(value));
                break;
            }
            case spv::op_type_vector:
            case spv::op_type_matrix:
            case spv::op_type_array: {
                auto value = of(opcode == spv::op_type_vector   ? Kind::vector
                                : opcode == spv::op_type_matrix ? Kind::matrix
                                                                : Kind::array);
                value.element = instruction[2];
                value.count = instruction[3];
                add_type(instruction, std::move(value));
                break;
            }
            case spv::op_type_image: {
                auto value = of(Kind::image);
                value.element = instruction[2];
                value.dimension = instruction[3];
                value.depth = instruction[4];
                value.arrayed = instruction[5];
                value.multisampled = instruction[6];
                value.sampled = instruction[7];
                add_type(instruction, std::move(value));
                break;
            }
            case spv::op_type_sampler:
                add_type(instruction, of(Kind::sampler));
                break;
            case spv::op_type_sampled_image:
            case spv::op_type_runtime_array: {
                auto value = of(opcode == spv::op_type_sampled_image ? Kind::sampled_image : Kind::runtime_array);
                value.element = instruction[2];
                add_type(instruction, std::move(value));
                break;
            }
            case spv::op_type_struct: {
                auto value = of(Kind::structure);
                value.members.assign(instruction.words.begin() + 2, instruction.words.end());
                add_type(instruction, std::move(value));
                break;
            }
            case spv::op_type_pointer: {
                auto value = of(Kind::pointer);
                value.storage = instruction[2];
                value.element = instruction[3];
                add_type(instruction, std::move(value));
                break;
            }
            case spv::op_constant: {
                define(instruction, instruction[2]);
                const auto found = types_.find(instruction[1]);
                // Array lengths are 32-bit integer constants.
                if (found != types_.end() && found->second.kind == Kind::integer && found->second.width == 32)
                    constants_[instruction[2]] = instruction[3];
                break;
            }
            case spv::op_variable: {
                define(instruction, instruction[2]);
                variables_.push_back({instruction[2], instruction[1], instruction[3]});
                break;
            }
            default:
                break;
            }
            position += count;
        }
        if (!memory_model)
            fail(stage_, "does not use the Logical addressing and GLSL450 memory models");
        if (!entry)
            fail(stage_,
                 std::string("has no ") + (vertex_stage(stage_) ? "vertex" : "fragment") + " entry point named main");
    }
    // GLSL's prefix for the component type of @p image: `i`, `u`, or none for 32-bit float.
    [[nodiscard]] std::string prefix(const Type &image) const {
        const auto &sampled = type(image.element);
        if (sampled.kind == Type::Kind::integer)
            return sampled.signedness ? "i" : "u";
        return sampled.kind == Type::Kind::floating && sampled.width == 32 ? "" : "(" + name(image.element) + ")";
    }
    static std::string dimension(const Type &image) {
        constexpr std::array<const char *, 7> dimensions{"1D", "2D", "3D", "Cube", "2DRect", "Buffer", "SubpassData"};
        return (image.dimension < dimensions.size() ? dimensions[image.dimension] : "?") +
               std::string(image.multisampled ? "MS" : "") + (image.arrayed ? "Array" : "");
    }
    Stage stage_;
    std::uint32_t bound_{};
    std::map<std::uint32_t, Type> types_;
    std::map<std::uint32_t, std::uint32_t> constants_;
    std::map<std::uint32_t, Decorations> decorations_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Decorations> members_;
    std::vector<Variable> variables_;
    std::vector<std::uint32_t> interface_;
};

// What one stage reads and passes on.
struct StageInterface {
    // Values passed between stages by location: the fragment inputs, or the vertex outputs.
    std::map<std::uint32_t, std::string> inputs, outputs;
    std::uint32_t attributes{};
    bool opaque_depth{}, opaque_color{};
};

// Checks the members of @p structure, the block @p what, against @p documented.
template <std::size_t Count>
void check_members(const Module &shader, std::uint32_t structure, const std::array<Member, Count> &documented,
                   const std::string &what) {
    const auto &value = shader.type(structure);
    for (std::uint32_t i = 0; i < value.members.size(); ++i) {
        const auto &decorated = shader.member(structure, i);
        if (!decorated.offset)
            fail(shader.stage(), "is invalid SPIR-V: a block member has no offset");
        const auto type = shader.member_name(structure, i);
        const auto found = std::find_if(documented.begin(), documented.end(),
                                        [&](const Member &entry) { return entry.offset == *decorated.offset; });
        if (found == documented.end())
            fail(shader.stage(), "declares " + what + " member at offset " + number(*decorated.offset) + " as " + type +
                                     ", but the custom material interface has no member there");
        if (found->type != type)
            fail(shader.stage(), "declares " + what + " member at offset " + number(*decorated.offset) + " as " + type +
                                     ", but the custom material interface has a " + std::string(found->type) +
                                     " there");
    }
}

StageInterface check_stage(const Module &shader, const CustomMaterialDefinition &definition) {
    const auto stage = shader.stage();
    StageInterface result;
    using Kind = Type::Kind;
    // Resources: every declared one counts as read.
    for (const auto &variable : shader.variables()) {
        if (variable.storage != spv::uniform_constant && variable.storage != spv::uniform &&
            variable.storage != spv::storage_buffer && variable.storage != spv::push_constant)
            continue;
        const auto pointee = shader.pointee(variable.type);
        const auto &type = shader.type(pointee);
        const auto &type_decorations = shader.decorations(pointee);
        const bool structure = type.kind == Kind::structure;
        if (variable.storage == spv::push_constant) {
            if (!structure || !type_decorations.block)
                fail(stage, "declares push constants that are not a block");
            check_members(shader, pointee, draw_members, "the draw push constant");
            continue;
        }
        const auto &decorated = shader.decorations(variable.id);
        if (!decorated.set || !decorated.binding)
            fail(stage, "declares a resource without a descriptor set and binding");
        const auto set = *decorated.set, binding = *decorated.binding;
        std::string kind = shader.name(pointee);
        if (structure && variable.storage == spv::uniform && type_decorations.block)
            kind = "uniform block";
        else if (structure && ((variable.storage == spv::uniform && type_decorations.buffer_block) ||
                               (variable.storage == spv::storage_buffer && type_decorations.block)))
            kind = "storage buffer";
        const auto where = "set " + number(set) + " binding " + number(binding);
        const auto expect = [&](std::string_view wanted, std::string_view provided) {
            if (kind != wanted)
                fail(stage, "declares " + where + " as " + kind + ", but the custom material interface provides " +
                                std::string(provided) + " there");
        };
        if (set == 0 && binding == 0) {
            expect("uniform block", "the frame uniform block");
            check_members(shader, pointee, frame_members, "the frame block");
        } else if (set == 0 && (binding == 1 || binding == 2)) {
            const std::string input =
                binding == 1 ? "opaque depth (set 0, binding 1)" : "opaque color (set 0, binding 2)";
            expect("sampler2D", "a sampler2D");
            if (vertex_stage(stage))
                fail(stage, "declares " + input + ", which only fragment shaders can read");
            if (stage == Stage::shadow_fragment)
                fail(stage, "declares " + input + ", which the depth-only variant cannot read");
            if (definition.blend == CustomBlend::opaque)
                fail(stage, "declares " + input + ", but only blended and additive custom materials can read it");
            (binding == 1 ? result.opaque_depth : result.opaque_color) = true;
        } else if (set == 1 && binding == 0) {
            if (!vertex_stage(stage))
                fail(stage, "declares the pose buffer (set 1, binding 0), which only vertex shaders can read");
            const bool poses = kind == "storage buffer" && type.members.size() == 1 &&
                               shader.member_name(pointee, 0) == "mat4[]" && shader.member(pointee, 0).offset == 0U &&
                               shader.decorations(type.members[0]).array_stride == 64U;
            if (!poses)
                fail(stage, "declares " + where + " as " + kind +
                                ", but the custom material interface provides the pose buffer of mat4[] there");
            if (!shader.member(pointee, 0).non_writable && !decorated.non_writable)
                fail(stage, "declares the pose buffer (set 1, binding 0) without readonly");
        } else if (set == 2 && binding == 0) {
            expect("uniform block", "the parameter uniform block");
            const auto bytes = shader.extent(pointee);
            if (bytes > definition.parameters.size())
                fail(stage, "declares a parameter block of " + number(bytes) + " bytes, but the material supplies " +
                                number(definition.parameters.size()));
        } else if (set == 2 && binding >= 1 && binding <= CustomMaterial::max_textures) {
            expect("sampler2D", "a sampler2D");
            if (binding > definition.textures.size())
                fail(stage, "declares texture " + number(binding - 1) + " (set 2, binding " + number(binding) +
                                "), but the material supplies " + number(definition.textures.size()) +
                                (definition.textures.size() == 1 ? " texture" : " textures"));
        } else
            fail(stage, "declares " + where + ", which the custom material interface does not provide");
    }
    // Inputs and outputs that the entry point lists.
    bool color = false;
    for (const auto id : shader.interface()) {
        const auto found = std::find_if(shader.variables().begin(), shader.variables().end(),
                                        [&](const Variable &candidate) { return candidate.id == id; });
        if (found == shader.variables().end() || (found->storage != spv::input && found->storage != spv::output))
            continue;
        const bool input = found->storage == spv::input;
        const auto pointee = shader.pointee(found->type);
        const auto &decorated = shader.decorations(id);
        bool built_in = decorated.built_in;
        const auto &type = shader.type(pointee);
        if (type.kind == Kind::structure)
            for (std::uint32_t i = 0; i < type.members.size(); ++i)
                built_in = built_in || shader.member(pointee, i).built_in;
        if (built_in)
            continue;
        const auto direction = input ? std::string("an input") : std::string("an output");
        if (!decorated.location)
            fail(stage, "declares " + direction + " without a location");
        const auto location = *decorated.location;
        if (decorated.component)
            fail(stage, "declares location " + number(location) +
                            " with a component, which custom materials do "
                            "not support");
        const auto name = shader.name(pointee);
        if (input && vertex_stage(stage)) {
            if (location >= attribute_types.size())
                fail(stage, "reads vertex attribute " + number(location) +
                                ", which the custom material interface does not provide");
            if (name != attribute_types[location])
                fail(stage, "reads vertex attribute " + number(location) + " as " + name +
                                ", but the custom material interface provides a " +
                                std::string(attribute_types[location]) + " there");
            result.attributes |= 1U << location;
            continue;
        }
        if (!input && !vertex_stage(stage)) {
            if (stage == Stage::shadow_fragment)
                fail(stage, "writes color at location " + number(location) +
                                ", but the depth-only variant has no color target");
            if (location != 0)
                fail(stage, "writes location " + number(location) +
                                ", but custom materials have one color output at location 0");
            if (name != "vec4")
                fail(stage, "writes location 0 as " + name + ", but custom materials write a vec4 there");
            color = true;
            continue;
        }
        // A value passed from the vertex shader to the fragment shader.
        if (std::find(varying_types.begin(), varying_types.end(), name) == varying_types.end())
            fail(stage, "passes " + name + " at location " + number(location) +
                            " between stages, but custom materials pass 32-bit scalars and vectors");
        if (location >= varying_locations)
            fail(stage, "passes a value at location " + number(location) +
                            " between stages, but custom materials use locations 0 to 15");
        if (!(input ? result.inputs : result.outputs).emplace(location, name).second)
            fail(stage, "declares location " + number(location) + " twice");
    }
    if (stage == Stage::fragment && !color)
        fail(stage, "writes no color at location 0");
    return result;
}

// Requires every input of @p fragment to be an output of @p vertex with the same type.
void match(const StageInterface &vertex, const StageInterface &fragment, Stage fragment_stage,
           const char *vertex_name) {
    for (const auto &[location, name] : fragment.inputs) {
        const auto found = vertex.outputs.find(location);
        if (found == vertex.outputs.end())
            fail(fragment_stage, "reads location " + number(location) + " as " + name + ", which the " + vertex_name +
                                     " does not write");
        if (found->second != name)
            fail(fragment_stage, "reads location " + number(location) + " as " + name + ", but the " + vertex_name +
                                     " writes a " + found->second + " there");
    }
}
} // namespace

CustomMaterial::CustomMaterial(CustomMaterialDefinition definition) : definition_(std::move(definition)) {
    const auto &value = definition_;
    require(!value.name.empty() && value.name.size() <= max_name_bytes, "Custom material name must be 1 to 4096 bytes");
    require(value.blend == CustomBlend::opaque || value.blend == CustomBlend::blended ||
                value.blend == CustomBlend::additive,
            "Unknown custom material blend mode");
    require(value.texel_retention == TexelRetention::keep || value.texel_retention == TexelRetention::until_upload,
            "Unknown custom material texel retention");
    require(value.parameters.size() <= max_parameter_bytes, "Custom material parameters exceed 256 bytes");
    require(value.textures.size() <= max_textures, "Custom material has more than 4 textures");
    detail::validate_surfaces({}, value.textures, detail::Texels::required);
    require(!value.vertex_shader.empty() && !value.fragment_shader.empty(),
            "Custom material needs a vertex and a fragment shader");
    require(value.shadow_fragment_shader.empty() || !value.shadow_vertex_shader.empty(),
            "Custom material has a shadow fragment shader without a shadow vertex shader");
    const auto vertex = check_stage(Module(value.vertex_shader, Stage::vertex), value);
    const auto fragment = check_stage(Module(value.fragment_shader, Stage::fragment), value);
    match(vertex, fragment, Stage::fragment, "vertex shader");
    if (!value.shadow_vertex_shader.empty()) {
        const auto shadow = check_stage(Module(value.shadow_vertex_shader, Stage::shadow_vertex), value);
        if (!value.shadow_fragment_shader.empty())
            match(shadow, check_stage(Module(value.shadow_fragment_shader, Stage::shadow_fragment), value),
                  Stage::shadow_fragment, "shadow vertex shader");
        shadow_vertex_attributes_ = shadow.attributes;
    }
    vertex_attributes_ = vertex.attributes;
    reads_opaque_depth_ = fragment.opaque_depth;
    reads_opaque_color_ = fragment.opaque_color;
    if (value.texel_retention == TexelRetention::until_upload)
        texels_ =
            detail::hold_texels(definition_.textures, "Custom material texture texels were released after upload");
}
std::vector<std::shared_ptr<const Image>> CustomMaterial::texel_images() const {
    if (const auto *hold = texels_.get())
        return hold->images();
    std::vector<std::shared_ptr<const Image>> result;
    result.reserve(definition_.textures.size());
    for (const auto &texture : definition_.textures)
        result.push_back(texture.image);
    return result;
}
void CustomMaterial::release_texels() const noexcept {
    if (auto *hold = texels_.get())
        hold->release();
}
} // namespace anima
