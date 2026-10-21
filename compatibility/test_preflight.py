"""Small own-code fixtures; no game assets or executables are run."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import preflight as p


def record(data):
    return [len(data), hashlib.sha256(data).hexdigest()]


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=p.HERE)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.core = {"game.exe": b"fixture exe", "ini/user.ini": b"strict config"}
        self.en = {"art/fe/english.loc": b"english", "art/fe/fonts/arial60.res": b"en font"}
        self.ru = {"art/fe/english.loc": b"russian", "art/fe/fonts/arial60.res": b"ru font"}
        self.baseline = {"critical": {k: record(v) for k, v in self.core.items()}, "locales": {
            "English": {"files": {k: record(v) for k, v in self.en.items()}},
            "Russian": {"files": {k: record(v) for k, v in self.ru.items()}},
        }}

    def put(self, records):
        for name, data in records.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def verify(self):
        return p.verify_records(self.baseline, p.scan(self.root))

    def test_both_packs_share_critical_digest(self):
        self.put(self.core | self.en)
        english = self.verify()
        self.put(self.ru)
        russian = self.verify()
        self.assertEqual(english["critical_digest"], russian["critical_digest"])
        self.assertEqual(english["language"], "English")
        self.assertEqual(russian["language"], "Russian")
        self.assertNotEqual(english["locale_digest"], russian["locale_digest"])

    def test_tamper_rejected_including_ini(self):
        for target in self.core:
            with self.subTest(target=target):
                self.put(self.core | self.en | {target: b"tampered"})
                with self.assertRaisesRegex(p.Rejected, "Critical content mismatch"):
                    self.verify()

    def test_missing_rejected(self):
        self.put(self.core | self.en)
        (self.root / "game.exe").unlink()
        with self.assertRaisesRegex(p.Rejected, "Missing runtime file"):
            self.verify()

    def test_case_duplicates_rejected_before_dictionary_collapse(self):
        with self.assertRaisesRegex(p.Rejected, "Duplicate case-insensitive"):
            p.index([("art/fe/INTRO.bik", 1), ("art/fe/intro.bik", 2)])

    def test_path_case_normalized(self):
        self.assertEqual(p.index([("ART\\FE\\INTRO.bik", 1)]), {"art/fe/intro.bik": 1})

    def test_mixed_pack_rejected(self):
        self.put(self.core | self.en | {"art/fe/fonts/arial60.res": self.ru["art/fe/fonts/arial60.res"]})
        with self.assertRaisesRegex(p.Rejected, "Unknown or mixed language pack"):
            self.verify()

    def test_unknown_language_data_rejected(self):
        self.put(self.core | self.en | {"art/fe/english.loc": b"unrecognized translation"})
        with self.assertRaisesRegex(p.Rejected, "Unknown or mixed language pack"):
            self.verify()

    def test_extra_runtime_and_hidden_dll_rejected(self):
        self.put(self.core | self.en)
        for target in ("ini/extra.ini", "art/extra.data", "gamelog/proxy.dll"):
            with self.subTest(target=target):
                self.put({target: b"extra"})
                with self.assertRaises(p.Rejected):
                    self.verify()
                (self.root / target).unlink()

    def test_known_metadata_and_generated_logs_ignored(self):
        self.put(self.core | self.en | {"unins000.dat": b"metadata", "GameLog/ExecutionPath": b"log"})
        self.assertEqual(self.verify()["status"], "pass")

    def test_outside_test_root_rejected(self):
        test_root = self.root / "test"
        test_root.mkdir()
        with self.assertRaisesRegex(p.Rejected, "Path must stay inside"):
            p.confined(self.root / "outside", test_root)

    def test_reparse_attribute_rejected(self):
        fake = type("FakeStat", (), {"st_mode": 0, "st_file_attributes": 0x400})()
        with patch.object(Path, "lstat", return_value=fake):
            with self.assertRaisesRegex(p.Rejected, "Reparse/symlink"):
                p.no_reparse(self.root)

    def test_new_output_only_and_no_output_on_failure(self):
        output = self.root / "result.json"
        self.put(self.core | self.en | {"ini/user.ini": b"tampered"})
        with patch.object(p, "TEST", self.root.parent), patch.object(p, "check_baseline", return_value=self.baseline):
            self.assertEqual(p.main(["verify", "--game-dir", str(self.root), "--output", str(output)]), 2)
        self.assertFalse(output.exists())
        p.write_new(output, {"status": "pass"})
        with self.assertRaisesRegex(p.Rejected, "NEW file"):
            p.write_new(output, {"status": "pass"})

    def test_cli_success_writes_language_and_common_digest(self):
        self.put(self.core | self.ru)
        output = self.root / "result.json"
        with patch.object(p, "TEST", self.root.parent), patch.object(p, "check_baseline", return_value=self.baseline):
            self.assertEqual(p.main(["verify", "--game-dir", str(self.root), "--output", str(output)]), 0)
            saved = output.read_bytes()
            self.assertEqual(p.main(["verify", "--game-dir", str(self.root), "--output", str(output)]), 2)
            self.assertEqual(output.read_bytes(), saved)
        result = json.loads(saved)
        self.assertEqual(result["language"], "Russian")
        self.assertEqual(result["critical_digest"], p.digest(self.baseline["critical"], "critical"))
        self.assertFalse(any(":\\" in str(value) for value in result.values()))

    def test_baseline_mutation_rejected(self):
        baseline = p.check_baseline(p.build_baseline())
        altered = copy.deepcopy(baseline)
        altered["critical"]["ini/user.ini"][1] = "0" * 64
        with self.assertRaisesRegex(p.Rejected, "not pinned"):
            p.check_baseline(altered)


if __name__ == "__main__":
    unittest.main()
