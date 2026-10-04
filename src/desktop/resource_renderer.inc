// Included inside VulkanRenderer::Impl. No backend types enter the public scene API.
struct GpuAsset {
    std::shared_ptr<const Mesh> asset;
    std::unique_ptr<ResourceBuffer> vertices, indices;
    // The width of each index in the index buffer: 16 bits for a mesh with at most 65,536 vertices, otherwise 32.
    VkIndexType index_type = VK_INDEX_TYPE_UINT32;
    std::unique_ptr<GpuMaterials> materials;
};
// How the matrices that place a draw orient its triangles, by the sign of the determinant of their first three
// columns, which the resource vertex shader passes to mesh.frag: keeping the winding of the faces that the mesh turns
// outward (positive), reversing it (negative), or varying, for matrices of both signs or one whose determinant lies so
// near zero that rounding on the GPU may give it either sign.
enum class Orientation : std::uint8_t { kept, reversed, varies };
// The transforms of one MeshPlacements on the GPU, uploaded once and cached per MeshPlacements object as meshes are:
// rows 0 to 2 of each affine matrix, which the vertex shaders read as per-instance attributes.
struct GpuPlacements {
    std::shared_ptr<const MeshPlacements> source;
    std::unique_ptr<ResourceBuffer> rows;
    // The orientation of each cluster's placements, by MeshPlacements::clusters() index.
    std::vector<Orientation> orientations;
};
struct ResourceInstance {
    std::shared_ptr<GpuAsset> gpu;
    const Scene::Instance *source{};
    // Where the instance's matrices start in the pose buffer: its palette, or with placements the rest pose's node
    // matrices, which object_offset's world matrix and each placement place.
    std::uint32_t palette_offset{};
    // Ranges of resource_draws, the instance's opaque and masked main-view draws: the first opaque_draw_count those
    // whose view_pipeline() cannot discard, then those whose pipeline may; and of resource_shadow_draws, its casters:
    // the first opaque_shadow_count opaque, then the masked ones. Each part keeps the order of Mesh::draws().
    std::size_t first_draw{}, draw_count{}, opaque_draw_count{}, first_shadow{}, shadow_count{}, opaque_shadow_count{};
    // With placements, their GPU rows. With placements or a visibility range, the pose buffer holds the object's world
    // matrix at object_offset, then a matrix whose first column is the range, as range_columns() encodes it, and whose
    // second holds the center of the mesh's rest bounds.
    std::shared_ptr<GpuPlacements> placements;
    std::uint32_t object_offset{};
    bool ranged{};
    // Whether the mesh is an impostor (Mesh::impostor()), whose draws record in MeshGroup::impostors.
    bool impostor{};
    // Whether the main view's MeshGroup::opaque left runs of the first opaque_draw_count draws to MeshGroup::masked:
    // runs of placement clusters that may lie in a margin of the visibility range, or whose orientation varies under a
    // single-sided draw. Preparation clears it, and record_resources() sets it as it records the opaque group.
    bool deferred_runs{};
    // Without placements, whether resource_draw_levels holds, from first_level, the level of each of its Mesh::draws()
    // that the view chose; it does when the frame records a draw with levels while the LOD threshold is above 0.
    bool chosen_levels{};
    std::size_t first_level{};
    // With placements, where the world bounds of its MeshPlacements::clusters() start in resource_cluster_bounds.
    std::size_t first_cluster{};
};
// Placements, in MeshPlacements::transforms() order, that one draw call copies: one visible cluster's, which recording
// merges with the next when they draw the same level of detail; a renderer without placements draws one run of its
// single copy. With placements, the run also holds the distance from the eye to its cluster's world bounds and the
// largest axis scale that places its copies, by which levels of detail are chosen, and the orientation of the
// cluster's placements and whether a copy may lie in a margin of the object's visibility range, which choose the
// pipeline of its copies in the main view (run_pipeline()).
struct PlacementRun {
    std::uint32_t first{}, count{};
    double distance{}, scale{};
    Orientation orientation = Orientation::kept;
    bool dissolving{};
};
// The main view's pipelines for opaque and masked mesh draws. mesh.frag discards the faces that a single-sided material
// turns away from the eye, where gl_FrontFacing times the orientation is negative, the pixels that a visibility range's
// margin dissolves and those that a masked material cuts out. A GPU that removes hidden surfaces before shading them,
// as Apple's do, cannot treat the fragments of a shader that may discard as opaque, so the cull pipelines compile
// without the discards (mesh.frag's mayDiscard). They cull the same faces in the rasterizer, whose facing is the one
// that gl_FrontFacing reports: none for a double-sided material, back faces where every matrix that places a
// single-sided draw keeps its winding, and front faces where every one reverses it. The discarding pipeline draws
// masked materials, copies that may lie in a margin of their visibility range, and single-sided draws whose
// orientation varies, as a skinned draw's may from one triangle to the next.
enum class MeshPipeline : std::uint8_t { cull_none, cull_back, cull_front, discarding };
static constexpr std::size_t mesh_pipeline_count = 4;
// The rasterizer's culling in each MeshPipeline.
static constexpr std::array<VkCullModeFlags, mesh_pipeline_count> mesh_pipeline_culling{
    VK_CULL_MODE_NONE, VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_NONE};
// The groups in which record_resources() records a pass's meshes, in this order. In the main view: the draw calls of
// meshes other than impostors through the pipelines that cannot discard, then those through the discarding pipeline,
// then impostors, whose fragment shader may discard and writes depth. The hidden surface removal of Apple GPUs rejects
// whatever lies beneath an opaque fragment before shading it, which it cannot do beneath a fragment of a shader that
// may discard or writes depth until that shader has run, and Apple advises drawing the opaque fragments first. In a
// shadow pass: the opaque casters other than impostors, through a pipeline without a fragment shader, then the masked
// ones, then impostors.
enum class MeshGroup : std::uint8_t { opaque, masked, impostors };
// How record_indexed() records a main-view draw of an opaque or masked mesh other than an impostor.
struct ViewRouting {
    // The draw's view_pipeline(), of which each run draws through its run_pipeline().
    MeshPipeline pipeline = MeshPipeline::discarding;
    // The runs that record: in MeshGroup::opaque those whose pipeline cannot discard, in MeshGroup::masked the others.
    MeshGroup group = MeshGroup::opaque;
};
struct GpuCustomMaterial;
// One shadow pass's view and frustum (shadow_renderer.inc).
struct ShadowPass;
// A main-view draw that composites over the target instead of replacing it, recorded after every opaque and
// masked draw in the order sort_back_to_front() gives: a blended material's, or a blended or additive custom
// material's.
struct BlendedDraw {
    // Index into resource_instances.
    std::size_t instance{};
    // Index into the instance's Mesh::draws().
    std::size_t draw{};
    // blend_order_key() of the draw's world bounds; larger is farther.
    double key{};
    // The custom material that draws it, or null for the draw's Material.
    const GpuCustomMaterial *custom{};
};
// A draw of an opaque custom material in the main view, or of a custom material's depth-only variant.
struct CustomDraw {
    // Index into resource_instances.
    std::size_t instance{};
    // Index into the instance's Mesh::draws().
    std::size_t draw{};
    const GpuCustomMaterial *custom{};
};
// resource_pipeline_layout's push constants, which its vertex and fragment stages share, in the two parts that
// record_draw() pushes: the pass's view and origin at offset 0, once per pass, then each draw's surface, indices and
// factor at offset sizeof(ResourceViewPush).
struct ResourceViewPush {
    Mat4 view;
    std::array<float, 4> origin;
};
struct ResourceDrawPush {
    std::array<float, 4> surface;
    std::array<std::uint32_t, 4> indices;
    std::array<float, 4> factor;
};
static constexpr std::uint32_t resource_push_bytes = sizeof(ResourceViewPush) + sizeof(ResourceDrawPush);
static_assert(sizeof(ResourceViewPush) == 80 && resource_push_bytes == 128 &&
              sizeof(ResourceViewPush) + offsetof(ResourceDrawPush, indices) == 96 &&
              sizeof(ResourceViewPush) + offsetof(ResourceDrawPush, factor) == 112 && sizeof(Mat4) == 64);
static_assert(sizeof(SourceVertex) == 96 && offsetof(SourceVertex, joints) == 44 &&
              offsetof(SourceVertex, weights) == 60);
