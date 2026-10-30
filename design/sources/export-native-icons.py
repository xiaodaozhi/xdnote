"""Export native icon containers from the approved design icon."""
import hashlib
import json
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "design/assets/ui-v2"
source = OUT / "brand/png/app-icon-1024.png"
image = Image.open(source).convert("RGBA")
targets = [OUT / "brand/app-icon.ico", OUT / "brand/app-icon.icns"]
image.save(targets[0], format="ICO", sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])
image.save(targets[1], format="ICNS")
manifest_path = OUT / "manifest.json"
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
manifest["assets"] = [a for a in manifest["assets"] if a["id"] != "brand-native-containers"]
manifest["assets"].append({"id": "brand-native-containers", "kind": "native-icon-design-export", "approval": "approved-design-direction", "source": "brand/png/app-icon-1024.png", "files": [p.relative_to(OUT).as_posix() for p in targets]})
for target in targets:
    data = target.read_bytes()
    record = {"path": target.relative_to(OUT).as_posix(), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    manifest["files"] = [f for f in manifest["files"] if f["path"] != record["path"]] + [record]
manifest["counts"]["files"] = len(manifest["files"])
manifest["counts"]["logicalAssets"] = len(manifest["assets"])
manifest["files"].sort(key=lambda f: f["path"])
manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
print(json.dumps({"nativeIconExports": [p.name for p in targets], "nativePlatformValidation": "not-run"}))
