"""Refresh durable design identities; do not create archives or runtime evidence."""
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DESIGN = ROOT / "design"
manifest_path = DESIGN / "manifest.json"
manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
manifest["assets"] = [
    item for item in manifest["assets"]
    if item.get("status") != "delivery" and item["id"] != "source-v2-package-designs.py"
]


def register(identifier, relative, kind, status="source"):
    p = DESIGN / relative
    data = p.read_bytes()
    item = {
        "id": identifier, "path": relative, "kind": kind, "status": status,
        "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        "referenceAssetIds": [],
        "source": {"kind": "project-local-design-export", "sourceFilename": p.name},
    }
    if p.suffix == ".png":
        width, height = struct.unpack(">II", data[16:24])
        item["pixelSize"] = {"width": width, "height": height, "meaning": "raster-metadata-not-logical-window-size"}
    manifest["assets"] = [entry for entry in manifest["assets"] if entry["id"] != identifier] + [item]


sources = (
    "ui-suite-v2.json", "ui-suite-v2-extensions.json", "ui-suite-v2-revisions.json",
    "ui-suite-v2-refinements.json", "brand-placements.json", "brand-mark.cjs",
    "compare-titlebar-heights.cjs", "compare-standalone-header.cjs", "standalone-chrome-edit.cjs",
    "align-standalone-titlebar.cjs", "build-ui-assets.cjs", "ui-icon-paths.cjs",
    "export-native-icons.py", "finalize-ui-suite.cjs", "index-design-materials.py",
    "check-ui-materials.cjs", "README.md",
)
for name in sources:
    register("source-v2-" + name, "sources/" + name, "design-source")
register("asset-manifest-v2", "assets/ui-v2/manifest.json", "asset-manifest")
register("gallery-v2", "index.html", "static-gallery", "current")
register("overview-v2", "hifi/v2/overview.png", "contact-sheet", "current")
register("featured-v2", "hifi/v2/featured.png", "contact-sheet", "current")
register("comparison-titlebar-height", "hifi/v2/comparisons/titlebar-height.png", "design-comparison", "exploration")
register("comparison-standalone-header", "hifi/v2/comparisons/standalone-header-alignment.png", "design-comparison", "exploration")
for p in (DESIGN / "assets/ui-v2/previews").glob("*.png"):
    register("preview-v2-" + p.stem, p.relative_to(DESIGN).as_posix(), "asset-preview", "current")
manifest["authoritySnapshots"] = []
for name in ("tokens.json", "asset-geometry.json", "window-registry.json", "surface-registry-v2.json"):
    p = ROOT / "docs/visual" / name
    data = p.read_bytes()
    manifest["authoritySnapshots"].append({
        "path": p.relative_to(ROOT).as_posix(), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
    })
manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
print(json.dumps({"scope": "static-design-index", "assets": len(manifest["assets"])}))
