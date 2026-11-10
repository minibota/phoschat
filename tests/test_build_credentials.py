#!/usr/bin/env python3
"""Offline regressions for credentials entering official package builds."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
TEST_ID = "12345"
TEST_HASH = "0123456789abcdef0123456789abcdef"


class BuildCredentialsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        module = (ROOT / "cmake/TelegramCredentials.cmake").as_posix()
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\n'
            'project(CredentialInputTest LANGUAGES NONE)\n'
            f'include("{module}")\n'
            'phoschat_configure_telegram_credentials("${PROJECT_BINARY_DIR}/generated")\n',
            encoding="utf-8")
        self.environment = dict(os.environ)
        self.environment.pop("PHOSCHAT_TELEGRAM_API_ID", None)
        self.environment.pop("PHOSCHAT_TELEGRAM_API_HASH", None)
        self.environment.pop("PHOSCHAT_SLACK_CLIENT_ID", None)
        self.environment.pop("PHOSCHAT_SLACK_OAUTH_ORIGIN", None)
        self.counter = 0

    def configure(self, api_id="", api_hash="", *arguments):
        self.counter += 1
        build = self.root / f"build-{self.counter}"
        environment = self.environment | {
            "PHOSCHAT_TELEGRAM_API_ID": api_id,
            "PHOSCHAT_TELEGRAM_API_HASH": api_hash,
        }
        result = subprocess.run(
            ["cmake", "-S", str(self.source), "-B", str(build), *arguments],
            env=environment, capture_output=True, text=True, check=False)
        return result, build

    def test_unbundled_default_and_required_missing(self):
        result, build = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        header = (build / "generated/telegram_build_credentials.h").read_text()
        self.assertIn('#define PHOSCHAT_TELEGRAM_API_ID 0', header)
        self.assertIn('#define PHOSCHAT_TELEGRAM_API_HASH ""', header)
        result, _ = self.configure("", "", "-DPHOSCHAT_REQUIRE_TELEGRAM_APP_CREDENTIALS=ON")
        self.assertNotEqual(result.returncode, 0)

    def test_valid_credentials_stay_out_of_logs_and_cache(self):
        result, build = self.configure(TEST_ID, TEST_HASH, "-DPHOSCHAT_REQUIRE_TELEGRAM_APP_CREDENTIALS=ON")
        self.assertEqual(result.returncode, 0, result.stderr)
        header = build / "generated/telegram_build_credentials.h"
        self.assertIn(TEST_HASH, header.read_text())
        self.assertNotIn(TEST_HASH, result.stdout + result.stderr)
        self.assertNotIn(TEST_HASH, (build / "CMakeCache.txt").read_text())
        if os.name == "posix":
            self.assertEqual(header.stat().st_mode & 0o777, 0o600)

    def test_partial_invalid_and_injected_values_are_rejected_without_echo(self):
        cases = [
            (TEST_ID, ""), ("", TEST_HASH), ("0", TEST_HASH), ("-1", TEST_HASH),
            ("2147483648", TEST_HASH), ("12345678901234567890", TEST_HASH),
            ("0123", TEST_HASH), ("1;2", TEST_HASH), ("123", "x" * 32),
            ("123", TEST_HASH + '"\n#error input'), ("123", TEST_HASH + "\n"),
        ]
        for api_id, api_hash in cases:
            with self.subTest(input_case=cases.index((api_id, api_hash))):
                result, _ = self.configure(api_id, api_hash)
                self.assertNotEqual(result.returncode, 0)
                if api_hash:
                    self.assertNotIn(api_hash, result.stdout + result.stderr)

    def test_json_input_and_mutually_exclusive_sources(self):
        private = self.source / "credentials.json"
        private.write_text(json.dumps({"api_id": TEST_ID, "api_hash": TEST_HASH}))
        argument = "-DPHOSCHAT_TELEGRAM_CREDENTIALS_FILE=credentials.json"
        result, build = self.configure("", "", argument, "-DPHOSCHAT_REQUIRE_TELEGRAM_APP_CREDENTIALS=ON")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(TEST_HASH, (build / "generated/telegram_build_credentials.h").read_text())
        result, _ = self.configure(TEST_ID, TEST_HASH, argument)
        self.assertNotEqual(result.returncode, 0)
        for invalid in ('invalid', '{}', '{"api_id":12345,"api_hash":"' + TEST_HASH + '"}'):
            private.write_text(invalid)
            result, _ = self.configure("", "", argument)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(TEST_HASH, result.stdout + result.stderr)

    def test_flatpak_manifest_does_not_contain_values(self):
        helper_path = ROOT / "packaging/prepare-telegram-build.py"
        spec = importlib.util.spec_from_file_location("telegram_build", helper_path)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        directory = self.root / "packaging/flatpak"
        directory.mkdir(parents=True)
        shutil.copy(ROOT / "packaging/flatpak/io.phoschat.desktop.json", directory)
        helper.prepare_flatpak(self.root, {"api_id": TEST_ID, "api_hash": TEST_HASH})
        manifest_text = (directory / "io.phoschat.desktop.private.json").read_text()
        self.assertNotIn(TEST_ID, manifest_text)
        self.assertNotIn(TEST_HASH, manifest_text)
        manifest = json.loads(manifest_text)
        app = next(module for module in manifest["modules"] if module["name"] == "phoschat")
        self.assertIn("-DPHOSCHAT_REQUIRE_TELEGRAM_APP_CREDENTIALS=ON", app["config-opts"])
        private = self.root / ".phoschat-build-credentials.json"
        self.assertEqual(json.loads(private.read_text()), {"api_id": TEST_ID, "api_hash": TEST_HASH})
        if os.name == "posix":
            self.assertEqual(private.stat().st_mode & 0o777, 0o600)

    def test_preflight_rejects_missing_credentials_without_leaking(self):
        result = subprocess.run(
            ["python3", str(ROOT / "packaging/prepare-telegram-build.py")],
            env=self.environment, capture_output=True, text=True, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Check repository secrets", result.stderr)
        environment = self.environment | {"PHOSCHAT_TELEGRAM_API_ID": TEST_ID,
                                          "PHOSCHAT_TELEGRAM_API_HASH": TEST_HASH}
        result = subprocess.run(
            ["python3", str(ROOT / "packaging/prepare-telegram-build.py")],
            env=environment, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout + result.stderr, "")

    def test_public_slack_id_reaches_flatpak_without_changing_private_credentials(self):
        helper_path = ROOT / "packaging/prepare-telegram-build.py"
        spec = importlib.util.spec_from_file_location("slack_build", helper_path)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        directory = self.root / "packaging/flatpak"
        directory.mkdir(parents=True)
        shutil.copy(ROOT / "packaging/flatpak/io.phoschat.desktop.json", directory)
        helper.prepare_flatpak(self.root, {"api_id": TEST_ID, "api_hash": TEST_HASH}, "12345.67890",
                               "https://oauth.example.test")
        manifest = (directory / "io.phoschat.desktop.private.json").read_text()
        self.assertIn("-DPHOSCHAT_SLACK_CLIENT_ID=12345.67890", manifest)
        self.assertIn("-DPHOSCHAT_SLACK_OAUTH_ORIGIN=https://oauth.example.test", manifest)
        self.assertNotIn(TEST_HASH, manifest)


class SlackBuildConfigurationTest(unittest.TestCase):
    def test_optional_public_id_and_invalid_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source"
            source.mkdir()
            module = (ROOT / "cmake/SlackApplication.cmake").as_posix()
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.22)\n'
                'project(SlackPublicIdTest LANGUAGES NONE)\n'
                f'include("{module}")\n'
                'phoschat_configure_slack_application("${PROJECT_BINARY_DIR}/generated")\n')
            for index, value in enumerate(("", "12345.67890", "invalid", "123.456\";evil", "1." + "1" * 33)):
                with self.subTest(value=value):
                    build = Path(temporary) / str(index)
                    result = subprocess.run(
                        ["cmake", "-S", str(source), "-B", str(build)],
                        env=dict(os.environ) | {"PHOSCHAT_SLACK_CLIENT_ID": value,
                                                "PHOSCHAT_SLACK_OAUTH_ORIGIN": ""},
                        capture_output=True, text=True, check=False)
                    self.assertEqual(result.returncode == 0, index < 2, result.stderr)
                    if index < 2:
                        self.assertIn(f'#define PHOSCHAT_SLACK_CLIENT_ID "{value}"',
                                      (build / "generated/slack_build_config.h").read_text())

    def test_public_https_origin_validation_matches_flatpak(self):
        helper_path = ROOT / "packaging/prepare-telegram-build.py"
        spec = importlib.util.spec_from_file_location("slack_origin_build", helper_path)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        valid = ("", "https://oauth.example.test", "https://a.b", "https://xn--bcher-kva.example",
                 "https://" + ".".join(["a" * 63, "b" * 63, "c" * 63, "d" * 61]))
        invalid = ("http://oauth.example.test", "https://localhost", "https://127.0.0.1",
                   "https://[::1]", "https://example.test/", "https://example.test/callback",
                   "https://user@example.test", "https://example.test:443", "https://example.test?q=1",
                   "https://example.test#fragment", "https://a..test", "https://-a.example",
                   "https://a-.example", "https://a.example.", "https://a.123", "https://a.example\n",
                   "https://a.example;injected", "https://" + "a" * 64 + ".test",
                   "https://" + ".".join(["a" * 63, "b" * 63, "c" * 63, "d" * 62]))
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source"
            source.mkdir()
            module = (ROOT / "cmake/SlackApplication.cmake").as_posix()
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.22)\n'
                'project(SlackOriginTest LANGUAGES NONE)\n'
                f'include("{module}")\n'
                'phoschat_configure_slack_application("${PROJECT_BINARY_DIR}/generated")\n')
            cases = [(origin, "12345.67890", origin in valid) for origin in valid + invalid]
            cases.append(("https://oauth.example.test", "", False))
            for index, (origin, client_id, accepted) in enumerate(cases):
                with self.subTest(origin=origin, client_id=client_id):
                    environment = {"PHOSCHAT_SLACK_CLIENT_ID": client_id,
                                   "PHOSCHAT_SLACK_OAUTH_ORIGIN": origin}
                    with patch.dict(os.environ, environment):
                        if accepted:
                            self.assertEqual(helper.slack_oauth_origin_from_environment(client_id), origin)
                        else:
                            with self.assertRaises(ValueError):
                                helper.slack_oauth_origin_from_environment(client_id)
                    build = Path(temporary) / str(index)
                    result = subprocess.run(
                        ["cmake", "-S", str(source), "-B", str(build)],
                        env=dict(os.environ) | environment,
                        capture_output=True, text=True, check=False)
                    self.assertEqual(result.returncode == 0, accepted, result.stderr)
                    if accepted:
                        self.assertIn(f'#define PHOSCHAT_SLACK_OAUTH_ORIGIN "{origin}"',
                                      (build / "generated/slack_build_config.h").read_text())


if __name__ == "__main__":
    unittest.main()
