"""Export native icon containers from the approved design icon."""
import hashlib
import json
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "design/assets/ui-v2"
source = OUT / "brand/png/app-icon-1024.png"
image = Image.open(source).convert("RGBA")
windows_icon = OUT / "brand/app-icon.ico"
macos_icon = OUT / "brand/app-icon.icns"
macos_png = OUT / "brand/png/app-icon-macos-1024.png"
image.save(windows_icon, format="ICO", sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])

# Keep the approved artwork proportional while resizing the visible backing,
# then center it on the legacy macOS canvas used by Finder and the Dock.
artwork = image.crop(image.getchannel("A").getbbox())
artwork = artwork.resize((824, 824), Image.Resampling.LANCZOS)
macos = Image.new("RGBA", (1024, 1024))
macos.paste(artwork, (100, 100))
macos.save(macos_png)
macos.save(macos_icon, format="ICNS")

targets = [windows_icon, macos_icon, macos_png]
manifest_path = OUT / "manifest.json"
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
native_ids = {"brand-native-containers", "brand-macos-icon"}
manifest["assets"] = [a for a in manifest["assets"] if a["id"] not in native_ids]
for asset_id, files in (("brand-native-containers", [windows_icon]), ("brand-macos-icon", [macos_icon, macos_png])):
    manifest["assets"].append({
        "id": asset_id, "kind": "native-icon-design-export", "approval": "approved-design-direction",
        "source": "brand/png/app-icon-1024.png", "files": [p.relative_to(OUT).as_posix() for p in files],
    })
for target in targets:
    data = target.read_bytes()
    record = {"path": target.relative_to(OUT).as_posix(), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    manifest["files"] = [f for f in manifest["files"] if f["path"] != record["path"]] + [record]
manifest["counts"]["files"] = len(manifest["files"])
manifest["counts"]["logicalAssets"] = len(manifest["assets"])
manifest["files"].sort(key=lambda f: f["path"])
manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
print(json.dumps({"nativeIconExports": [p.name for p in targets], "nativePlatformValidation": "not-run"}))
