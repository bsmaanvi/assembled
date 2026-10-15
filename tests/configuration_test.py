"""End-to-end checks against a real executable and temporary storage."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ConfigurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "intraday-config"
        subprocess.run([
            "g++", "-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            str(ROOT / "main.cpp"), str(ROOT / "rule_store.cpp"),
            str(ROOT / "rule_collection.cpp"),
            str(ROOT / "notification_engine.cpp"),
            "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.path = Path(self.storage.name) / "rules.txt"

    def run_app(self, commands, path=None, expected_code=0):
        result = subprocess.run(
            [str(self.binary), str(self.path if path is None else path)],
            input=commands, text=True, capture_output=True, timeout=5,
        )
        self.assertEqual(result.returncode, expected_code, result.stderr)
        return result.stdout + result.stderr

    def test_create_edit_disable_restart_enable(self):
        # Create backlog and SLA rules, disable backlog, edit its threshold.
        output = self.run_app(
            "1\n1\n1\n20\n1\n3\n2\n5\n1\n3\n1\n1\n1\n25\n2\n6\n"
        )
        self.assertIn("#1 [disabled] billing: more than 25", output)
        self.assertIn("#2 [enabled] vip: oldest ticket wait", output)
        output = self.run_app("2\n4\n1\n6\n")
        self.assertIn("#1 [disabled] billing: more than 25", output)
        output = self.run_app("2\n1\n2\n2\n6\n")
        self.assertIn("#1 [enabled] billing: more than 25", output)
        self.assertIn("Saved rule #3", output)

    def test_edit_can_change_rule_type(self):
        self.run_app("1\n1\n1\n20\n3\n1\n2\n2\n6\n")
        output = self.run_app("2\n6\n")
        self.assertIn("#1 [enabled] tier_2: oldest ticket wait", output)
        self.assertNotIn("more than 20", output)

    def test_invalid_input_preserves_saved_file(self):
        self.run_app("1\n1\n1\n20\n6\n")
        before = self.path.read_bytes()
        cases = [
            "abc\n", "0\n", "1\n4\n", "1\n1\n3\n",
            "1\n1\n1\n-1\n", "1\n1\n1\n20abc\n",
            "1\n1\n1\n1.5\n", "1\n1\n1\n9999999999999999999\n",
            "3\n999\n", "4\n999\n", "5\n999\n",
        ]
        for commands in cases:
            with self.subTest(commands=commands):
                self.assertIn("Not applied:", self.run_app(commands + "6\n"))
                self.assertEqual(self.path.read_bytes(), before)

    def test_zero_threshold(self):
        self.run_app("1\n1\n1\n0\n6\n")
        self.assertIn("more than 0", self.run_app("2\n6\n"))

    def test_eof_during_creation_does_not_save_partial_rule(self):
        self.assertIn("Session ended", self.run_app("1\n1\n1\n"))
        self.assertFalse(self.path.exists())
        self.run_app("1\n1\n2\n1\n")
        self.assertIn("#1 [enabled] billing", self.run_app("2\n6\n"))

    def test_malformed_files_are_not_overwritten(self):
        cases = [
            "", "OTHER_VERSION\n", "INTRADAY_RULES_V1\n1 1 backlog \"billing\"\n",
            "INTRADAY_RULES_V1\n1 1 backlog \"billing\" -1\n",
            "INTRADAY_RULES_V1\n1 1 sla \"unknown\"\n",
            "INTRADAY_RULES_V1\n1 1 sla \"billing\" trailing\n",
            "INTRADAY_RULES_V1\n1 2 sla \"billing\"\n",
            "INTRADAY_RULES_V1\n1 1 unknown \"billing\"\n",
            "INTRADAY_RULES_V1\n1 1 sla \"billing\"\n1 0 sla \"vip\"\n",
            "INTRADAY_RULES_V1\n0 1 sla \"billing\"\n",
        ]
        for content in cases:
            with self.subTest(content=content):
                self.path.write_text(content)
                self.run_app("1\n1\n2\n6\n", expected_code=1)
                self.assertEqual(self.path.read_text(), content)

    def test_save_failure_does_not_change_memory(self):
        missing = Path(self.storage.name) / "missing" / "rules.txt"
        output = self.run_app("1\n1\n2\n2\n6\n", path=missing)
        self.assertIn("Not applied:", output)
        self.assertIn("No rules configured yet", output)
        self.assertFalse(missing.exists())

    def test_replacement_failure_preserves_target(self):
        # Replace the target with a nonempty directory after startup to force
        # the final rename to fail, then check memory and cleanup behavior.
        directory = Path(self.storage.name) / "parent"
        directory.mkdir()
        path = directory / "rules.txt"
        self.run_app("1\n1\n2\n6\n", path=path)
        before = path.read_bytes()
        process = subprocess.Popen(
            [str(self.binary), str(path)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        # Wait until the startup menu's input prompt has flushed stdout.
        prefix = ""
        while not prefix.endswith("Choose action: "):
            character = process.stdout.read(1)
            if not character:
                self.fail("App exited before presenting its menu")
            prefix += character
        path.unlink()
        path.mkdir()
        (path / "preserved.txt").write_bytes(before)
        stdout, stderr = process.communicate("1\n3\n2\n2\n6\n", timeout=5)
        self.assertEqual(process.returncode, 0)
        self.assertIn("Not applied:", stderr)
        self.assertIn("#1 [enabled] billing", stdout)
        self.assertNotIn("#2 [enabled]", stdout)
        self.assertEqual((path / "preserved.txt").read_bytes(), before)
        self.assertEqual(list(directory.glob("rules.txt.tmp.*")), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