// Bytes of one placement's rows on the GPU.
static constexpr std::uint32_t placement_stride = 3 * 4 * sizeof(float);
// Per-instance attributes from binding 1: rows 0 to 2 of a placement's affine matrix.
static constexpr auto placement_attributes = std::to_array<VkVertexInputAttributeDescription>({
    {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
    {9, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
    {10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
});
static constexpr std::array<VkVertexInputBindingDescription, 2> resource_bindings{{
    {0, sizeof(SourceVertex), VK_VERTEX_INPUT_RATE_VERTEX},
    {1, placement_stride, VK_VERTEX_INPUT_RATE_INSTANCE},
}};
// SourceVertex attributes that the resource vertex shader reads, by location.
static constexpr auto resource_attributes = std::to_array<VkVertexInputAttributeDescription>({
    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SourceVertex, position)},
    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SourceVertex, normal)},
    {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SourceVertex, color)},
    {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SourceVertex, uv)},
    {4, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(SourceVertex, joints)},
    {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SourceVertex, weights)},
    {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SourceVertex, tangent)},
    {7, 0, VK_FORMAT_R32_SFLOAT, offsetof(SourceVertex, alpha)},
});
VkShaderModule resource_vertex_shader{};
VkPipelineLayout resource_pipeline_layout{};
VkDescriptorSetLayout pose_layout{};
// The view's mesh pipelines, indexed by whether they compile the height fog's code (uses_height_fog()): the opaque and
// masked draws' by MeshPipeline, and the blended draws'.
std::array<std::array<VkPipeline, 2>, mesh_pipeline_count> resource_pipelines{};
std::array<VkPipeline, 2> blended_resource_pipelines{};
// The mesh pipeline that record_indexed() bound last in the main view, which record_resources() clears before its
// meshes, the first draws that the main view records, so that it stays bound until the impostors bind theirs.
VkPipeline bound_mesh_pipeline{};
// What record_draw() has bound and pushed through resource_pipeline_layout, so that within a pass it binds the frame's
// sets and pushes the view once, and binds a material set only where it changes. Every pipeline of the mesh groups and
// the blended draws shares that layout, so what one draw binds and pushes serves the next through any of them.
// record_resources() and record_blended() clear it before their first draw. bind_custom() and record_sky() clear it
// too: they bind sets through layouts that are not compatible with resource_pipeline_layout for set 0
// (custom_pipeline_layout's set 0 differs, and environment_pipeline_layout has no push constant range), which disturbs
// every set bound through it, and custom draws push their own constants through custom_pipeline_layout.
struct ResourceBound {
    // Whether the frame's sets and the pass's ResourceViewPush are bound: the pose set at set 2 and, in the main view,
    // the environment set at set 1, which no shadow pipeline reads.
    bool frame{};
    // The material set bound at set 0, or null for none.
    VkDescriptorSet material{};
};
ResourceBound resource_bound;
// Impostor meshes (Mesh::impostor()) draw through their own shaders, in the view and in the shadow passes.
VkShaderModule impostor_vertex_shader{}, impostor_fragment_shader{}, impostor_shadow_fragment_shader{};
std::array<VkPipeline, 2> impostor_pipelines{};
VkPipeline shadow_impostor_pipeline{};
// Holds each frame slot's pose set (FrameSlot::pose_set).
VkDescriptorPool pose_pool{};
std::vector<std::shared_ptr<const Scene>> resource_scenes;
std::map<const Mesh *, std::shared_ptr<GpuAsset>> resource_cache;
std::map<const MeshPlacements *, std::shared_ptr<GpuPlacements>> placement_cache;
// One identity placement, bound at binding 1 for instances without placements.
std::unique_ptr<ResourceBuffer> identity_placement;
// The runs that the instance being recorded draws in the current pass.
std::vector<PlacementRun> placement_runs;
std::vector<ResourceInstance> resource_instances;
// The levels of detail that the view chose for the draws of instances without placements, by
// ResourceInstance::first_level plus the draw's index into Mesh::draws(): null for the full draw. Every pass that
// records a draw this frame reads its level here, so the shadow passes draw the level that the view chose.
std::vector<const DrawLevel *> resource_draw_levels;
// The world bounds of the clusters of instances with placements, by ResourceInstance::first_cluster plus the cluster's
// index into MeshPlacements::clusters(), computed once per frame for every pass's find_placement_runs().
std::vector<RenderBounds> resource_cluster_bounds;
// The opaque and masked draws of the main view: each one's index into its instance's Mesh::draws() and its
// view_pipeline().
struct ViewDraw {
    std::size_t draw{};
    MeshPipeline pipeline = MeshPipeline::discarding;
};
std::vector<ViewDraw> resource_draws;
// An object with main-view draws in resource_draws: its index into resource_instances and its view_order_key().
struct ViewObject {
    double key{};
    std::size_t instance{};
};
// The objects with main-view draws in resource_draws, nearest first, and at equal keys in selection and instance order.
std::vector<ViewObject> resource_view_order;
// Indices into Mesh::draws() of the shadow casters.
std::vector<std::size_t> resource_shadow_draws;
std::vector<BlendedDraw> resource_blended_draws;
std::vector<CustomDraw> resource_custom_draws, resource_custom_shadow_draws;
// Whether a blended or additive custom material in resource_blended_draws reads opaque depth or color.
bool resource_opaque_inputs{};
RenderFrustum resource_frustum;
ResourceStats resources{};

// The distance by which blended draws sort, from the center of @p bounds: squared from the eye in a perspective
// view, or along the view direction in an orthographic one, where view_origin holds the unit direction toward the
// camera. Either way a larger key is farther. Doubles keep the squares of finite float coordinates finite.
double blend_order_key(const RenderBounds &bounds) const {
    const std::array<double, 3> center{(double(bounds.minimum.x) + bounds.maximum.x) / 2,
                                       (double(bounds.minimum.y) + bounds.maximum.y) / 2,
                                       (double(bounds.minimum.z) + bounds.maximum.z) / 2};
    double key = 0;
    if (view_origin[3] != 0)
        for (std::size_t axis = 0; axis < center.size(); ++axis) {
            const auto offset = center[axis] - view_origin[axis];
            key += offset * offset;
        }
    else
        for (std::size_t axis = 0; axis < center.size(); ++axis)
            key -= center[axis] * view_origin[axis];
    return key;
}
// Orders @p draws back to front. The sort is stable, so draws with equal keys keep selection, instance and draw
// order, and their order cannot change from one frame to the next.
static void sort_back_to_front(std::vector<BlendedDraw> &draws) {
    std::stable_sort(draws.begin(), draws.end(),
                     [](const BlendedDraw &a, const BlendedDraw &b) { return a.key > b.key; });
}
// The distance by which the main view orders the objects whose opaque and masked meshes it draws, nearest first: to
// the nearest point of @p bounds, from the eye in a perspective view, which is 0 for bounds that hold the eye, or along
// the view direction in an orthographic one, where view_origin holds the unit direction toward the camera and the
// distance is measured from the plane through the world origin, so that it may be negative. Bounds that overflowed to
// infinity can leave the distance undefined (NaN), and such a key orders last.
double view_order_key(const RenderBounds &bounds) const {
    double key = 0;
    if (view_origin[3] != 0)
        key = nearest_distance(eye(), bounds.minimum, bounds.maximum);
    else
        for (const auto [lo, hi, toward] : {std::array{bounds.minimum.x, bounds.maximum.x, view_origin[0]},
                                            std::array{bounds.minimum.y, bounds.maximum.y, view_origin[1]},
                                            std::array{bounds.minimum.z, bounds.maximum.z, view_origin[2]}})
            key -= std::max(double(lo) * toward, double(hi) * toward);
    return std::isnan(key) ? std::numeric_limits<double>::infinity() : key;
}
// Orders @p objects nearest first. Objects with equal keys keep selection and instance order, as a stable sort would,
// so that their order does not flicker from one frame to the next; comparing instance indices gives that order without
// the buffer that std::stable_sort allocates.
static void sort_front_to_back(std::vector<ViewObject> &objects) {
    std::sort(objects.begin(), objects.end(), [](const ViewObject &a, const ViewObject &b) {
        return std::tie(a.key, a.instance) < std::tie(b.key, b.instance);
    });
}

