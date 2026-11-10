#!/usr/bin/env python3
"""Validate official app credentials, optionally stage them for a Flatpak build.

Read values only from the environment. The private JSON is a source input copied
into Flatpak's build sandbox; only its path appears in the generated manifest.
Neither file belongs in release artifacts or version control.
"""

import argparse
import json
import os
from pathlib import Path
import re
import tempfile


def credentials_from_environment():
    api_id = os.environ.get("PHOSCHAT_TELEGRAM_API_ID", "")
    api_hash = os.environ.get("PHOSCHAT_TELEGRAM_API_HASH", "")
    if not re.fullmatch(r"[1-9][0-9]{0,9}", api_id) or int(api_id) > 2147483647:
        raise ValueError("Configure valid TELEGRAM_API_ID and TELEGRAM_API_HASH repository secrets.")
    if not re.fullmatch(r"[0-9a-fA-F]{32}", api_hash):
        raise ValueError("Configure valid TELEGRAM_API_ID and TELEGRAM_API_HASH repository secrets.")
    return {"api_id": api_id, "api_hash": api_hash}


def private_write(path, data):
    descriptor, temporary = tempfile.mkstemp(dir=path.parent, prefix=path.name + ".")
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(data)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def prepare_flatpak(root, credentials):
    manifest_path = root / "packaging/flatpak/io.phoschat.desktop.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    module = next(item for item in manifest["modules"] if item.get("name") == "phoschat")
    module["config-opts"] += [
        "-DPHOSCHAT_REQUIRE_TELEGRAM_APP_CREDENTIALS=ON",
        "-DPHOSCHAT_TELEGRAM_CREDENTIALS_FILE=.phoschat-build-credentials.json",
    ]
    # Sources and the generated header stay in /run/build, never under /app.
    # The source-only key file is deleted once PhosChat has been installed.
    module.setdefault("post-install", []).append(
        "rm -f /run/build/phoschat/.phoschat-build-credentials.json")
    private_write(root / ".phoschat-build-credentials.json", json.dumps(credentials) + "\n")
    private_write(manifest_path.with_name("io.phoschat.desktop.private.json"),
                  json.dumps(manifest, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flatpak", action="store_true", help="stage a private Flatpak manifest")
    arguments = parser.parse_args()
    try:
        credentials = credentials_from_environment()
        if arguments.flatpak:
            prepare_flatpak(Path(__file__).resolve().parents[1], credentials)
    except (ValueError, OSError, KeyError, StopIteration):
        parser.exit(1, "Official Telegram build setup failed. Check repository secrets and build inputs.\n")


if __name__ == "__main__":
    main()
