"""Verify lossless resource separation against an intermediate compiler reference.

An optional accepted reference protects existing authored motion during the
cutover. Neither reference is loaded by the production runtime.
"""

import argparse
import json
from pathlib import Path
from .preservation import content


def resource_content(manifest_path, garment_catalog=None):
    manifest = json.loads(manifest_path.read_text())
    directory = manifest_path.parent
    contract = json.loads((directory / manifest["motion_contract"]).read_text())
    if contract["version"] != 3 or manifest["clips"] or manifest["equipment"]:
        raise ValueError("Expected an independent production resource manifest")
    body = content(directory / manifest["model"])
    motion = content((directory / manifest["motion_contract"]).parent / contract["resource"])
    meshes = dict(body[0])
    transforms = dict(body[3])
    catalog = json.loads(garment_catalog.read_text()) if garment_catalog else {"items": []}
    for item in catalog["items"]:
        for fit in item["fits"].values():
            if (
                fit["skeleton"] == manifest["skeleton"]["id"]
                and fit["bind_signature"] == manifest["skeleton"]["bind_signature"]
            ):
                gear = content(garment_catalog.parent / fit["model"])
                if meshes.keys() & gear[0].keys():
                    raise ValueError("Duplicate fitted garment surfaces")
                meshes.update(gear[0])
                transforms.update(gear[3])
    return meshes, motion[1], body[2], transforms


def qualify_change(
    accepted_manifest,
    candidate_manifest,
    allowed=(),
    *,
    accepted_garments=None,
    candidate_garments=None,
):
    before = resource_content(accepted_manifest, accepted_garments)
    after = resource_content(candidate_manifest, candidate_garments)
    if any(before[i] != after[i] for i in (0, 2, 3)):
        raise ValueError("Motion workflow changed body, fitted garments or bind data")
    if set(before[1]) != set(after[1]):
        raise ValueError("Register motion additions/removals as a recipe migration")
    changed = sorted(name for name in before[1] if before[1][name] != after[1][name])
    if set(changed) - set(allowed):
        raise ValueError("Changed undeclared motion owners: " + str(set(changed) - set(allowed)))
    return {
        "geometry_and_bind_exact": True,
        "changed_motion_resources": changed,
        "allowed_motion_changes": sorted(allowed),
    }


def qualify(manifest_path, reference_path, accepted_path=None, allowed=(), *, garment_catalog=None):
    manifest = json.loads(manifest_path.read_text())
    directory = manifest_path.parent
    contract = json.loads((directory / manifest["motion_contract"]).read_text())
    body = content(directory / manifest["model"])
    motion = content((directory / manifest["motion_contract"]).parent / contract["resource"])
    reference = content(reference_path)
    if manifest["clips"] or manifest["equipment"] or body[1] or motion[0] or motion[2]:
        raise ValueError("Resources mix body, motion or equipment ownership")
    meshes = dict(body[0])
    transforms = dict(body[3])
    catalog = json.loads(garment_catalog.read_text()) if garment_catalog else {"items": []}
    fits = []
    for item in catalog["items"]:
        for fit in item["fits"].values():
            if (
                fit["skeleton"] != manifest["skeleton"]["id"]
                or fit["bind_signature"] != manifest["skeleton"]["bind_signature"]
            ):
                continue
            gear = content(garment_catalog.parent / fit["model"])
            if gear[1] or gear[2] != body[2] or meshes.keys() & gear[0].keys():
                raise ValueError("Garments own motion, disagree on bind or duplicate surfaces")
            meshes.update(gear[0])
            transforms.update(gear[3])
            fits.append(fit["model"])
    if meshes != reference[0] or body[2] != reference[2] or transforms != reference[3]:
        raise ValueError("Resource separation changed geometry, shading or bind data")
    names = {c["name"] for c in contract["clips"]}
    if set(motion[1]) != names | contract["layers"].keys():
        raise ValueError("Motion contract does not cover its resource")
    for name in names:
        if motion[1][name] != reference[1][name]:
            raise ValueError("Base motion changed while packaging: " + name)
    for name, layer in contract["layers"].items():
        owns = set(layer["owned_joints"]) | set(layer["context_joints"])
        expected = [c for c in reference[1][name] if c[0] in owns]
        if motion[1][name] != expected:
            raise ValueError("Owned layer changed while packaging: " + name)
    protected = []
    if accepted_path:
        accepted = content(accepted_path)
        if (
            accepted[0] != reference[0]
            or accepted[2] != reference[2]
            or accepted[3] != reference[3]
        ):
            raise ValueError("Accepted body, garments, shading or bind changed")
        for name in names:
            if name in allowed:
                continue
            if name not in accepted[1] or accepted[1][name] != reference[1][name]:
                raise ValueError("Protected motion changed: " + name)
            protected.append(name)
    return {
        "resource_separation_exact": True,
        "garments": fits,
        "base_motions": sorted(names),
        "owned_layers": sorted(contract["layers"]),
        "protected_motion": sorted(protected),
        "allowed_motion_changes": sorted(allowed),
    }


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--reference", type=Path, required=True)
    p.add_argument("--accepted", type=Path)
    p.add_argument("--garment-catalog", type=Path)
    p.add_argument("--allow-motion", action="append", default=[])
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    result = qualify(
        a.manifest, a.reference, a.accepted, a.allow_motion, garment_catalog=a.garment_catalog
    )
    a.out.write_text(json.dumps(result, indent=2) + "\n")
    print("PASS lossless body, garment and motion resource separation")
