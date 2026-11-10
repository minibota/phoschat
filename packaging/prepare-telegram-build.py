#!/usr/bin/env python3
"""Validate app build settings, optionally stage them for a Flatpak build.

Read values only from the environment. The private JSON is a source input copied
into Flatpak's build sandbox; only its path appears in the generated manifest.
Neither file belongs in release artifacts or version control.
Public Slack app settings may be included directly in the generated manifest.
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


def slack_client_id_from_environment():
    client_id = os.environ.get("PHOSCHAT_SLACK_CLIENT_ID", "")
    if client_id and not re.fullmatch(r"[0-9]{1,32}\.[0-9]{1,32}", client_id):
        raise ValueError("Configure a valid public SLACK_CLIENT_ID repository variable.")
    return client_id


def slack_oauth_origin_from_environment(client_id):
    origin = os.environ.get("PHOSCHAT_SLACK_OAUTH_ORIGIN", "")
    if not origin:
        return ""
    host = origin.removeprefix("https://")
    labels = host.split(".")
    label_pattern = r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
    if (not client_id or not origin.startswith("https://") or len(host) > 253
            or len(labels) < 2 or not re.fullmatch(r"[A-Za-z].*", labels[-1])
            or any(not re.fullmatch(label_pattern, label) for label in labels)):
        raise ValueError("Configure a bare public HTTPS origin and SLACK_CLIENT_ID repository variables.")
    return origin


def prepare_flatpak(root, credentials, slack_client_id="", slack_oauth_origin=""):
    manifest_path = root / "packaging/flatpak/io.phoschat.desktop.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    module = next(item for item in manifest["modules"] if item.get("name") == "phoschat")
    module["config-opts"] += [
        "-DPHOSCHAT_REQUIRE_TELEGRAM_APP_CREDENTIALS=ON",
        "-DPHOSCHAT_TELEGRAM_CREDENTIALS_FILE=.phoschat-build-credentials.json",
    ]
    if slack_client_id:
        # Public app identity may enter the manifest; OAuth secrets never do.
        module["config-opts"].append("-DPHOSCHAT_SLACK_CLIENT_ID=" + slack_client_id)
    if slack_oauth_origin:
        module["config-opts"].append("-DPHOSCHAT_SLACK_OAUTH_ORIGIN=" + slack_oauth_origin)
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
        slack_client_id = slack_client_id_from_environment()
        slack_oauth_origin = slack_oauth_origin_from_environment(slack_client_id)
        if arguments.flatpak:
            prepare_flatpak(Path(__file__).resolve().parents[1], credentials, slack_client_id,
                            slack_oauth_origin)
    except (ValueError, OSError, KeyError, StopIteration):
        parser.exit(1, "Official build setup failed. Check repository secrets, public app variables and build inputs.\n")


if __name__ == "__main__":
    main()
