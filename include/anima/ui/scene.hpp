#pragma once
#include <anima/scene.hpp>
#include <anima/ui/document.hpp>

/// @file
/// Scene and prefab integration for UI documents: the UiPanel component, its visibility driver and
/// a persistence codec. Part of the optional `anima::ui_scene` target, which `ANIMA_BUILD_UI` or
/// `ANIMA_BUILD_UI_DOCUMENTS` builds when `ANIMA_BUILD_ASSETS=ON`; it needs no SDL or Vulkan.

namespace anima {
/// Maps a stored UiPanel asset key to the document path to load. The codec passes the key exactly
/// as stored, and only a key that UiPanel accepts; exceptions the resolver throws propagate.
using UiDocumentResolver = std::function<std::filesystem::path(std::string_view)>;
/// Component that owns one document for its GameObject.
///
/// Every panel loads its own document, so prefab instances never share one. The document is laid
/// out in screen space; the object's transform does not affect it. sync_ui_panels shows it while
/// the component is enabled on an active object and the panel is set visible, and hides it
/// otherwise. Removing the component, destroying its object or scene, or rolling back a failed
/// prefab instantiation closes the document.
class UiPanel {
  public:
    /// Loads @p path through @p host and, if @p visible, shows the document at once, whatever the
    /// component's or object's state; the next sync_ui_panels reconciles it. The codec stores
    /// @p asset_key in place of the path. Throws `std::invalid_argument` for an empty key or one
    /// over 4,096 bytes, and fails as UiDocuments::load does.
    UiPanel(UiDocuments &host, std::string asset_key, const std::filesystem::path &path, bool visible = true);
    /// The owned document. Showing or hiding it directly lasts until the next sync_ui_panels.
    [[nodiscard]] UiDocument &document() { return document_; }
    [[nodiscard]] const UiDocument &document() const { return document_; }
    /// Application-defined key that the codec stores in place of the document path.
    [[nodiscard]] const std::string &asset_key() const { return asset_key_; }
    /// Sets the authored visibility, which takes effect at the next sync_ui_panels.
    void set_visible(bool visible);
    /// Authored visibility, which the codec stores; not the document's current state.
    [[nodiscard]] bool visible() const { return visible_; }

  private:
    std::string asset_key_;
    UiDocument document_;
    bool visible_ = true;
};
/// Shows or hides the document of every UiPanel in @p scene to match its component's active state
/// and authored visibility. Call it after changing enablement, activation or UiPanel::set_visible,
/// before UI layout and input.
///
/// Every panel is validated first, including inactive ones: an expired document throws
/// `std::out_of_range` before any visibility changes. Showing and hiding run `show` and `hide`
/// callbacks, whose effects are not rolled back. Each panel stays alive through its own
/// callbacks; panels removed, or documents closed, by earlier callbacks are skipped, and panels
/// added meanwhile wait for the next call. Inside those callbacks, phases and drivers of @p scene
/// throw `std::logic_error`, and callback exceptions are captured by the document host; report
/// them with UiDocuments::check_events. Throws `std::logic_error` while the scene is updating,
/// under construction or destroyed.
void sync_ui_panels(Scene &scene);
/// Synchronizes every scene of @p scenes as sync_ui_panels(Scene &) does, validating all of them
/// before any change. Inside the callbacks, every scene of the set is guarded and set membership
/// changes also throw `std::logic_error`; the call throws it while the set is changing or
/// scheduling.
void sync_ui_panels(SceneSet &scenes);
/// Registers the `anima.ui-panel.v1` component codec, bound to @p host without keeping it alive:
/// restoring after the host is destroyed or shut down throws `std::out_of_range`.
///
/// The payload stores the asset key and authored visibility as a JSON object of at most 8,192
/// bytes with exactly `asset` (a string) and `visible` (a boolean); unknown, missing or duplicate
/// fields are rejected, and a key that UiPanel rejects throws its `std::invalid_argument` before
/// @p resolve runs. The scene or prefab stores component enablement. Paths, subscriptions and
/// document contents are not stored: restoring loads a new document from the path @p resolve
/// returns. Throws `std::invalid_argument` for an empty @p resolve or when @p codecs already has
/// a UiPanel codec or this key.
void add_ui_component_codec(ComponentCodecs &codecs, UiDocuments &host, UiDocumentResolver resolve);
} // namespace anima
