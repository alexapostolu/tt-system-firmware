# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""Offline checks for the prebuilt, signed boot blobs (MCUBoot + recovery).

The ``cmfw`` (MCUBoot) and ``safeimg`` (recovery) images are not built from
source. They are the officially signed artifacts published on a release and
pulled in by ``scripts/fetch_boot_blobs.py``. These tests guard that the
checked-in blobs stay consistent with ``zephyr/module.yml`` and are
structurally valid, so a corrupted, truncated, or unregistered blob is caught
in CI rather than at flash time.
"""

import hashlib
import re
import struct
from pathlib import Path

import pytest
import yaml

TEST_ROOT = Path(__file__).parent.resolve()
MODULE_ROOT = TEST_ROOT.parents[4]
BLOBS_DIR = MODULE_ROOT / "zephyr" / "blobs"
MODULE_YAML = MODULE_ROOT / "zephyr" / "module.yml"

BOARD_DIR = MODULE_ROOT / "boards" / "tenstorrent" / "tt_blackhole"
PARTITIONS_DTSI = BOARD_DIR / "tt_blackhole_fixed_partitions.dtsi"
PREFLASH_YAML = BOARD_DIR / "bootfs" / "preflash-bootfs.yaml"

# MCUBoot image header magic (little-endian uint32 at offset 0 of a signed image).
MCUBOOT_IMAGE_MAGIC = 0x96F3B83D

BOOT_BLOB_RE = re.compile(r"tt_blackhole_(mcuboot|recovery)_[A-Z0-9_]+\.bin")


def _module_blob_entries():
    data = yaml.safe_load(MODULE_YAML.read_text())
    return [
        blob
        for blob in data.get("blobs", [])
        if BOOT_BLOB_RE.fullmatch(Path(blob["path"]).name)
    ]


def _boot_blob_files():
    return sorted(
        path for path in BLOBS_DIR.glob("*.bin") if BOOT_BLOB_RE.fullmatch(path.name)
    )


_MODULE_ENTRIES = _module_blob_entries()
_RECOVERY_FILES = [
    p for p in _boot_blob_files() if p.name.startswith("tt_blackhole_recovery_")
]
_MCUBOOT_FILES = [
    p for p in _boot_blob_files() if p.name.startswith("tt_blackhole_mcuboot_")
]


def test_boot_blobs_registered():
    """The MCUBoot and recovery blobs must be registered in module.yml."""
    assert _MODULE_ENTRIES, "no MCUBoot/recovery blobs registered in zephyr/module.yml"


@pytest.mark.parametrize(
    "entry", _MODULE_ENTRIES, ids=[e["path"] for e in _MODULE_ENTRIES]
)
def test_boot_blob_sha256_matches_manifest(entry):
    """Each registered boot blob exists and its sha256 matches the manifest.

    Stricter than scripts/verify_blob.py, which silently skips entries whose
    file is missing.
    """
    path = BLOBS_DIR / entry["path"]
    assert path.is_file(), f"blob {entry['path']} referenced in module.yml is missing"
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    assert digest == entry["sha256"], (
        f"{entry['path']} sha256 {digest} != module.yml {entry['sha256']}"
    )


def test_every_boot_blob_on_disk_is_registered():
    """A boot blob checked in without a module.yml entry would ship unverified."""
    registered = {entry["path"] for entry in _MODULE_ENTRIES}
    on_disk = {path.name for path in _boot_blob_files()}
    unregistered = sorted(on_disk - registered)
    assert not unregistered, f"boot blobs missing from module.yml: {unregistered}"


@pytest.mark.parametrize("path", _RECOVERY_FILES, ids=[p.name for p in _RECOVERY_FILES])
def test_recovery_blob_is_signed_mcuboot_image(path):
    """Recovery images are MCUBoot-signed: valid header magic and sane sizes."""
    data = path.read_bytes()
    assert len(data) >= 16, f"{path.name} too small to contain an MCUBoot header"
    magic, _load_addr, hdr_size, protect_tlv_size, img_size = struct.unpack_from(
        "<IIHHI", data, 0
    )
    assert magic == MCUBOOT_IMAGE_MAGIC, (
        f"{path.name} does not start with the MCUBoot image magic; "
        "it may not be a signed recovery image"
    )
    # Header + payload (+ optional protected TLVs) must fit within the file,
    # leaving room for the signature TLV trailer.
    assert hdr_size + img_size + protect_tlv_size <= len(data), (
        f"{path.name} header/image sizes ({hdr_size}+{img_size}+{protect_tlv_size}) "
        f"exceed file length {len(data)}"
    )


@pytest.mark.parametrize("path", _MCUBOOT_FILES, ids=[p.name for p in _MCUBOOT_FILES])
def test_mcuboot_blob_is_nonempty_binary(path):
    """The MCUBoot bootloader is a raw image; ensure it isn't empty or erased."""
    data = path.read_bytes()
    assert len(data) > 1024, f"{path.name} is implausibly small for a bootloader"
    head = set(data[:256])
    assert head not in ({0xFF}, {0x00}), f"{path.name} looks blank/erased"


def test_boot_critical_partitions_point_at_blobs():
    """Regression guard: cmfw/safeimg/failover must load the prebuilt blobs.

    If any of these revert to a ``$BUILD_DIR`` source build, the boot-critical
    images would no longer be the officially signed release artifacts.
    """
    dtsi = PARTITIONS_DTSI.read_text()
    expected = [
        ('label = "cmfw"', "$BLOBS_DIR/tt_blackhole_mcuboot_$PROD_NAME.bin"),
        ('label = "safeimg"', "$BLOBS_DIR/tt_blackhole_recovery_$PROD_NAME.bin"),
        ('label = "failover"', "$BLOBS_DIR/tt_blackhole_mcuboot_$PROD_NAME.bin"),
    ]
    for label, blob_path in expected:
        assert label in dtsi, f"{PARTITIONS_DTSI.name}: missing partition {label}"
        assert blob_path in dtsi, (
            f"{PARTITIONS_DTSI.name}: expected {label} to reference {blob_path}"
        )

    preflash = yaml.safe_load(PREFLASH_YAML.read_text())
    by_name = {img["name"]: img for img in preflash["images"]}
    by_name[preflash["fail_over_image"]["name"]] = preflash["fail_over_image"]
    assert "blobs/tt_blackhole_mcuboot" in by_name["cmfw"]["binary"]
    assert "blobs/tt_blackhole_recovery" in by_name["recovery"]["binary"]
    assert "blobs/tt_blackhole_mcuboot" in by_name["failover"]["binary"]
