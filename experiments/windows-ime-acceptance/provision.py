"""Disconnect the one-time guest before any product payload is transferred."""
import base64
import json
from pathlib import Path
import socket
import sys
import time


def wait_serial(root, marker, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for line in (root / "serial.log").read_text(errors="replace").splitlines():
            if line.startswith("XDNOTE_VM_RESULT "):
                record = json.loads(base64.b64decode(line.removeprefix("XDNOTE_VM_RESULT "), validate=True))
                (root / "evidence" / "result.json").write_text(json.dumps(record, ensure_ascii=False, indent=2))
                raise RuntimeError(record.get("error", "The guest exited during provisioning"))
            if line == marker or line.startswith(marker + " "):
                return line
        time.sleep(1)
    raise TimeoutError(f"The isolated guest did not reach {marker}")


class Qmp:
    def __init__(self, root):
        self.connection = socket.socket(socket.AF_UNIX)
        self.connection.settimeout(120)
        self.connection.connect(str(root / "qmp"))
        self.stream = self.connection.makefile("rwb")
        self.events = []
        if "QMP" not in json.loads(self.stream.readline()):
            raise RuntimeError("Missing QMP greeting")
        self.call("qmp_capabilities")

    def read(self):
        line = self.stream.readline()
        if not line:
            raise RuntimeError("The QMP connection closed")
        record = json.loads(line)
        if "event" in record:
            self.events.append(record)
        return record

    def call(self, command, **arguments):
        self.stream.write(json.dumps({"execute": command, "arguments": arguments}).encode() + b"\n")
        self.stream.flush()
        while True:
            result = self.read()
            if "error" in result:
                raise RuntimeError(f"{command}: {result['error']}")
            if "return" in result:
                return result["return"]

    def wait_deleted(self):
        while True:
            for event in self.events:
                if event.get("data", {}).get("device") == "provision-nic":
                    if event["event"] == "DEVICE_UNPLUG_GUEST_ERROR":
                        raise RuntimeError("The guest rejected network-device removal")
                    if event["event"] == "DEVICE_DELETED":
                        return
            self.read()


def contains_device(value, device):
    if isinstance(value, dict):
        return value.get("qdev_id") == device or any(contains_device(item, device) for item in value.values())
    if isinstance(value, list):
        return any(contains_device(item, device) for item in value)
    return False


def validate_offline(record):
    adapters = record.get("activePhysicalAdapters")
    if type(adapters) is not int or adapters != 0 or record.get("languageCapability") != "Installed":
        raise RuntimeError("The guest did not confirm installed input and zero active physical adapters")


def main(root):
    root = Path(root)
    evidence = root / "evidence"
    state = {"deviceDeleted": False, "deviceAbsentFromPci": False, "backendRemoved": False, "guestOffline": False}
    try:
        wait_serial(root, "XDNOTE_VM_STAGE detach-provisioning-network", 1050)
        qmp = Qmp(root)
        qmp.call("device_del", id="provision-nic")
        qmp.wait_deleted()
        state["deviceDeleted"] = True
        if contains_device(qmp.call("query-pci"), "provision-nic"):
            raise RuntimeError("The provisioning adapter is still present on the PCI bus")
        state["deviceAbsentFromPci"] = True
        qmp.call("netdev_del", id="provision-net")
        state["backendRemoved"] = True
        line = wait_serial(root, "XDNOTE_VM_OFFLINE", 180)
        record = json.loads(base64.b64decode(line.removeprefix("XDNOTE_VM_OFFLINE "), validate=True))
        validate_offline(record)
        state["guestOffline"] = True
        state["guest"] = record
    except Exception as error:
        state["error"] = str(error)
        raise
    finally:
        (evidence / "network-isolation.json").write_text(json.dumps(state, indent=2) + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
