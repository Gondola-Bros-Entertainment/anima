#pragma once
// Texture memory through the public API: how long compiled meshes and custom materials hold the texels of their
// textures on the CPU (TexelRetention), and what their device images take (ResourceStats::resident_texture_bytes),
// whole and without the top mip levels that VulkanRenderer::set_texture_mip_skip() drops. Geometry memory: the bytes
// each mesh's indices upload at, 2 for a mesh with at most 65,536 vertices and otherwise 4
// (ResourceStats::geometry_uploaded_bytes), and that both widths draw the same.
// CPU memory is measured as the bytes of texel storage still alive, through weak references to each source Image: an
// image counts until its last holder lets it go, whoever that is.
#include "blending.hpp"
#include "custom_materials.hpp"
#include "gpu_checks.hpp"
#include "rejection.hpp"
#include <anima/assets/mesh_preparation.hpp>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/impostor.hpp>
#include <anima/scene.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace texture_memory_test {
using rejection::rejects;
constexpr auto released_message = "Mesh texture texels were released after upload";
// The last stage that can fail before an upload lets texels go.
constexpr auto late_failure = anima::RendererFailureStage::descriptors;
constexpr auto late_failure_message = "Injected resource preparation failure after descriptors";

inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
// A representative texture: 2048 texels square, 16 MiB of RGBA8 texels, about 21 MiB with mips on the device.
constexpr std::uint32_t edge = 2048;
// Smooth red and green ramps across the texture, with blue in a checker of 64-texel squares so that minification
// has detail to filter.
inline std::shared_ptr<const anima::Image> ramp() {
    constexpr std::uint32_t square = 64;
    anima::Image image{edge, edge, std::vector<std::uint8_t>(std::size_t{edge} * edge * 4)};
    for (std::uint32_t y = 0; y < edge; ++y)
        for (std::uint32_t x = 0; x < edge; ++x) {
            auto *texel = &image.rgba[(std::size_t{y} * edge + x) * 4];
            texel[0] = static_cast<std::uint8_t>(x * 255 / (edge - 1));
            texel[1] = static_cast<std::uint8_t>(y * 255 / (edge - 1));
            texel[2] = (x / square + y / square) % 2 ? 224 : 32;
            texel[3] = 255;
        }
    return std::make_shared<const anima::Image>(std::move(image));
}
// A quad facing +Z at z = -3, 2 units square, whose unlit material shows @p image as its base color.
inline std::shared_ptr<const anima::Asset> textured_quad(std::shared_ptr<const anima::Image> image) {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(1);
    auto surface = blending_test::opaque({1, 1, 1});
    surface.texture = 0;
    asset->materials.push_back(surface);
    asset->textures.push_back({std::move(image), {}});
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    constexpr float depth = -3;
    const std::array<std::array<float, 2>, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    for (const auto corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
        anima::SourceVertex vertex;
        vertex.position = {corners[corner][0], corners[corner][1], depth};
        vertex.normal = {0, 0, 1};
        vertex.uv = {(corners[corner][0] + 1) / 2, (1 - corners[corner][1]) / 2};
        primitive.vertices.push_back(vertex);
    }
    asset->primitives.push_back(std::move(primitive));
    return asset;
}
// The most vertices a mesh can have and still upload 16-bit indices.
constexpr std::size_t narrow_vertices = 65536;
// A white unlit quad facing +Z at z = -3, 2 units square, after @p fillers vertices of zero-area triangles at its
// center, which draw nothing, so that the quad's own 4 vertices come last in Mesh::vertices(), at the top of the
// index range.
inline std::shared_ptr<const anima::Mesh> quad_after(std::size_t fillers) {
    anima::Asset asset;
    asset.nodes.resize(1);
    asset.materials.push_back(blending_test::opaque({1, 1, 1}));
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    constexpr float depth = -3;
    // Mesh::compile welds only vertices whose attributes all match, so distinct texture coordinates keep each filler.
    const auto filler = [](std::size_t i) {
        anima::SourceVertex vertex;
        vertex.position = {0, 0, depth};
        vertex.normal = {0, 0, 1};
        vertex.uv = {static_cast<float>(i), 0};
        return vertex;
    };
    for (std::size_t i = 0; i < fillers; ++i)
        primitive.vertices.push_back(filler(i));
    // Repeating the first fillers completes the last triangle without adding vertices.
    for (std::size_t i = 0; primitive.vertices.size() % 3; ++i)
        primitive.vertices.push_back(filler(i));
    const std::array<std::array<float, 2>, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    for (const auto corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
        anima::SourceVertex vertex;
        vertex.position = {corners[corner][0], corners[corner][1], depth};
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return anima::Mesh::compile(asset);
}
// Bytes of texel storage still alive among @p images.
inline std::size_t held_bytes(std::initializer_list<const std::weak_ptr<const anima::Image> *> images) {
    std::size_t total = 0;
    for (const auto *image : images)
        if (const auto alive = image->lock())
            total += alive->rgba.size();
    return total;
}
inline std::shared_ptr<anima::Scene> scene_of(const std::shared_ptr<const anima::Mesh> &mesh) {
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->create({}, mesh);
    return scene;
}

// One renderer on one window, drawing selections and keeping each frame read back by name.
class Harness {
  public:
    /// A harness whose renderer decodes BC7 images to RGBA8 when @p decode_bc7 (RendererOptions::decode_bc7) and
    /// starts at @p texture_mip_skip (RendererOptions::texture_mip_skip).
    explicit Harness(const std::filesystem::path &output, bool decode_bc7 = false, std::uint32_t texture_mip_skip = 0)
        : window_(gpu_check::window("Anima texture memory verification", 640, 480)),
          renderer_(window_.get(), options(decode_bc7, texture_mip_skip)), images(output) {
        int width = 0, height = 0;
        require(SDL_GetWindowSizeInPixels(window_.get(), &width, &height) && width > 0 && height > 0,
                "Texture memory window has no drawable size");
        view_ = blending_test::perspective_view(float(width) / float(height));
    }
    [[nodiscard]] anima::VulkanRenderer &renderer() { return renderer_; }
    /// Selects @p scenes, uploading their meshes.
    void select(std::vector<std::shared_ptr<const anima::Scene>> scenes) {
        renderer_.set_scenes(std::move(scenes));
        renderer_.set_view(view_);
    }
    /// Draws the selection and reads the frame back as @p name.
    void render(const std::string &name) {
        renderer_.request_capture();
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Texture memory watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Texture memory test interrupted");
            if (renderer_.draw())
                break;
            SDL_Delay(5);
        }
        images.add(name, gpu_check::take(renderer_));
    }
    /// Shuts the renderer down and requires clean validation.
    void finish() {
        const auto result = renderer_.shutdown();
        require(!result.validation_errors && !result.validation_warnings, "Texture memory GPU validation failed");
    }

  private:
    static anima::RendererOptions options(bool decode_bc7, std::uint32_t texture_mip_skip) {
        anima::RendererOptions settings;
        settings.validation = true;
        settings.decode_bc7 = decode_bc7;
        settings.texture_mip_skip = texture_mip_skip;
        return settings;
    }
    gpu_check::Video video_;
    gpu_check::Window window_;
    anima::VulkanRenderer renderer_;
    anima::Mat4 view_{};

  public:
    gpu_check::Captures images;
};

