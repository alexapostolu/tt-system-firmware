#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""
Regenerate the signed MCUBoot and recovery blobs from a tt-system-firmware
release.

The MCUBoot and recovery images under `zephyr/blobs/` are not built from source
They are the officially signed images published on the GitHub releases page.
This script pulls a given release, extracts those images from each board's bundle,
and writes them back out as `zephyr/blobs/tt_blackhole_{mcuboot,recovery}_<BOARD>.bin`.

For each board the `<board>.fwbundle` release asset is downloaded and its
tt-boot-fs image is parsed. The `cmfw` entry is the signed MCUBoot bootloader
and the `safeimg` entry is the signed recovery image.

Usage:
    # Regenerate all board blobs from a release
    python scripts/fetch_boot_blobs.py 19.12.0-rc2

    # Regenerate and update the sha256/version fields in zephyr/module.yml too
    python scripts/fetch_boot_blobs.py 19.12.0-rc2 --write
"""

from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tt_boot_fs

_logger = logging.getLogger(__name__)

REPO_ROOT = Path(__file__).resolve().parents[1]
MODULE_YAML = REPO_ROOT / "zephyr" / "module.yml"
REPO = "tenstorrent/tt-system-firmware"
HTTP_TIMEOUT_S = 60

MCUBOOT_TAG = "cmfw"
RECOVERY_TAG = "safeimg"


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def get_board_assets(repo: str, release: str) -> dict[str, str]:
    """Return board name -> download URL for per-board fwbundles in a release."""
    url = f"https://api.github.com/repos/{repo}/releases/tags/v{release}"
    try:
        with urllib.request.urlopen(url, timeout=HTTP_TIMEOUT_S) as resp:
            release_data = json.load(resp)
    except urllib.error.HTTPError as e:
        raise RuntimeError(
            f"failed to inspect release {release} ({e.code} {e.reason}); "
            "does the release exist?"
        ) from e

    assets = {}
    for asset in release_data.get("assets", []):
        name = asset.get("name", "")
        # The release also contains a combined fw_pack-<version>.fwbundle.
        if name.endswith(".fwbundle") and not name.startswith("fw_pack-"):
            assets[Path(name).stem] = asset["browser_download_url"]

    if not assets:
        raise RuntimeError(f"release {release} has no per-board fwbundle assets")
    return dict(sorted(assets.items()))


def download_asset(url: str, dest: Path) -> None:
    _logger.info("Downloading %s", url)
    try:
        with urllib.request.urlopen(url, timeout=HTTP_TIMEOUT_S) as resp, open(
            dest, "wb"
        ) as f:
            shutil.copyfileobj(resp, f)
    except urllib.error.HTTPError as e:
        raise RuntimeError(
            f"failed to download {Path(url).name} ({e.code} {e.reason})"
        ) from e


def extract_entry(image: Path, tag: str, out_path: Path) -> None:
    if out_path.exists():
        out_path.unlink()
    tt_boot_fs.extract(image, tag, out_path, input_base64=True)
    if not out_path.exists() or out_path.stat().st_size == 0:
        raise RuntimeError(f"failed to extract '{tag}' from {image}")


def load_module_sha_map() -> dict[str, str]:
    """Map blob filename -> expected sha256 from zephyr/module.yml."""
    data = yaml.safe_load(MODULE_YAML.read_text())
    result: dict[str, str] = {}
    for blob in data.get("blobs", []):
        path = blob.get("path")
        sha = blob.get("sha256")
        if path and sha:
            result[Path(path).name] = sha
    return result


def update_module_yaml(
    module_yml: Path, digests: dict[str, str], release: str
) -> list[str]:
    """Rewrite the ``sha256`` and ``version`` of the given blobs in module.yml.

    Only the lines belonging to blobs present in ``digests`` are touched, so
    comments, ordering, and formatting of the rest of the file are preserved.
    The ``version`` field is set to ``release``, and any ``from v<version>
    release`` marker in the ``description`` is kept in sync.

    Returns the sorted list of blob names that were updated.
    """
    lines = module_yml.read_text().splitlines(keepends=True)
    out: list[str] = []
    updated: set[str] = set()
    current: str | None = None
    for line in lines:
        path_match = re.match(r"^(\s*)-\s+path:\s+(\S+)\s*$", line)
        if path_match:
            name = Path(path_match.group(2)).name
            current = name if name in digests else None
            out.append(line)
            continue
        if current is not None:
            sha_match = re.match(r"^(\s*)sha256:\s+\S+\s*$", line)
            if sha_match:
                out.append(f"{sha_match.group(1)}sha256: {digests[current]}\n")
                updated.add(current)
                continue
            ver_match = re.match(r"^(\s*)version:\s+.*$", line)
            if ver_match:
                out.append(f"{ver_match.group(1)}version: '{release}'\n")
                continue
            desc_match = re.match(
                r'^(\s*description:\s+".*?from )v\S+( release.*)$', line
            )
            if desc_match:
                out.append(f"{desc_match.group(1)}v{release}{desc_match.group(2)}\n")
                continue
        out.append(line)
    module_yml.write_text("".join(out))
    return sorted(updated)


def process_board(
    board: str,
    asset_url: str,
    blobs_dir: Path,
    sha_map: dict[str, str],
    computed: dict[str, str],
) -> bool:
    """Regenerate the two blobs for one board. Returns True on success.

    The freshly computed ``filename -> sha256`` mapping is recorded in
    ``computed`` so the caller can optionally write it back to module.yml.
    """
    suffix = board.upper()
    mcuboot_name = f"tt_blackhole_mcuboot_{suffix}.bin"
    recovery_name = f"tt_blackhole_recovery_{suffix}.bin"

    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        bundle = tmp_dir / f"{board}.fwbundle"
        download_asset(asset_url, bundle)

        extract_root = tmp_dir / "bundle"
        extract_root.mkdir()
        with tarfile.open(bundle, "r:gz") as tar:
            tar.extractall(extract_root, filter="data")

        # A bundle contains one <board>/image.bin per ASIC. MCUBoot and the recovery
        # image are identical across a board's ASICs, so the first image is sufficient.
        images = sorted(extract_root.rglob("image.bin"))
        if not images:
            raise RuntimeError(f"{board}.fwbundle contains no image.bin")
        image = images[0]

        targets = [
            (MCUBOOT_TAG, blobs_dir / mcuboot_name),
            (RECOVERY_TAG, blobs_dir / recovery_name),
        ]

        ok = True
        for entry_tag, out_path in targets:
            extract_entry(image, entry_tag, out_path)
            digest = sha256_of(out_path)
            computed[out_path.name] = digest
            expected = sha_map.get(out_path.name)
            status = ""
            if expected is not None:
                if digest == expected:
                    status = " (matches module.yml)"
                else:
                    status = f" (MISMATCH, module.yml has {expected})"
                    ok = False
            _logger.info("  %-40s %s%s", out_path.name, digest, status)
        return ok


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False,
    )
    parser.add_argument(
        "release",
        help="Release version to fetch blobs from (for example, 19.12.0).",
    )
    parser.add_argument(
        "--blobs-dir",
        type=Path,
        default=REPO_ROOT / "zephyr" / "blobs",
        help="Output directory for the blobs (default: zephyr/blobs).",
    )
    parser.add_argument(
        "--write",
        "--update-module",
        dest="write",
        action="store_true",
        help="Write the regenerated sha256 and version back into zephyr/module.yml "
        "instead of only reporting mismatches.",
    )
    return parser.parse_args()


def main() -> int:
    logging.basicConfig(format="%(message)s", level=logging.INFO)
    args = parse_args()

    args.blobs_dir.mkdir(parents=True, exist_ok=True)
    sha_map = load_module_sha_map()

    try:
        board_assets = get_board_assets(REPO, args.release)
    except Exception as e:  # noqa: BLE001
        _logger.error("Failed: %s", e)
        return os.EX_SOFTWARE

    computed: dict[str, str] = {}
    mismatches: list[str] = []
    for board, asset_url in board_assets.items():
        _logger.info("[%s]", board)
        try:
            if not process_board(
                board,
                asset_url,
                args.blobs_dir,
                sha_map,
                computed,
            ):
                mismatches.append(board)
        except Exception as e:  # noqa: BLE001
            _logger.error("  failed: %s", e)
            return os.EX_SOFTWARE

    if args.write:
        updated = update_module_yaml(MODULE_YAML, computed, args.release)
        _logger.info("Updated %d blob entries in %s", len(updated), MODULE_YAML)
        return os.EX_OK

    if mismatches:
        _logger.error(
            "Generated blobs differ from module.yml for: %s",
            ", ".join(mismatches),
        )
        return os.EX_SOFTWARE
    _logger.info("Done. Wrote blobs to %s", args.blobs_dir)
    return os.EX_OK


if __name__ == "__main__":
    sys.exit(main())
