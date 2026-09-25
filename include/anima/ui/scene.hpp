#pragma once
#include <anima/scene.hpp>
#include <anima/ui/document.hpp>
namespace anima {
using UiDocumentResolver = std::function<std::filesystem::path(std::string_view)>;
// One independently owned document per component/prefab instance. The asset key
// is application-defined; codecs resolve it explicitly, never serialize paths.
class UiPanel {
  public:
    UiPanel(UiDocuments &host, std::string asset_key, const std::filesystem::path &path, bool visible = true);
    [[nodiscard]] UiDocument &document() { return document_; }
    [[nodiscard]] const UiDocument &document() const { return document_; }
    [[nodiscard]] const std::string &asset_key() const { return asset_key_; }
    void set_visible(bool visible);
    [[nodiscard]] bool visible() const { return visible_; }

  private:
    std::string asset_key_;
    UiDocument document_;
    bool visible_ = true;
};
// Reconcile inherited activation, component enablement and visibility before UI
// layout/input. Requires idle scenes. Validate all documents before publication;
// native show/hide events may mutate objects, so callback effects are not atomic.
// Removed panels/closed documents are skipped; additions wait until the next call.
// Nested drivers, scene phases and set membership changes are rejected during events.
void sync_ui_panels(Scene &scene);
void sync_ui_panels(SceneSet &scenes);
void add_ui_component_codec(ComponentCodecs &codecs, UiDocuments &host, UiDocumentResolver resolve);
} // namespace anima