// A mesh with 65,536 vertices uploads 2 bytes per index and one with 65,537 uploads 4, and each draws its last
// vertices, which an index of the wrong width would lose, exactly as a quad of 4 vertices does.
inline void check_index_widths(Harness &harness) {
    harness.select({scene_of(blending_test::facing(blending_test::opaque({1, 1, 1}), {0, 0, -3}, 1, 1))});
    harness.render("plain quad");
    harness.images.require_foreground("plain quad", "The plain quad is not visible");
    struct Width {
        std::string name;
        std::size_t vertices, index_bytes;
    };
    for (const auto &width :
         {Width{"16-bit indices", narrow_vertices, 2}, Width{"32-bit indices", narrow_vertices + 1, 4}}) {
        const auto mesh = quad_after(width.vertices - 4);
        require(mesh->vertices().size() == width.vertices, "The " + width.name + " quad has the wrong vertex count");
        const auto before = harness.renderer().resource_stats().geometry_uploaded_bytes;
        harness.select({scene_of(mesh)});
        const auto uploaded = harness.renderer().resource_stats().geometry_uploaded_bytes - before;
        std::cout << "INDEX MEMORY " << width.vertices << " vertices, " << mesh->indices().size()
                  << " indices: " << uploaded << " geometry bytes uploaded\n";
        require(uploaded == mesh->vertices().size_bytes() + mesh->indices().size() * width.index_bytes,
                "The " + width.name + " quad must upload " + std::to_string(width.index_bytes) + " bytes per index");
        harness.render(width.name);
        harness.images.require_same("plain quad", width.name, "The " + width.name + " quad drew differently");
    }
}

