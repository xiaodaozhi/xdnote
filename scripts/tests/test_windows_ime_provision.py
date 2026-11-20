import base64
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "windows_ime_provision", ROOT / "experiments/windows-ime-acceptance/provision.py"
)
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)


class FakeQmp:
    def __init__(self, pci=None):
        self.calls = []
        self.pci = pci or []

    def call(self, name, **arguments):
        self.calls.append((name, arguments))
        return self.pci if name == "query-pci" else {}

    def wait_deleted(self):
        self.calls.append(("wait_deleted", {}))


class ProvisionIsolationTests(unittest.TestCase):
    def offline_line(self, count=0, capability="Installed"):
        record = {"activePhysicalAdapters": count, "languageCapability": capability}
        return "XDNOTE_VM_OFFLINE " + base64.b64encode(json.dumps(record).encode()).decode()

    def test_transfer_gate_requires_adapter_and_backend_removal(self):
        fake = FakeQmp()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "evidence").mkdir()
            with patch.object(provision, "Qmp", return_value=fake), patch.object(
                provision, "wait_serial", side_effect=["XDNOTE_VM_STAGE detach-provisioning-network", self.offline_line()]
            ):
                provision.main(root)
            state = json.loads((root / "evidence/network-isolation.json").read_text())
        self.assertTrue(all(state[name] for name in (
            "deviceDeleted", "deviceAbsentFromPci", "backendRemoved", "guestOffline"
        )))
        self.assertEqual([item[0] for item in fake.calls],
                         ["device_del", "wait_deleted", "query-pci", "netdev_del"])

    def test_present_adapter_or_invalid_guest_receipt_blocks_transfer(self):
        for pci, line in (
            ([{"devices": [{"pci_bridge": {"devices": [{"qdev_id": "provision-nic"}]}}]}], self.offline_line()),
            ([], self.offline_line(count=1)),
            ([], self.offline_line(count=False)),
            ([], self.offline_line(capability="NotPresent")),
        ):
            with self.subTest(pci=pci, line=line), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "evidence").mkdir()
                fake = FakeQmp(pci)
                with patch.object(provision, "Qmp", return_value=fake), patch.object(
                    provision, "wait_serial", side_effect=["XDNOTE_VM_STAGE detach-provisioning-network", line]
                ):
                    with self.assertRaises(RuntimeError):
                        provision.main(root)
                state = json.loads((root / "evidence/network-isolation.json").read_text())
                self.assertFalse(state["guestOffline"])
                if pci:
                    self.assertNotIn("netdev_del", [item[0] for item in fake.calls])


if __name__ == "__main__":
    unittest.main()
