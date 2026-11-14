"""Exercise a real staged payload without installing into a user directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform

import desktop
import user_install
from background_validation import execute, offscreen_environment


def validate(payload, destination, build):
    payload = desktop.build_path(payload)
    destination = desktop.build_path(destination)
    build = desktop.build_path(build)
    evidence = destination.with_name(destination.name + "-evidence")
    if destination.exists() or evidence.exists():
        raise ValueError("Choose unused validation directories; existing evidence is preserved")
    evidence.mkdir(parents=True)
    sample = evidence / "preserved-user-data.json"
    sample.write_text('{"settings":"preserved","draft":"fixture"}', encoding="utf-8")
    before = hashlib.sha256(sample.read_bytes()).hexdigest()
    steps = [user_install.install(payload, destination, apply=True, registration=False)]
    system = platform.system()
    if system in ("Linux", "Darwin"):
        from deployment_unix import smoke
        if system == "Darwin":
            contents = destination / "xdnote.app/Contents"
            bindir, plugins, libraries = contents / "MacOS", contents / "PlugIns", contents / "Frameworks"
        else:
            bindir, plugins, libraries = destination / "bin", destination / "plugins", destination / "lib"
        probes = smoke(bindir, plugins, libraries, evidence, system)
    else:
        env = {name: value for name, value in os.environ.items()
               if not name.upper().startswith(("QT", "QML", "LD_LIBRARY", "DYLD_"))}
        env["PATH"] = str(destination) + os.pathsep + str(Path(os.environ["SystemRoot"]) / "System32")
        env = offscreen_environment(env, build)
        env.update(QT_PLUGIN_PATH=str(destination), QT_QPA_PLATFORM_PLUGIN_PATH=str(destination / "platforms"),
                   QML_IMPORT_PATH=str(destination / "qml"), QML2_IMPORT_PATH=str(destination / "qml"))
        commands = [("light", [destination / "xdnote_background_probe.exe", "preview", 0, "light", evidence / "light.png"]),
                    ("dark", [destination / "xdnote_background_probe.exe", "preview", 0, "dark", evidence / "dark.png"]),
                    ("product", [destination / "xdnote_preview_product_probe.exe", 10240, "mixed", evidence / "product.png"])]
        probes = []
        for name, command in commands:
            output, _ = execute(command, env, evidence / (name + ".log"), 90)
            values = [json.loads(line) for line in output.splitlines() if line.startswith('{"')]
            if len(values) != 1 or not values[0].get("ok"):
                raise RuntimeError("Installed runtime probe failed: " + name)
            probes.append({"case": name, "result": values[0]})
    steps.append(user_install.install(payload, destination, apply=True, registration=False))
    steps.append(user_install.uninstall(destination, apply=True, registration=False))
    after = hashlib.sha256(sample.read_bytes()).hexdigest()
    if destination.exists() or before != after:
        raise RuntimeError("Installation roundtrip did not preserve its fixture data")
    result = {"ok": True, "system": system, "steps": steps, "probes": probes,
              "fixtureSha256Before": before, "fixtureSha256After": after,
              "realRegistryChanged": False, "releaseReady": False,
              "buildIdentity": json.loads((build / "build-identity.json").read_text(encoding="utf-8"))}
    (evidence / "report.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"ok": True, "system": system, "probes": len(probes), "userDataUnchanged": True,
                      "realRegistryChanged": False, "report": str(evidence / "report.json")}, ensure_ascii=False))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args()
    validate(args.payload, args.destination, args.build_dir)