// Requires dropping one more top level to have saved about three quarters of the device bytes, from @p whole to
// @p kept: a level holds three quarters of the texels of the chain that it heads.
inline void require_quartered(std::uint64_t whole, std::uint64_t kept, const std::string &what) {
    require(kept < whole && (whole - kept) * 100 >= whole * 70 && (whole - kept) * 100 <= whole * 76,
            what + ": dropping a level left " + std::to_string(kept) + " of " + std::to_string(whole) +
                " device bytes, not about a quarter");
}
// Top mip levels dropped at upload, from RendererOptions::texture_mip_skip and then set_texture_mip_skip(): device
// bytes of the ramp mipmapped and unmipmapped, drawn frames, and impostor atlases, whose frames limit the levels.
inline void check_mip_skip(const std::filesystem::path &output) {
    Harness harness(output, false, 1);
    auto &renderer = harness.renderer();
    require(renderer.texture_mip_skip() == 1, "The renderer must start at RendererOptions::texture_mip_skip");
    // Each upload compiles its own Mesh, which stays cached while this holds it; its bytes count the 1x1 white of its
    // missing maps too.
    std::vector<std::shared_ptr<const anima::Mesh>> meshes;
    const auto upload = [&](std::shared_ptr<const anima::Mesh> mesh) {
        const auto before = renderer.resource_stats().resident_texture_bytes;
        meshes.push_back(std::move(mesh));
        renderer.prepare_meshes(std::span(&meshes.back(), 1));
        return renderer.resource_stats().resident_texture_bytes - before;
    };
    const auto mipmapped = textured_quad(ramp());
    auto unmipmapped = *mipmapped;
    unmipmapped.textures.at(0).sampler.mipmapped = false;
    const auto one = upload(anima::Mesh::compile(*mipmapped));
    const auto skipped_mesh = meshes.back();
    const auto unmipmapped_one = upload(anima::Mesh::compile(unmipmapped));
    harness.select({scene_of(skipped_mesh)});
    harness.render("skip-1");

    const auto resident = renderer.resource_stats().resident_texture_bytes;
    renderer.set_texture_mip_skip(0);
    require(renderer.texture_mip_skip() == 0, "set_texture_mip_skip() must set the count");
    require(renderer.resource_stats().resident_texture_bytes == resident,
            "set_texture_mip_skip() must not change uploaded textures");
    renderer.prepare_meshes(std::span(&skipped_mesh, 1));
    require(renderer.resource_stats().resident_texture_bytes == resident,
            "A cached mesh must keep the levels it was uploaded with");
    const auto none = upload(anima::Mesh::compile(*mipmapped));
    const auto whole_mesh = meshes.back();
    const auto unmipmapped_none = upload(anima::Mesh::compile(unmipmapped));
    harness.select({scene_of(whole_mesh)});
    harness.render("skip-0");
    renderer.set_texture_mip_skip(2);
    const auto two = upload(anima::Mesh::compile(*mipmapped));
    // More levels than the chain holds leave its last, 1x1, which shows one color.
    renderer.set_texture_mip_skip(100);
    const auto all = upload(anima::Mesh::compile(*mipmapped));
    const auto flat_mesh = meshes.back();
    const auto unmipmapped_all = upload(anima::Mesh::compile(unmipmapped));
    harness.select({scene_of(flat_mesh)});
    harness.render("skip-all");

    // An impostor atlas of @p frames by @p frames frames of @p frame texels each, uploaded at @p skip.
    const auto impostor = [&](std::uint32_t frames, std::uint32_t frame, std::uint32_t skip) {
        const auto atlas_edge = frames * frame;
        const auto image = std::make_shared<const anima::Image>(anima::Image{
            atlas_edge, atlas_edge, std::vector<std::uint8_t>(std::size_t{atlas_edge} * atlas_edge * 4, 128)});
        anima::ImpostorAtlas atlas;
        atlas.frames = {anima::ImpostorLayout::hemisphere, frames, {0, 0, 0}, 1};
        atlas.color = {image, {}, anima::TextureEncoding::srgb};
        atlas.normal_depth = {image, {}, anima::TextureEncoding::linear};
        atlas.surface = {image, {}, anima::TextureEncoding::linear};
        renderer.set_texture_mip_skip(skip);
        return upload(anima::Mesh::compile_impostor(atlas));
    };
    // Frames of 72 texels, 9 times 8: 3 levels leave frames of 9 whole texels, and a 4th would split texels between
    // frames.
    const auto split_2 = impostor(8, 72, 2), split_3 = impostor(8, 72, 3), split_4 = impostor(8, 72, 4),
               split_all = impostor(8, 72, 100);
    // Frames of 64 texels: 4 levels leave frames of 4 texels, and a 5th would leave 2.
    const auto narrow_3 = impostor(16, 64, 3), narrow_4 = impostor(16, 64, 4), narrow_5 = impostor(16, 64, 5);
    harness.finish();

    std::cout << "TEXTURE MIP SKIP device bytes of each mesh's images, the " << edge << "x" << edge
              << " RGBA8 texture with mips and the 1x1 fallback: skip 0 " << none << ", 1 " << one << ", 2 " << two
              << ", all but the last " << all << "; unmipmapped at 0, 1 and all " << unmipmapped_none << ", "
              << unmipmapped_one << ", " << unmipmapped_all << "; impostors of 72-texel frames at 2, 3, 4 and all "
              << split_2 << ", " << split_3 << ", " << split_4 << ", " << split_all
              << ", of 64-texel frames at 3, 4 and 5 " << narrow_3 << ", " << narrow_4 << ", " << narrow_5 << '\n';
    require_quartered(none, one, "The first level of a mipmapped texture");
    require_quartered(one, two, "The second level of a mipmapped texture");
    require(all < two, "Dropping every level but the last must leave less than dropping 2");
    require(unmipmapped_one == unmipmapped_none && unmipmapped_all == unmipmapped_none,
            "An unmipmapped texture must upload whole whatever the skip");
    require(split_3 < split_2 && split_4 == split_3 && split_all == split_3,
            "An impostor atlas must drop levels only while its frames stay whole numbers of texels across");
    require(narrow_4 < narrow_3 && narrow_5 == narrow_4,
            "An impostor atlas must drop levels only while its frames stay at least 4 texels across");
    // The quad minifies the ramp past its first level, so dropping it leaves the levels that the quad reads.
    harness.images.require_foreground("skip-0", "The textured quad is not visible");
    harness.images.require_parity("skip-0", "skip-1");
    harness.images.require_changed("skip-0", "skip-all", .2, "The last level alone must draw one color");
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --texture-memory OUTPUT");
    const std::filesystem::path output = argv[2];
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    constexpr std::size_t texel_bytes = std::size_t{edge} * edge * 4;
    // Two images with the same texels: one compiled into a Mesh that keeps it, one into a Mesh that holds it until
    // upload. The application keeps neither.
    std::weak_ptr<const anima::Image> kept_image, released_image;
    std::shared_ptr<const anima::Mesh> kept, released;
    {
        const auto kept_source = ramp();
        const auto released_source = std::make_shared<const anima::Image>(*kept_source);
        kept_image = kept_source;
        released_image = released_source;
        kept = anima::Mesh::compile(*textured_quad(kept_source));
        released = anima::Mesh::compile(*textured_quad(released_source),
                                        {.texel_retention = anima::TexelRetention::until_upload});
    }
    const auto kept_before = held_bytes({&kept_image}), released_before = held_bytes({&released_image});
    require(kept_before == texel_bytes && released_before == texel_bytes,
            "Each mesh must hold its image's texels until it is uploaded");
    require(released->description()->textures[0].image->rgba.empty(),
            "A mesh compiled until upload describes its texture without texels");
    const auto kept_scene = scene_of(kept), released_scene = scene_of(released);

    gpu_check::Image kept_frame;
    {
        Harness harness(output);
        harness.select({kept_scene});
        const auto kept_device = harness.renderer().resource_stats().resident_texture_bytes;
        harness.render("kept");
        harness.select({released_scene});
        const auto both_device = harness.renderer().resource_stats().resident_texture_bytes;
        const auto kept_after = held_bytes({&kept_image}), released_after = held_bytes({&released_image});
        std::cout << "TEXTURE MEMORY " << edge << "x" << edge << " RGBA8 texture: CPU texel bytes before upload: keep "
                  << kept_before << ", until_upload " << released_before << "; after upload: keep " << kept_after
                  << ", until_upload " << released_after << "; device bytes of each mesh's images, with mips and "
                  << "the 1x1 fallback: keep " << kept_device << ", until_upload " << both_device - kept_device << '\n';
        require(kept_after == texel_bytes, "A mesh that keeps its texels must still hold them after upload");
        require(released_after == 0 && released_image.expired(),
                "A mesh compiled until upload must let its texels go once uploaded");
        require(both_device - kept_device == kept_device, "Both meshes must upload the same device images");
        // The device images are complete: the mesh whose texels are gone draws exactly as the one that kept them.
        harness.render("released");
        harness.images.require_foreground("kept", "The textured quad is not visible");
        harness.images.require_same("kept", "released", "Releasing texels changed the drawn texture");
        // A preparation reads the texels while the mesh still holds them, and its upload lets them go too.
        const auto late =
            anima::Mesh::compile(*textured_quad(ramp()), {.texel_retention = anima::TexelRetention::until_upload});
        const std::weak_ptr<const anima::Image> late_image = late->texel_images().at(0);
        const anima::MeshPreparation prepared(late);
        harness.renderer().prepare_mesh(prepared);
        require(late_image.expired(), "A prepared upload must also let the texels go");
        // A failed upload keeps the texels, so the retry that the failure allows can read them.
        const auto retried =
            anima::Mesh::compile(*textured_quad(ramp()), {.texel_retention = anima::TexelRetention::until_upload});
        const std::weak_ptr<const anima::Image> retried_image = retried->texel_images().at(0);
        rejects<anima::InjectedRendererFailure>(
            [&] { harness.renderer().prepare_meshes(std::span(&retried, 1), {late_failure}); }, late_failure_message);
        require(!retried_image.expired(), "A failed upload must keep the texels");
        harness.renderer().prepare_meshes(std::span(&retried, 1));
        require(retried_image.expired(), "The retried upload must let the texels go");
        kept_frame = harness.images["kept"];
        check_index_widths(harness);
        harness.finish();
    }

    check_mip_skip(output);

    // Another renderer, as after a RendererFatalError, needs the texels again, and they are gone.
    Harness second(output);
    second.images.add("kept", std::move(kept_frame));
    rejects<std::logic_error>([&] { second.select({released_scene}); }, released_message);
    rejects<std::logic_error>([&] { second.renderer().prepare_meshes(std::span(&released, 1)); }, released_message);
    rejects<std::logic_error>([&] { (void)anima::MeshPreparation(released); }, released_message);
    // A mesh that becomes visible after selection is uploaded by draw(), which reports the same failure.
    const auto hidden = std::make_shared<anima::Scene>();
    auto hidden_renderer = hidden->create({}, released).renderer();
    hidden_renderer.set_visible(false);
    second.select({hidden});
    hidden_renderer.set_visible(true);
    rejects<anima::SceneResourceError>([&] { (void)second.renderer().draw(); }, released_message);
    // Texels that the application still holds stay readable, so another renderer can upload them again.
    const auto application_image = ramp();
    const auto shared = anima::Mesh::compile(*textured_quad(application_image),
                                             {.texel_retention = anima::TexelRetention::until_upload});
    second.select({scene_of(shared)});
    require(application_image.use_count() == 1, "The mesh must not hold an image that the application keeps");
    second.render("shared");
    second.images.require_same("kept", "shared", "An image that the application keeps drew differently");
    // Custom materials hold their textures' texels until upload in the same way.
    auto definition = custom_material_test::effect_definition("effect", anima::CustomBlend::opaque, {{1, 1, 1}});
    definition.texel_retention = anima::TexelRetention::until_upload;
    const std::weak_ptr<const anima::Image> effect_image = definition.textures.at(0).image;
    const auto material = std::make_shared<const anima::CustomMaterial>(std::move(definition));
    require(!effect_image.expired(), "A custom material must hold its texture until upload");
    const auto effect_scene = scene_of(blending_test::facing(blending_test::opaque({1, 1, 1}), {0, 0, -3}, 1, 1));
    effect_scene->object(effect_scene->instances().front()).renderer().set_custom_material(0, material);
    second.select({effect_scene});
    require(effect_image.expired(), "A custom material compiled until upload must let its texels go once uploaded");
    second.render("effect");
    second.images.require_foreground("effect", "The custom material is not visible");
    second.finish();
    std::cout << "PASS texture memory: meshes and custom materials compiled until upload let their texels go once "
                 "uploaded, keep drawing, and report their release when uploaded again; mipmapped textures drop "
                 "the top levels that the renderer asks for, impostor atlases within their frames; meshes of up to "
                 "65,536 vertices upload 16-bit indices and larger ones 32-bit, and both draw the same\n";
    return 0;
}
} // namespace texture_memory_test