// Creates a buffer of @p bytes for @p usage in memory with the @p required properties, preferring @p preferred ones as
// create_buffer does. Buffers that the host writes and shaders read prefer device-local memory, which on a discrete GPU
// is the host-visible window into its own memory where it has one; VMA falls back to system memory when no such type
// exists or its heap cannot hold the buffer. Staging buffers, which only a copy reads, leave @p preferred empty.
std::unique_ptr<ResourceBuffer> resource_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                                                VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) {
    auto value = std::make_unique<ResourceBuffer>();
    value->allocator = allocator;
    value->bytes = bytes;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto allocated =
        create_buffer(info, required, value->buffer, value->allocation, "Create resource buffer", preferred);
    value->allocation_bytes = allocated.size;
    value->mapping = allocated.pMappedData;
    return value;
}
void flush_resource(ResourceBuffer &buffer) {
    check(vmaFlushAllocation(allocator, buffer.allocation, 0, VK_WHOLE_SIZE), "Flush resource buffer");
}
void upload_resource_buffer(ResourceBuffer &destination, const void *data, VkAccessFlags access) {
    auto staging =
        resource_buffer(destination.bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    std::memcpy(staging->mapping, data, static_cast<std::size_t>(destination.bytes));
    flush_resource(*staging);
    // UploadBatch retires submitted work before staging or the candidate can unwind.
    UploadBatch upload{device, allocator, &fatal, &errors, graphics_family};
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(upload.command, &begin), "Begin geometry upload");
    const VkBufferCopy copy{0, 0, destination.bytes};
    vkCmdCopyBuffer(upload.command, staging->buffer, destination.buffer, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = access;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = destination.buffer;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(upload.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0,
                         nullptr, 1, &barrier, 0, nullptr);
    check(vkEndCommandBuffer(upload.command), "End geometry upload");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &upload.command;
    check(vkQueueSubmit(graphics_queue, 1, &submit, upload.fence), "Submit geometry upload");
    upload.pending = true;
    upload.wait();
}
void create_resource_layout() {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.limits.maxPushConstantsSize < resource_push_bytes ||
        properties.limits.maxPerStageDescriptorStorageBuffers < 1 || properties.limits.maxBoundDescriptorSets < 3 ||
        properties.limits.maxVertexInputAttributes < resource_attributes.size() + placement_attributes.size() ||
        properties.limits.maxVertexInputBindings < resource_bindings.size() ||
        properties.limits.maxVertexInputBindingStride < sizeof(SourceVertex))
        throw std::runtime_error("Device lacks resource renderer interface limits");
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader.codeSize = sizeof(resource_vertex_code);
    shader.pCode = resource_vertex_code;
    check(vkCreateShaderModule(device, &shader, nullptr, &resource_vertex_shader), "Create resource vertex shader");
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                                               nullptr};
    VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor.bindingCount = 1;
    descriptor.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device, &descriptor, nullptr, &pose_layout), "Create pose layout");
    const VkDescriptorSetLayout layouts[]{texture_layout, environment_layout, pose_layout};
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, resource_push_bytes};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 3;
    info.pSetLayouts = layouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device, &info, nullptr, &resource_pipeline_layout), "Create resource pipeline layout");
    const auto slots = static_cast<std::uint32_t>(frames.size());
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slots};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = slots;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    check(vkCreateDescriptorPool(device, &pool, nullptr, &pose_pool), "Create pose descriptor pool");
    // Each slot's set points at its pose buffer from the first preparation that writes palettes into it.
    for (auto &slot : frames) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pose_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &pose_layout;
        check(vkAllocateDescriptorSets(device, &allocate, &slot.pose_set), "Allocate pose descriptor");
    }
    const std::array<float, 12> identity_rows{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    identity_placement =
        resource_buffer(placement_stride, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    upload_resource_buffer(*identity_placement, identity_rows.data(), VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
}
// Orientation counts as varying where the determinant's magnitude is at most this share of the product of the lengths
// of the three columns. The resource vertex shader computes the determinant of the same float columns as a dot and a
// cross product, which rounding moves by a few units in the last place of that product, a small fraction of this.
static constexpr double orientation_tolerance = 1e-4;
// How @p m orients the triangles that it places.
static Orientation orientation(const Mat4 &m) {
    const std::array<double, 3> x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    const auto length_of = [](const std::array<double, 3> &v) { return std::hypot(v[0], v[1], v[2]); };
    const double determinant =
        x[0] * (y[1] * z[2] - y[2] * z[1]) + x[1] * (y[2] * z[0] - y[0] * z[2]) + x[2] * (y[0] * z[1] - y[1] * z[0]);
    const double margin = orientation_tolerance * length_of(x) * length_of(y) * length_of(z);
    // A nonfinite matrix compares false both ways.
    return determinant > margin    ? Orientation::kept
           : determinant < -margin ? Orientation::reversed
                                   : Orientation::varies;
}
// The orientation of copies that a matrix of orientation @p outer places through one of orientation @p inner.
static Orientation compose(Orientation outer, Orientation inner) {
    if (outer == Orientation::varies || inner == Orientation::varies)
        return Orientation::varies;
    return outer == inner ? Orientation::kept : Orientation::reversed;
}
// The pipeline of an opaque single-sided draw whose placing matrices orient its triangles as @p placed.
static MeshPipeline culling(Orientation placed) {
    return placed == Orientation::kept       ? MeshPipeline::cull_back
           : placed == Orientation::reversed ? MeshPipeline::cull_front
                                             : MeshPipeline::discarding;
}
// The main-view pipeline of draw @p draw of @p source, whose material is @p material or, when null, the default
// Material, where @p dissolving says whether the object may lie in a margin of its visibility range. With placements it
// is the pipeline for copies whose placements keep their winding and lie outside the range's margins; run_pipeline()
// gives each cluster's.
static MeshPipeline view_pipeline(const Scene::Instance &source, const IndexedDraw &draw, const Material *material,
                                  bool dissolving) {
    if (dissolving || (material && material->alpha_mode == AlphaMode::mask))
        return MeshPipeline::discarding;
    if (!material || material->double_sided)
        return MeshPipeline::cull_none;
    // The first vertex's blend of joint matrices orients each skinned triangle, which no matrix of the palette decides.
    if (draw.skinned)
        return MeshPipeline::discarding;
    // A copy's matrix composes the object's world matrix, its placement and its node's rest matrix, as resource.vert
    // composes them.
    if (source.placements)
        return culling(
            compose(orientation(source.world), orientation(source.asset->rest_pose().world.at(draw.palette_offset))));
    return culling(orientation(source.palette.at(draw.palette_offset)));
}
// The pipeline that draws the copies in @p run of a draw whose view_pipeline() is @p draw_pipeline: the discarding one
// where they may lie in a margin of the visibility range, and for a single-sided draw culled by facing, the culling of
// its placing matrices' orientation composed with that of the run's placements.
static MeshPipeline run_pipeline(MeshPipeline draw_pipeline, const PlacementRun &run) {
    if (run.dissolving)
        return MeshPipeline::discarding;
    if (draw_pipeline != MeshPipeline::cull_back && draw_pipeline != MeshPipeline::cull_front)
        return draw_pipeline;
    return culling(
        compose(draw_pipeline == MeshPipeline::cull_back ? Orientation::kept : Orientation::reversed, run.orientation));
}
// The GPU rows of @p placements, uploading them on first use, with the orientation of each cluster's placements.
std::shared_ptr<GpuPlacements> gpu_placements(const std::shared_ptr<const MeshPlacements> &placements) {
    if (auto found = placement_cache.find(placements.get()); found != placement_cache.end())
        return found->second;
    const auto transforms = placements->transforms();
    std::vector<float> rows;
    rows.reserve(transforms.size() * 12);
    for (const auto &m : transforms)
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned column = 0; column < 4; ++column)
                rows.push_back(m[column * 4 + row]);
    auto value = std::make_shared<GpuPlacements>();
    value->source = placements;
    value->rows = resource_buffer(rows.size() * sizeof(float),
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    upload_resource_buffer(*value->rows, rows.data(), VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
    value->orientations.reserve(placements->clusters().size());
    for (const auto &cluster : placements->clusters()) {
        auto shared = orientation(transforms[cluster.first]);
        for (auto i = cluster.first + 1; i < cluster.first + cluster.count && shared != Orientation::varies; ++i)
            if (orientation(transforms[i]) != shared)
                shared = Orientation::varies;
        value->orientations.push_back(shared);
    }
    placement_cache.emplace(placements.get(), value);
    return value;
}
// The distance from @p eye to the nearest point of the box from @p minimum to @p maximum, 0 inside it.
static double nearest_distance(Vec3 eye, Vec3 minimum, Vec3 maximum) {
    double squared = 0;
    for (const auto [lo, hi, at] : {std::array{minimum.x, maximum.x, eye.x}, std::array{minimum.y, maximum.y, eye.y},
                                    std::array{minimum.z, maximum.z, eye.z}}) {
        const double gap = std::max({double(lo) - at, double(at) - hi, 0.0});
        squared += gap * gap;
    }
    return std::sqrt(squared);
}
// The length of the longest of the first three columns of @p m: the largest scale it applies along an axis.
static double axis_scale(const Mat4 &m) {
    return std::max({double(length(axis_x(m))), double(length(axis_y(m))), double(length(axis_z(m)))});
}
// Half the diagonal of @p bounds, the radius of the sphere around them, which a shadow pass compares with its least
// caster radius (ShadowPass::min_caster_radius).
static double bounds_radius(const RenderBounds &bounds) {
    const double x = double(bounds.maximum.x) - bounds.minimum.x, y = double(bounds.maximum.y) - bounds.minimum.y,
                 z = double(bounds.maximum.z) - bounds.minimum.z;
    return std::sqrt(x * x + y * y + z * z) / 2;
}
// The radius of a copy of @p render_instance, an object with placements, under a placement whose largest axis scale is
// 1: half the diagonal of its mesh's rest bounds times the largest axis scale of its world matrix. Each cluster's
// copies reach at most that times MeshPlacements::Cluster::scale, for matrices of translation, rotation and scale.
static double copy_radius(const Scene::Instance &render_instance) {
    return bounds_radius(render_instance.asset->rest_bounds()) * axis_scale(render_instance.world);
}
// The distances from @p eye to the nearest and to the farthest point of the box from @p minimum to @p maximum.
static std::pair<double, double> distance_span(Vec3 eye, Vec3 minimum, Vec3 maximum) {
    double nearest = 0, farthest = 0;
    for (const auto [lo, hi, at] : {std::array{minimum.x, maximum.x, eye.x}, std::array{minimum.y, maximum.y, eye.y},
                                    std::array{minimum.z, maximum.z, eye.z}}) {
        const double gap = std::max({double(lo) - at, double(at) - hi, 0.0});
        const double reach = std::max(std::abs(double(lo) - at), std::abs(double(hi) - at));
        nearest += gap * gap;
        farthest += reach * reach;
    }
    return {std::sqrt(nearest), std::sqrt(farthest)};
}
// Whether a box from @p minimum to @p maximum may hold a point at a distance from @p eye inside @p range.
static bool within_range(const VisibilityRange &range, Vec3 eye, Vec3 minimum, Vec3 maximum) {
    const auto [nearest, farthest] = distance_span(eye, minimum, maximum);
    return nearest < range.end && farthest >= range.begin;
}
// Whether a box from @p minimum to @p maximum may hold a point at a distance from @p eye strictly inside one of
// @p range's margins, where visibilityAt() gives a copy measured to that point a share of its pixels between 0 and 1.
// At a margin's edges a copy draws whole or not at all, and dissolved() keeps every pixel of a share above 15.5/16 and
// none below 0.5/16, so where rounding moves a distance across an edge by less than 1/32 of the margin, the shader's
// float distance and this test's double one draw the same pixels.
static bool may_dissolve(const VisibilityRange &range, Vec3 eye, Vec3 minimum, Vec3 maximum) {
    const auto [nearest, farthest] = distance_span(eye, minimum, maximum);
    const auto overlaps = [&](double from, double to) { return from < to && nearest < to && farthest > from; };
    return overlaps(range.begin, double(range.begin) + range.begin_margin) ||
           overlaps(double(range.end) - range.end_margin, range.end);
}
// The center of @p mesh's rest bounds, which visibility ranges measure to once an object or copy places it, as Godot
// measures to the center of an instance's bounds; the mesh's origin when it has no bounds.
static Vec3 range_center(const Mesh &mesh) {
    const auto &bounds = mesh.rest_bounds();
    return bounds.valid ? (bounds.minimum + bounds.maximum) * .5F : Vec3{};
}
// The pose buffer matrix after a ranged object's world matrix: the begin, begin margin, end and end margin of @p range,
// which scaled_range() has scaled, then @p center. Shaders may assume that no value is infinite, so an endless range,
// which validation leaves no end margin, ends at the largest float, which no distance reaches.
static Mat4 range_columns(const VisibilityRange &range, Vec3 center) {
    Mat4 columns{};
    columns[0] = range.begin;
    columns[1] = range.begin_margin;
    columns[2] = std::isinf(range.end) ? std::numeric_limits<float>::max() : range.end;
    columns[3] = range.end_margin;
    columns[4] = center.x;
    columns[5] = center.y;
    columns[6] = center.z;
    return columns;
}
// The world bounds of @p cluster's copies under @p world.
static RenderBounds cluster_bounds(const MeshPlacements::Cluster &cluster, const Mat4 &world) {
    RenderBounds bounds;
    const auto &b = cluster.bounds;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto v = point(world, {corner & 1 ? b.maximum.x : b.minimum.x, corner & 2 ? b.maximum.y : b.minimum.y,
                                     corner & 4 ? b.maximum.z : b.minimum.z});
        if (!bounds.valid)
            bounds = {v, v, true};
        bounds.minimum = {std::min(bounds.minimum.x, v.x), std::min(bounds.minimum.y, v.y),
                          std::min(bounds.minimum.z, v.z)};
        bounds.maximum = {std::max(bounds.maximum.x, v.x), std::max(bounds.maximum.y, v.y),
                          std::max(bounds.maximum.z, v.z)};
    }
    return bounds;
}
// Whether @p frustum, when given, contains @p bounds, and @p range, when given, can draw a copy at their distance from
// @p eye.
static bool cluster_visible(const RenderBounds &bounds, const RenderFrustum *frustum,
                            const std::optional<VisibilityRange> &range, Vec3 eye) {
    return (!frustum || frustum->intersects(bounds)) &&
           (!range || within_range(*range, eye, bounds.minimum, bounds.maximum));
}
// Pixels of the view's height per unit of error at distance 1, or at any distance in an orthographic view: the length
// of the view-projection's second row, which is the projection's vertical scale, times half the scene targets' height,
// so that the LOD threshold counts the pixels that the view renders.
double lod_projection() const {
    return std::sqrt(double(view_projection[1]) * view_projection[1] + double(view_projection[5]) * view_projection[5] +
                     double(view_projection[9]) * view_projection[9]) *
           scene_extent.height / 2;
}
// The coarsest level of @p draw whose error, scaled by @p scale and projected at @p distance from the eye, covers at
// most the LOD threshold in pixels; null for the full draw.
const DrawLevel *level_for(const IndexedDraw &draw, double distance, double scale) const {
    const bool perspective = view_origin[3] != 0;
    if (draw.levels.empty() || options.lod_threshold <= 0 || (perspective && distance <= 0))
        return nullptr;
    const double pixels = lod_projection() * scale / (perspective ? distance : 1);
    const DrawLevel *chosen = nullptr;
    for (const auto &level : draw.levels) {
        if (double(level.error) * pixels > options.lod_threshold)
            break;
        chosen = &level;
    }
    return chosen;
}
// The level of draw @p draw_index of @p render_instance, an object without placements, that the view chooses: by the
// distance to the draw's world bounds and the largest scale among the matrices that place it, its node's matrix or its
// skin's joint matrices, which may number hundreds.
const DrawLevel *object_level(const Scene::Instance &render_instance, std::size_t draw_index) const {
    const auto &draw = render_instance.asset->draws()[draw_index];
    const auto &bounds = render_instance.primitive_bounds[draw_index];
    double scale = 0;
    for (std::uint32_t j = 0; j < draw.palette_count; ++j)
        scale = std::max(scale, axis_scale(render_instance.palette[draw.palette_offset + j]));
    return level_for(draw, nearest_distance(eye(), bounds.minimum, bounds.maximum), scale);
}
// @p range with its begin, end and margins times the view distance scale (VulkanRenderer::set_view_distance_scale()),
// multiplied in double and rounded to float. A finite distance that the product carries past the largest float is
// clamped to it, so that no finite range becomes endless and an endless one stays endless; a scale that is a power of
// two scales every distance exactly, which keeps two ranges' margins exactly as complementary as they were.
VisibilityRange scaled_range(const VisibilityRange &range) const {
    const double scale = options.view_distance_scale;
    const auto scaled = [scale](float distance) {
        return std::isinf(distance)
                   ? distance
                   : float(std::min(double(distance) * scale, double(std::numeric_limits<float>::max())));
    };
    return {scaled(range.begin), scaled(range.end), scaled(range.begin_margin), scaled(range.end_margin)};
}
// The visibility range that culling applies to @p render_instance, scaled as scaled_range() does: none for the default
// range or an orthographic view.
std::optional<VisibilityRange> culling_range(const Scene::Instance &render_instance) const {
    if (render_instance.visibility_range == VisibilityRange{} || view_origin[3] == 0)
        return std::nullopt;
    return scaled_range(render_instance.visibility_range);
}
Vec3 eye() const { return {view_origin[0], view_origin[1], view_origin[2]}; }
// Fills placement_runs with one run for each of @p render_instance's placement clusters that @p frustum, when given,
// contains and its visibility range can draw, by the cluster bounds that prepare_resources() computed for the frame,
// and whose largest copy reaches @p min_caster_radius, a shadow pass's least caster radius (copy_radius());
// record_indexed() merges adjacent runs that draw the same level through the same pipeline. Without placements the
// single copy is one run, whose draws' view_pipeline() already holds its orientation and visibility range.
void find_placement_runs(const ResourceInstance &render_instance, const RenderFrustum *frustum,
                         double min_caster_radius = 0) {
    placement_runs.clear();
    if (!render_instance.placements) {
        placement_runs.push_back({0, 1});
        return;
    }
    const auto range = culling_range(*render_instance.source);
    const auto world_scale = axis_scale(render_instance.source->world);
    const auto unit_copy = min_caster_radius > 0 ? copy_radius(*render_instance.source) : 0;
    const auto clusters = render_instance.placements->source->clusters();
    for (std::size_t i = 0; i < clusters.size(); ++i) {
        const auto &cluster = clusters[i];
        const auto &bounds = resource_cluster_bounds[render_instance.first_cluster + i];
        if (unit_copy * cluster.scale < min_caster_radius)
            continue;
        if (cluster_visible(bounds, frustum, range, eye()))
            placement_runs.push_back({cluster.first, cluster.count,
                                      nearest_distance(eye(), bounds.minimum, bounds.maximum),
                                      world_scale * cluster.scale, render_instance.placements->orientations[i],
                                      range && may_dissolve(*range, eye(), bounds.minimum, bounds.maximum)});
    }
}
// Whether shadow pass @p shadow draws draw @p draw_index of @p render_instance, a caster: its world bounds, which cover
// every copy, reach the pass's frustum and, without placements, their radius reaches the pass's least caster radius.
// find_placement_runs() compares placed copies by cluster instead.
static bool shadow_holds(const ShadowPass &shadow, const ResourceInstance &render_instance, std::size_t draw_index) {
    const auto &bounds = render_instance.source->primitive_bounds[draw_index];
    return shadow.frustum.intersects(bounds) &&
           (render_instance.placements || bounds_radius(bounds) >= shadow.min_caster_radius);
}
std::shared_ptr<GpuAsset> gpu_asset(const std::shared_ptr<const Mesh> &asset, RendererFailureStage failure,
                                    const MeshPreparation *prepared = nullptr) {
    if (auto found = resource_cache.find(asset.get()); found != resource_cache.end())
        return found->second;
    auto value = std::make_shared<GpuAsset>();
    value->asset = asset;
    // Every index, those of the levels of detail included, is below the vertex count, so a mesh with at most 65,536
    // vertices uploads its indices at 16 bits, which halves their memory and fetch. Draws offset and count indices,
    // not bytes, and primitive restart is off, so 0xFFFF is an ordinary index and the draws are unchanged.
    const auto narrow = asset->vertices().size() <= std::size_t{std::numeric_limits<std::uint16_t>::max()} + 1;
    if (narrow)
        value->index_type = VK_INDEX_TYPE_UINT16;
    const auto vertices = asset->vertices().size_bytes(),
               indices = asset->indices().size() * (narrow ? sizeof(std::uint16_t) : sizeof(std::uint32_t));
    value->materials = std::make_unique<GpuMaterials>();
    value->materials->device = device;
    value->materials->allocator = allocator;
    value->materials->source = asset->materials();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (!asset->vertices().empty() && asset->vertices().size() - 1 > properties.limits.maxDrawIndexedIndexValue)
        throw std::invalid_argument("Resource index exceeds device indexed-draw range");
    for (const auto &texture : asset->materials()->textures)
        if (texture.image->width > properties.limits.maxImageDimension2D ||
            texture.image->height > properties.limits.maxImageDimension2D)
            throw std::invalid_argument("Resource texture exceeds device image dimensions");
    std::vector<std::shared_ptr<const anima::Image>> texels;
    if (vertices && indices) {
        // A preparation holds every image it uploads. Otherwise read the texels before allocating anything, since a
        // Mesh compiled with TexelRetention::until_upload may have let them go.
        if (!prepared)
            texels = asset->texel_images();
        value->vertices =
            resource_buffer(vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        upload_resource_buffer(*value->vertices, asset->vertices().data(), VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
        inject_scene(failure, RendererFailureStage::vertex, false);
        value->indices = resource_buffer(indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (narrow) {
            std::vector<std::uint16_t> narrowed(asset->indices().size());
            std::ranges::transform(asset->indices(), narrowed.begin(),
                                   [](std::uint32_t index) { return static_cast<std::uint16_t>(index); });
            upload_resource_buffer(*value->indices, narrowed.data(), VK_ACCESS_INDEX_READ_BIT);
        } else
            upload_resource_buffer(*value->indices, asset->indices().data(), VK_ACCESS_INDEX_READ_BIT);
        inject_scene(failure, RendererFailureStage::index, false);
        UploadBatch upload{device, allocator, &fatal, &errors, graphics_family};
        upload_textures(*value->materials, upload, failure, false, texels, prepared);
    }
    resource_cache.emplace(asset.get(), value);
    ++resources.mesh_uploads;
    resources.geometry_uploaded_bytes += vertices + indices;
    // Every copy has completed, so a Mesh compiled with TexelRetention::until_upload no longer needs its texels.
    asset->release_texels();
    return value;
}
void prepare_resources(std::span<const std::shared_ptr<const Scene>> sources, RendererFailureStage failure = {},
                       bool use_frustum = true) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::vector<Mat4> palettes;
    std::vector<ResourceInstance> instances;
    std::vector<ViewDraw> draws;
    // The current instance's draws through the discarding pipeline, which follow its other draws in draws.
    std::vector<ViewDraw> discarding_draws;
    std::vector<ViewObject> view_order;
    std::vector<std::size_t> shadows;
    // The current instance's masked casters, which follow its opaque ones in shadows.
    std::vector<std::size_t> masked_shadows;
    std::vector<BlendedDraw> blended;
    std::vector<CustomDraw> custom_draws, custom_shadows;
    std::vector<const DrawLevel *> draw_levels;
    std::vector<RenderBounds> placed_cluster_bounds;
    std::uint64_t candidate_instances = 0, culled_instances = 0, candidate_draws = 0, culled_draws = 0,
                  culled_clusters = 0, range_culled = 0;
    for (const auto &source : sources)
        for (auto id : source->instances()) {
            const auto &render_instance = source->instance(id);
            if (!render_instance.visible || !render_instance.active || render_instance.asset->draws().empty())
                continue;
            // An object outside its visibility range draws in no pass, and one in a margin dissolves; placed copies are
            // culled and dissolve by cluster instead.
            bool dissolving = false;
            if (const auto range = use_frustum ? culling_range(render_instance) : std::nullopt;
                range && !render_instance.placements) {
                const auto center = point(render_instance.world, range_center(*render_instance.asset));
                if (!within_range(*range, eye(), center, center)) {
                    ++range_culled;
                    continue;
                }
                dissolving = may_dissolve(*range, eye(), center, center);
            }
            const auto first_draw = draws.size(), first_shadow = shadows.size(), first_blended = blended.size(),
                       first_custom = custom_draws.size(), first_custom_shadow = custom_shadows.size(),
                       first_level = draw_levels.size();
            bool candidate = false;
            // The radius that a cascade with a least caster radius compares: a draw's own without placements, and with
            // them the object's largest copy, computed for the first draw that needs it, since a cascade that skips
            // the largest copy skips every cluster as its pass records them.
            double largest_copy = -1;
            const auto caster_radius = [&](const RenderBounds &bounds) {
                if (!render_instance.placements)
                    return bounds_radius(bounds);
                if (largest_copy < 0) {
                    float scale = 0;
                    for (const auto &cluster : render_instance.placements->clusters())
                        scale = std::max(scale, cluster.scale);
                    largest_copy = copy_radius(render_instance) * scale;
                }
                return largest_copy;
            };
            const auto source_draws = render_instance.asset->draws();
            const bool cull = use_frustum && options.frustum_culling;
            const bool outside = cull && !resource_frustum.intersects(render_instance.bounds);
            for (std::size_t i = 0; i < source_draws.size(); ++i) {
                if (!render_instance.primitive_visible[i] || !source_draws[i].index_count)
                    continue;
                candidate = true;
                ++candidate_draws;
                const auto material_index = source_draws[i].material;
                const auto *material =
                    material_index < 0 ? nullptr : &render_instance.asset->materials()->material_data[material_index];
                // A custom material in the draw's material slot replaces its Material in every pass.
                const auto *custom =
                    material_index < 0 ? nullptr : render_instance.custom_materials[material_index].get();
                const bool blend = custom ? custom->blend() != CustomBlend::opaque
                                          : material && material->alpha_mode == AlphaMode::blend;
                const auto &bounds = render_instance.primitive_bounds[i];
                const bool culled = outside || (cull && !resource_frustum.intersects(bounds));
                if (culled)
                    ++culled_draws;
                else if (blend) // Every nonempty draw has valid bounds.
                    blended.push_back(
                        {instances.size(), i, blend_order_key(bounds),
                         custom ? &gpu_custom(render_instance.custom_materials[material_index]) : nullptr});
                else if (custom)
                    custom_draws.push_back(
                        {instances.size(), i, &gpu_custom(render_instance.custom_materials[material_index])});
                else {
                    const ViewDraw drawn{i, view_pipeline(render_instance, source_draws[i], material, dissolving)};
                    (drawn.pipeline == MeshPipeline::discarding ? discarding_draws : draws).push_back(drawn);
                }
                // A depth-only shadow map holds neither unlit surfaces nor partial transmission; a custom material
                // casts through its depth-only variant.
                const bool casts = custom ? custom->casts_shadows() : !material || !(material->unlit || blend);
                bool cast = false;
                if (casts && render_instance.casts_shadows) {
                    cast = environment.detail_shadow.enabled && detail_pass.frustum.intersects(bounds);
                    // Every cascade that the draw may shadow extends its depth over it, unless it is too small for the
                    // cascade to draw.
                    for (std::uint32_t c = 0; c < cascade_fits.count; ++c)
                        if (const auto least = cascade_passes[c].min_caster_radius;
                            least <= 0 || caster_radius(bounds) >= least)
                            cast = cascade_casts(c, bounds) || cast;
                }
                if (cast) {
                    if (custom)
                        custom_shadows.push_back(
                            {instances.size(), i, &gpu_custom(render_instance.custom_materials[material_index])});
                    else if (material && material->alpha_mode == AlphaMode::mask)
                        masked_shadows.push_back(i);
                    else
                        shadows.push_back(i);
                }
                // The view chooses the level of each draw of an object that some pass records, once for every pass;
                // placed copies choose theirs by cluster as each pass records them. Such an object is always kept.
                if ((!culled || cast) && !render_instance.placements && !source_draws[i].levels.empty() &&
                    options.lod_threshold > 0) {
                    if (draw_levels.size() == first_level)
                        draw_levels.resize(first_level + source_draws.size());
                    draw_levels[first_level + i] = object_level(render_instance, i);
                }
            }
            const auto opaque_draw_count = draws.size() - first_draw;
            draws.insert(draws.end(), discarding_draws.begin(), discarding_draws.end());
            discarding_draws.clear();
            const auto opaque_shadow_count = shadows.size() - first_shadow;
            shadows.insert(shadows.end(), masked_shadows.begin(), masked_shadows.end());
            masked_shadows.clear();
            if (!candidate)
                continue;
            ++candidate_instances;
            const bool main_view =
                draws.size() != first_draw || blended.size() != first_blended || custom_draws.size() != first_custom;
            if (!main_view)
                ++culled_instances;
            // An instance with blended or custom draws is always kept, so their instance index above stays valid.
            if (!main_view && shadows.size() == first_shadow && custom_shadows.size() == first_custom_shadow)
                continue;
            const auto first_cluster = placed_cluster_bounds.size();
            if (render_instance.placements) {
                for (const auto &cluster : render_instance.placements->clusters())
                    placed_cluster_bounds.push_back(cluster_bounds(cluster, render_instance.world));
                if (main_view && use_frustum) {
                    const auto range = culling_range(render_instance);
                    for (auto bounds = placed_cluster_bounds.begin() + std::ptrdiff_t(first_cluster);
                         bounds != placed_cluster_bounds.end(); ++bounds) {
                        if (cull && !cluster_visible(*bounds, &resource_frustum, std::nullopt, eye()))
                            ++culled_clusters;
                        else if (range && !cluster_visible(*bounds, nullptr, range, eye()))
                            ++range_culled;
                    }
                }
            }
            // With placements or a visibility range, the object's world matrix and the range with the point it measures
            // to, then with placements the rest pose's node matrices, which the vertex shaders compose with each
            // placement, or else the palette.
            const auto &rest = render_instance.asset->rest_pose().world;
            const bool ranged = render_instance.visibility_range != VisibilityRange{};
            const bool header = render_instance.placements || ranged;
            const auto matrices =
                (header ? 2 : 0) + (render_instance.placements ? rest.size() : render_instance.palette.size());
            const auto max_matrices = properties.limits.maxStorageBufferRange / sizeof(Mat4);
            if (palettes.size() > max_matrices || matrices > max_matrices - palettes.size())
                throw std::length_error("Instance palettes exceed device storage-buffer range");
            const auto first_matrix = static_cast<std::uint32_t>(palettes.size());
            ResourceInstance prepared{gpu_asset(render_instance.asset, failure),
                                      &render_instance,
                                      first_matrix,
                                      first_draw,
                                      draws.size() - first_draw,
                                      opaque_draw_count,
                                      first_shadow,
                                      shadows.size() - first_shadow,
                                      opaque_shadow_count,
                                      {},
                                      first_matrix,
                                      ranged,
                                      render_instance.asset->impostor() != nullptr,
                                      false,
                                      draw_levels.size() != first_level,
                                      first_level,
                                      first_cluster};
            if (header) {
                prepared.palette_offset = first_matrix + 2;
                palettes.push_back(render_instance.world);
                palettes.push_back(range_columns(scaled_range(render_instance.visibility_range),
                                                 range_center(*render_instance.asset)));
            }
            if (render_instance.placements) {
                prepared.placements = gpu_placements(render_instance.placements);
                palettes.insert(palettes.end(), rest.begin(), rest.end());
            } else
                palettes.insert(palettes.end(), render_instance.palette.begin(), render_instance.palette.end());
            if (draws.size() != first_draw)
                view_order.push_back({view_order_key(render_instance.bounds), instances.size()});
            instances.push_back(std::move(prepared));
        }
    // The palettes go to the pose buffer of the slot that the next frame uses, whose previous frame has finished.
    auto &pose_buffer = frame().pose_buffer;
    const VkDeviceSize bytes = palettes.size() * sizeof(Mat4);
    if (bytes && (!pose_buffer || pose_buffer->bytes < bytes)) {
        // Grow only the small pose buffer; mesh resources and instance IDs stay put.
        VkDeviceSize capacity = pose_buffer ? pose_buffer->bytes : sizeof(Mat4);
        while (capacity < bytes)
            capacity = std::min<VkDeviceSize>(capacity * 2, properties.limits.maxStorageBufferRange);
        auto candidate = resource_buffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        inject_scene(failure, RendererFailureStage::palette, false);
        const VkDescriptorBufferInfo buffer{candidate->buffer, 0, capacity};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = frame().pose_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        pose_buffer.swap(candidate); // The slot's fence has retired the only frame that read the old buffer.
    }
    if (bytes) {
        std::memcpy(pose_buffer->mapping, palettes.data(), bytes);
        flush_resource(*pose_buffer);
    }
    sort_front_to_back(view_order);
    sort_back_to_front(blended);
    // The frame copies opaque depth and color only when a drawn custom material reads them; draw() allocates the
    // copies.
    const bool reads_opaque = std::any_of(blended.begin(), blended.end(), [](const BlendedDraw &draw) {
        return draw.custom && (draw.custom->source->reads_opaque_depth() || draw.custom->source->reads_opaque_color());
    });
    inject_scene(failure, RendererFailureStage::ready, false);
    resource_instances.swap(instances);
    resource_draws.swap(draws);
    resource_view_order.swap(view_order);
    resource_shadow_draws.swap(shadows);
    resource_blended_draws.swap(blended);
    resource_custom_draws.swap(custom_draws);
    resource_custom_shadow_draws.swap(custom_shadows);
    resource_draw_levels.swap(draw_levels);
    resource_cluster_bounds.swap(placed_cluster_bounds);
    resource_opaque_inputs = resources.opaque_inputs = reads_opaque;
    resources.pose_uploaded_bytes = bytes;
    resources.instances = candidate_instances - culled_instances;
    resources.candidate_instances = candidate_instances;
    resources.culled_instances = culled_instances;
    resources.candidate_draws = candidate_draws;
    resources.culled_draws = culled_draws;
    resources.culled_clusters = culled_clusters;
    resources.range_culled = range_culled;
}
// Zeroes the main view's draw counters, which record_indexed() accumulates as the main view records.
void clear_main_view_counters() noexcept {
    resources.draw_calls = resources.submitted_indices = resources.drawn_copies = resources.lod_draws = 0;
    resources.discarding_draw_calls = 0;
}
void set_scenes(std::vector<std::shared_ptr<const Scene>> sources, SceneReplacementOptions settings,
                bool initial = false) {
    running();
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!sources[i])
            throw std::invalid_argument("Renderer selection contains a null scene");
        if (std::find(sources.begin(), sources.begin() + i, sources[i]) != sources.begin() + i)
            throw std::invalid_argument("Renderer selection contains a duplicate scene");
    }
    const auto &failure = settings.fail_after;
    if (failure != RendererFailureStage::none && failure != RendererFailureStage::vertex &&
        failure != RendererFailureStage::index && failure != RendererFailureStage::texture &&
        failure != RendererFailureStage::texture_upload && failure != RendererFailureStage::descriptors &&
        failure != RendererFailureStage::palette && failure != RendererFailureStage::ready &&
        failure != RendererFailureStage::upload_timeout && failure != RendererFailureStage::device_lost)
        throw std::invalid_argument("Unknown resource failure stage");
    try {
        wait_for_frames("Wait before resource scene selection");
        // Selection preflights resources independently of the previous camera.
        // Visibility is evaluated again with the current camera at each draw.
        if (!sources.empty())
            prepare_resources(sources, failure, false);
        else {
            inject_scene(failure, RendererFailureStage::ready, initial);
            resource_instances.clear();
            resource_draws.clear();
            resource_view_order.clear();
            resource_shadow_draws.clear();
            resource_blended_draws.clear();
            resource_custom_draws.clear();
            resource_custom_shadow_draws.clear();
            resource_draw_levels.clear();
            resource_cluster_bounds.clear();
            resource_opaque_inputs = resources.opaque_inputs = false;
            resources.pose_uploaded_bytes = resources.instances = 0;
            resources.candidate_instances = resources.culled_instances = 0;
            resources.candidate_draws = resources.culled_draws = 0;
            resources.culled_clusters = resources.range_culled = 0;
            // A draw without scenes records no main view, so nothing else would clear the previous frame's counts.
            clear_main_view_counters();
            resources.shadow_draw_calls = resources.shadow_submitted_indices = 0;
        }
    } catch (const VulkanFailure &error) {
        if (fatal || error.result == VK_TIMEOUT || error.result == VK_ERROR_DEVICE_LOST) {
            fatal = true;
            throw RendererFatalError(error.what());
        }
        throw std::runtime_error(error.what()); // VulkanFailure is private.
    } catch (const InjectedRendererFailure &error) {
        if (fatal)
            throw RendererFatalError("Device lost while retiring resource upload");
        throw InjectedRendererFailure(error.stage(), initial);
    } catch (...) {
        if (fatal)
            throw RendererFatalError("Device lost while retiring resource upload");
        throw;
    }
    resource_scenes = std::move(sources);
    ++stats.scene_generations;
}
void prepare_meshes(std::span<const std::shared_ptr<const Mesh>> assets, const ResourcePreparationOptions &settings,
                    const MeshPreparation *prepared = nullptr) {
    running();
    const auto &failure = settings.fail_after;
    if (failure != RendererFailureStage::none && failure != RendererFailureStage::vertex &&
        failure != RendererFailureStage::index && failure != RendererFailureStage::texture &&
        failure != RendererFailureStage::texture_upload && failure != RendererFailureStage::descriptors &&
        failure != RendererFailureStage::ready && failure != RendererFailureStage::upload_timeout &&
        failure != RendererFailureStage::device_lost)
        throw std::invalid_argument("Unknown resource preparation failure stage");
    for (const auto &asset : assets)
        if (!asset)
            throw std::invalid_argument("Cannot prepare a null render asset");
    if (assets.empty())
        return;
    // Nothing here waits for the frames in flight, since none of them uses anything that gpu_asset() changes: it
    // returns a cached mesh as it is, and an upload writes only objects that it creates, sharing existing samplers
    // unchanged. Each upload waits for its own copies, whose fences Vulkan signals only after every command submitted
    // to the queue before them, so those waits also last until the frames in flight have finished.
    try {
        for (const auto &asset : assets)
            (void)gpu_asset(asset, failure, prepared);
        inject_scene(failure, RendererFailureStage::ready, false);
    } catch (const VulkanFailure &error) {
        if (fatal || error.result == VK_TIMEOUT || error.result == VK_ERROR_DEVICE_LOST) {
            fatal = true;
            throw RendererFatalError(error.what());
        }
        throw std::runtime_error(error.what()); // VulkanFailure is private.
    } catch (...) {
        if (fatal)
            throw RendererFatalError("Device lost while retiring resource preparation");
        throw;
    }
}
// Binds @p render_instance's vertices and indices, and its placement rows or the identity placement.
void bind_geometry(const ResourceInstance &render_instance) {
    const auto command = frame().command;
    const auto &gpu = *render_instance.gpu;
    const std::array<VkBuffer, 2> buffers{gpu.vertices->buffer, render_instance.placements
                                                                    ? render_instance.placements->rows->buffer
                                                                    : identity_placement->buffer};
    const std::array<VkDeviceSize, 2> offsets{};
    vkCmdBindVertexBuffers(command, 0, static_cast<std::uint32_t>(buffers.size()), buffers.data(), offsets.data());
    vkCmdBindIndexBuffer(command, gpu.indices->buffer, 0, gpu.index_type);
}
// Records draw @p draw_index of @p render_instance with the bound geometry, through @p view, and counts it in the
// shadow or main-view statistics: with @p routing, in the main view, the runs of its group through the pipeline that
// record_indexed() binds for each, and otherwise every run through the bound pipeline. It binds the draw's material set
// where resource_bound holds another, unless @p material_set is false for a pipeline that reads none
// (shadow_opaque_pipeline), and the frame's sets with the pass's ResourceViewPush where resource_bound lacks them,
// then pushes the draw's ResourceDrawPush. Returns whether it left runs to the other group.
bool record_draw(const ResourceInstance &render_instance, std::size_t draw_index, const Mat4 &view, bool shadow,
                 bool material_set, std::optional<ViewRouting> routing = std::nullopt) {
    const auto command = frame().command;
    const auto &source = *render_instance.source;
    const auto &draw = source.asset->draws()[draw_index];
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    auto &bound = resource_bound;
    if (material_set) {
        const auto set = render_instance.gpu->materials->material_sets.at(std::size_t(draw.material + 1));
        if (set != bound.material) {
            // Binding set 0 keeps the sets after it only where set 0 already holds a set bound through this layout.
            if (!bound.material)
                bound.frame = false;
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resource_pipeline_layout, 0, 1, &set, 0,
                                    nullptr);
            bound.material = set;
        }
    }
    if (!bound.frame) {
        // Binding the sets after set 0 keeps it, since it holds a set bound through this layout or none that the
        // pipeline reads.
        if (shadow)
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resource_pipeline_layout, 2, 1,
                                    &frame().pose_set, 0, nullptr);
        else {
            const std::array sets{frame().environment_set, frame().pose_set};
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resource_pipeline_layout, 1,
                                    static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        }
        const ResourceViewPush pass{view, view_origin};
        vkCmdPushConstants(command, resource_pipeline_layout, stages, 0, sizeof(pass), &pass);
        bound.frame = true;
    }
    const Material fallback;
    const auto &material = draw.material < 0 ? fallback : source.asset->materials()->material_data.at(draw.material);
    const auto factor = draw.material < 0 ? Vec3{1, 1, 1} : source.factors.at(draw.material);
    // An impostor's draw carries its frames' sphere where other draws carry their metallic and roughness factors, and
    // its frames per side and layout where they say whether they are skinned, which an impostor never is.
    const auto *impostor_frames = source.asset->impostor();
    const ResourceDrawPush push{impostor_frames ? std::array{impostor_frames->center.x, impostor_frames->center.y,
                                                             impostor_frames->center.z, impostor_frames->radius}
                                                : std::array{material.metallic, material.roughness, 0.F, 0.F},
                                {render_instance.palette_offset + draw.palette_offset,
                                 impostor_frames ? impostor_frames->count | std::uint32_t(impostor_frames->layout) << 8
                                 : draw.skinned  ? 1U
                                                 : 0U,
                                 render_instance.object_offset,
                                 (render_instance.placements ? 1U : 0U) | (render_instance.ranged ? 2U : 0U)},
                                {factor.x, factor.y, factor.z, 1}};
    vkCmdPushConstants(command, resource_pipeline_layout, stages, sizeof(ResourceViewPush), sizeof(push), &push);
    return record_indexed(render_instance, draw_index, shadow, routing);
}
// Records draw @p draw_index of @p render_instance for every run of placement_runs, with the bound geometry,
// descriptors and push constants, each at the level of detail its distance and scale choose, merging adjacent runs
// that choose the same level into one call, and counts each call and copy in the shadow or main-view statistics. With
// @p routing, in the main view, only the runs of its group record, each through its run_pipeline(), bound before its
// call unless bound_mesh_pipeline is already that pipeline, and runs merge only within one pipeline; otherwise every
// run draws through the bound pipeline. Pipelines that share resource_pipeline_layout keep the descriptor sets and
// push constants bound before them. Returns whether it left runs to the other group; a run that it leaves also keeps
// the runs on either side of it from merging.
bool record_indexed(const ResourceInstance &render_instance, std::size_t draw_index, bool shadow,
                    std::optional<ViewRouting> routing = std::nullopt) {
    const auto &source = *render_instance.source;
    const auto &draw = source.asset->draws()[draw_index];
    // Without placements the level that the view chose for the frame; with them, each run's cluster and the node's
    // rest matrix, which the copies' placements multiply, choose its level.
    const auto level_of = [&](const PlacementRun &run) -> const DrawLevel * {
        if (!render_instance.placements)
            return render_instance.chosen_levels ? resource_draw_levels[render_instance.first_level + draw_index]
                                                 : nullptr;
        if (draw.levels.empty() || options.lod_threshold <= 0)
            return nullptr;
        return level_for(draw, run.distance, run.scale * axis_scale(source.asset->rest_pose().world[draw.node]));
    };
    const auto command = frame().command;
    const auto record = [&](const DrawLevel *level, std::uint32_t first, std::uint32_t count, MeshPipeline through) {
        if (routing) {
            const auto bound = resource_pipelines[std::size_t(through)][uses_height_fog()];
            if (bound != bound_mesh_pipeline)
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
            bound_mesh_pipeline = bound;
            resources.discarding_draw_calls += through == MeshPipeline::discarding;
        }
        const auto index_count = level ? level->index_count : draw.index_count;
        vkCmdDrawIndexed(command, index_count, count, level ? level->first_index : draw.first_index, 0, first);
        const auto indices = std::uint64_t{index_count} * count;
        if (shadow) {
            ++resources.shadow_draw_calls;
            resources.shadow_submitted_indices += indices;
        } else {
            ++resources.draw_calls;
            resources.submitted_indices += indices;
            resources.drawn_copies += count;
            resources.lod_draws += level != nullptr;
        }
    };
    const DrawLevel *pending_level = nullptr;
    auto pending_pipeline = MeshPipeline::discarding;
    std::uint32_t pending_first = 0, pending_count = 0;
    bool left = false;
    for (const auto &run : placement_runs) {
        const auto through = routing ? run_pipeline(routing->pipeline, run) : MeshPipeline::discarding;
        if (routing && (through == MeshPipeline::discarding) != (routing->group == MeshGroup::masked)) {
            left = true;
            continue;
        }
        const auto *level = level_of(run);
        if (pending_count && level == pending_level && through == pending_pipeline &&
            pending_first + pending_count == run.first) {
            pending_count += run.count;
            continue;
        }
        if (pending_count)
            record(pending_level, pending_first, pending_count, pending_pipeline);
        pending_level = level;
        pending_pipeline = through;
        pending_first = run.first;
        pending_count = run.count;
    }
    if (pending_count)
        record(pending_level, pending_first, pending_count, pending_pipeline);
    return left;
}
// Records shadow pass @p shadow or, without one, the main view: the meshes by MeshGroup, then the opaque custom
// material draws, or in a shadow pass their depth-only variants, then in the main view the sky where none of them drew
// and, unless the frame copies opaque inputs first, the blended draws back to front. The main view records each group's
// objects in resource_view_order, nearest first, so that a GPU that tests depth before shading skips more of the
// surfaces that nearer ones hide, and its masked group also takes the runs that the opaque group left. A shadow pass
// records objects in selection and instance order, since a shadow map keeps the least depth of each texel whatever the
// order. Within a group each object's draws keep the order of Mesh::draws(), those that the masked group revisits
// included.
void record_resources(const ShadowPass *shadow = nullptr) {
    const auto command = frame().command;
    if (!shadow) {
        clear_main_view_counters();
        bound_mesh_pipeline = VK_NULL_HANDLE;
    }
    // The pass's view differs from the last pass's, and other renderers may have bound sets in between.
    resource_bound = {};
    const auto &view = shadow ? shadow->view : view_projection;
    const auto *frustum = shadow ? &shadow->frustum : options.frustum_culling ? &resource_frustum : nullptr;
    const auto objects = shadow ? resource_instances.size() : resource_view_order.size();
    // Each group draws through its pipeline, bound once its first draw records; in the main view the runs of each mesh
    // draw bind the MeshPipeline that they need instead.
    for (const auto group : {MeshGroup::opaque, MeshGroup::masked, MeshGroup::impostors}) {
        const bool impostors = group == MeshGroup::impostors;
        const auto group_pipeline = impostors
                                        ? (shadow ? shadow_impostor_pipeline : impostor_pipelines[uses_height_fog()])
                                    : !shadow                    ? VK_NULL_HANDLE
                                    : group == MeshGroup::opaque ? shadow_opaque_pipeline
                                                                 : shadow_masked_pipeline;
        // shadow_opaque_pipeline has no fragment shader, so it reads no material set.
        const bool material_set = !shadow || group != MeshGroup::opaque;
        bool bound = false;
        for (std::size_t object = 0; object < objects; ++object) {
            auto &render_instance = resource_instances[shadow ? object : resource_view_order[object].instance];
            if (render_instance.impostor != impostors)
                continue;
            // The instance's draws lie in two parts, each in Mesh::draws() order: those whose view_pipeline() cannot
            // discard, or in a shadow pass the opaque casters, then the others. The opaque group records the first
            // part; the masked group the second and, in the main view, the first again where the opaque group left
            // runs of it; and the impostor group both, since an impostor's draws all record through its own pipeline.
            // A group that records from both parts merges them back into Mesh::draws() order.
            const auto first = shadow ? render_instance.first_shadow : render_instance.first_draw;
            const auto split =
                first + (shadow ? render_instance.opaque_shadow_count : render_instance.opaque_draw_count);
            const auto last = first + (shadow ? render_instance.shadow_count : render_instance.draw_count);
            const bool revisit = !shadow && render_instance.deferred_runs;
            auto lower = first, upper = split;
            const auto lower_end = group == MeshGroup::masked && !revisit ? first : split,
                       upper_end = group == MeshGroup::opaque ? split : last;
            if (lower == lower_end && upper == upper_end)
                continue;
            find_placement_runs(render_instance, frustum, shadow ? shadow->min_caster_radius : 0);
            if (placement_runs.empty())
                continue;
            if (!bound && group_pipeline)
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, group_pipeline);
            bound = true;
            bind_geometry(render_instance);
            const auto draw_of = [&](std::size_t entry) {
                return shadow ? resource_shadow_draws[entry] : resource_draws[entry].draw;
            };
            bool deferred = false;
            while (lower < lower_end || upper < upper_end) {
                const auto i =
                    upper == upper_end || (lower < lower_end && draw_of(lower) < draw_of(upper)) ? lower++ : upper++;
                if (!shadow) {
                    const auto &[draw_index, draw_pipeline] = resource_draws[i];
                    const auto routing = impostors ? std::nullopt : std::optional(ViewRouting{draw_pipeline, group});
                    deferred = record_draw(render_instance, draw_index, view, false, true, routing) || deferred;
                    continue;
                }
                const auto draw_index = resource_shadow_draws[i];
                if (!shadow_holds(*shadow, render_instance, draw_index))
                    continue;
                record_draw(render_instance, draw_index, view, true, material_set);
            }
            if (!shadow && group == MeshGroup::opaque)
                render_instance.deferred_runs = deferred;
        }
    }
    record_custom_draws(shadow ? resource_custom_shadow_draws : resource_custom_draws, view, frustum, shadow);
    if (shadow)
        return;
    record_sky();
    if (!resource_opaque_inputs)
        record_blended();
}
// Records resource_blended_draws in order, each through the blended pipeline or its custom material's. Depth then
// holds every opaque and masked surface, which these pipelines test against without writing. A blended draw after a
// custom material's binds the resource sets and view again, since bind_custom() clears resource_bound.
void record_blended() {
    const auto command = frame().command;
    // A frame that copies opaque inputs records these in a pass of its own, after other commands, so they bind as
    // record_resources() does, from nothing.
    resource_bound = {};
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    const ResourceInstance *bound = nullptr;
    const auto *frustum = options.frustum_culling ? &resource_frustum : nullptr;
    for (const auto &blended : resource_blended_draws) {
        const auto &render_instance = resource_instances[blended.instance];
        find_placement_runs(render_instance, frustum);
        if (placement_runs.empty())
            continue;
        const auto draw_pipeline =
            blended.custom ? blended.custom->pipeline : blended_resource_pipelines[uses_height_fog()];
        if (draw_pipeline != bound_pipeline) {
            if (blended.custom)
                bind_custom(*blended.custom, false);
            else
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_pipeline);
            bound_pipeline = draw_pipeline;
        }
        if (&render_instance != bound) {
            bound = &render_instance;
            bind_geometry(render_instance);
        }
        if (blended.custom)
            record_custom_draw(render_instance, blended.draw, view_projection, false);
        else
            record_draw(render_instance, blended.draw, view_projection, false, true);
    }
}
ResourceStats resource_stats() const {
    auto value = resources;
    value.resident_material_samplers = std::count_if(material_samplers.begin(), material_samplers.end(),
                                                     [](const auto &entry) { return !entry.second.expired(); });
    value.cached_assets = resource_cache.size();
    value.resident_geometry_bytes = value.resident_texture_bytes = 0;
    for (const auto &[key, gpu] : resource_cache) {
        (void)key;
        if (gpu->vertices)
            value.resident_geometry_bytes += gpu->vertices->allocation_bytes + gpu->indices->allocation_bytes;
        for (const auto &texture : gpu->materials->textures)
            value.resident_texture_bytes += texture.allocation_bytes;
    }
    value.resident_placement_bytes = 0;
    for (const auto &[key, gpu] : placement_cache) {
        (void)key;
        value.resident_placement_bytes += gpu->rows->allocation_bytes;
    }
    value.cached_custom_materials = custom_cache.size();
    for (const auto &[key, gpu] : custom_cache) {
        (void)key;
        for (const auto &texture : gpu->textures)
            value.resident_texture_bytes += texture.allocation_bytes;
    }
    value.opaque_input_bytes =
        opaque_inputs ? opaque_inputs->color.allocation_bytes + opaque_inputs->depth.allocation_bytes : 0;
    value.shadow_bytes = cascade_target ? cascade_target->texture.allocation_bytes : 0;
    value.shadow_bytes += detail_shadow_target ? detail_shadow_target->texture.allocation_bytes : 0;
    value.atmosphere_bytes = atmosphere_bytes();
    value.pose_buffer_bytes = 0;
    for (const auto &slot : frames)
        value.pose_buffer_bytes += slot.pose_buffer ? slot.pose_buffer->allocation_bytes : 0;
    return value;
}
// Releases the cache entries that only the renderer still references, destroying them once no frame in flight can use
// them.
void retire_resources() {
    resource_instances.clear();
    resource_draws.clear();
    resource_view_order.clear();
    resource_shadow_draws.clear();
    resource_blended_draws.clear();
    resource_custom_draws.clear();
    resource_custom_shadow_draws.clear();
    resource_draw_levels.clear();
    resource_cluster_bounds.clear();
    resource_opaque_inputs = false;
    // Drops each entry whose source @p source gives only the cache's reference to.
    const auto release = [&](auto &cache, const auto &source) {
        for (auto it = cache.begin(); it != cache.end();) {
            if (source(*it->second).use_count() == 1) {
                release_after_frames(std::move(it->second));
                it = cache.erase(it);
            } else
                ++it;
        }
    };
    release(resource_cache, [](const GpuAsset &gpu) -> const auto & { return gpu.asset; });
    release(custom_cache, [](const GpuCustomMaterial &gpu) -> const auto & { return gpu.source; });
    release(placement_cache, [](const GpuPlacements &gpu) -> const auto & { return gpu.source; });
}
