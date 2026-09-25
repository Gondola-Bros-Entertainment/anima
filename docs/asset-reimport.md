# Asset reimport

`anima::AssetImports` in `<anima/assets/imports.hpp>` gives an application stable,
typed resource identities and explicit content-based reimport. It belongs to
`anima::assets` and adds no dependencies. Importers translate project files into
immutable resources; applications decide when to refresh and adopt new versions.

```cpp
#include <anima/assets/asset.hpp>
#include <anima/assets/imports.hpp>

anima::AssetImports imports(project_directory);
auto character = imports.add<anima::Asset>("character", [](anima::ImportSource &source) {
    return anima::load_asset(source.read("models/character.glb"));
});
auto previous = character.get();

// Between scene updates, after an application-defined refresh request:
const auto changed_keys = imports.refresh();
auto current = character.get();
// previous remains valid even if current now contains a different resource.
```

`add<T>` imports immediately and rejects empty/duplicate keys, null resources,
invalid inputs or inputs modified during import. `get<T>(key)` finds an existing
identity and checks its exact resource type. References start at revision 1;
`revision()` advances whenever their importer successfully publishes a refresh.
It advances even if the importer produces a resource equivalent to the old one.

Each importer reads its dependencies through `ImportSource::read`. Paths are
relative to the registry's canonical project root. Absolute paths and paths or
symlinks resolving outside that root reject. Reads return borrowed byte spans
valid during the import. The default per-file bound is 64 MiB; the constructor
accepts another positive bound. Resources must own any data needed afterward.
Importers must access their file inputs through this interface for tracking and
verification to cover them; unrelated external side effects cannot be rolled back.

`refresh()` reads the currently registered inputs and compares their bytes with
the accepted snapshots. It does not rely on timestamps or file size. Every
importer affected by a changed dependency runs against the transaction's shared
input snapshots. Newly read dependencies become tracked; obsolete dependencies
are removed. The registry verifies its inputs again before publishing. If any
read, importer or verification fails, it throws and keeps every previous resource
and revision. Callers decide how to report errors and when to retry.

A successful refresh publishes all affected resources together and returns their
keys in lexical order. Old resources remain alive until publication is complete
and any held snapshots release them. Adding an importer against a shared input
that changed since its accepted version rejects: refresh existing identities
first. `erase(key)` removes registry ownership and tracking; held references keep
their final version. Reusing the key creates a new identity and never revives the
erased reference.

The registry and references are used on one thread between scene updates.
Importers cannot mutate their registry or recursively refresh it. CPU import is
synchronous and retains accepted input bytes for comparison. There is no file
watcher or background worker, and this is not an operating-system filesystem
transaction: later file edits are found by the next refresh.

Scene objects and GPU resources are not changed by a refresh. The application
prepares replacement meshes from accepted assets and publishes them using the
existing [scene](scene-objects.md) and [resource preparation](resource-preparation.md)
contracts. Animation binding, component state and save-data migration are explicit
consumer decisions. `load_asset(bytes)` and `load_motion_asset(bytes)` parse
embedded GLB snapshots without retaining their input buffers or opening files.
