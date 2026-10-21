#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import json
import logging
import shutil
import subprocess

import pyluwen
import pytest

from e2e_smoke import wait_arc_boot

# ruff: noqa: F811
from e2e_smoke import arc_chip_dut, launched_arc_dut  # noqa: F401

try:
    from e2e_smoke import unlaunched_dut  # noqa: F401
except ImportError:
    pass  # we should have it from twister


logger = logging.getLogger(__name__)

MAX_TEST_ITERATIONS = 1000

TDP_PATH = "chip_limits.tdp_limit"
ETH_SPEED_PATH = "eth_property_table.eth_speed_override"


def _run_bh_mod(bh_mod, *args):
    return subprocess.run([bh_mod, *args], capture_output=True, check=False)


def _describe(result):
    return (
        f"rc={result.returncode}; "
        f"stdout={result.stdout.decode(errors='replace')!r} "
        f"stderr={result.stderr.decode(errors='replace')!r}"
    )


def _smi_reset_after_bh_mod(board_name):
    """
    Reset the chip with tt-smi after a bh-mod write. bh-mod resets the chip
    itself, but only waits for it to reappear on PCIe, not for ARC to boot.
    """
    if board_name is not None and "galaxy" in board_name.lower():
        cmd = ["tt-smi", "-glx_reset", "--eth_train_skip"]
    else:
        cmd = ["tt-smi", "-r", "--eth_train_skip"]
    result = subprocess.run(cmd, capture_output=True, check=False)
    if result.returncode != 0:
        logger.warning(f"'{' '.join(cmd)}' after bh-mod failed, {_describe(result)}")


def _set_overrides(bh_mod, asic_id, board_name, tdp, speed):
    """
    Write the TDP and ETH speed overrides in one bh-mod call, reset the chip
    with tt-smi, then check ARC came back with both applied.
    """
    write = _run_bh_mod(bh_mod, "set", f"{TDP_PATH}={tdp}", f"{ETH_SPEED_PATH}={speed}")
    assert write.returncode == 0, f"bh-mod set failed, {_describe(write)}"
    _smi_reset_after_bh_mod(board_name)

    chip = wait_arc_boot(asic_id, timeout=60)
    measured_tdp = chip.get_telemetry().tdp_limit_max
    del chip  # So we don't hold stale file descriptors across the next reset
    assert measured_tdp == tdp, f"expected tdp_limit_max={tdp}, got {measured_tdp}"

    verify = _run_bh_mod(bh_mod, "get", "-f", "json", ETH_SPEED_PATH)
    assert verify.returncode == 0, f"bh-mod get failed, {_describe(verify)}"
    row = json.loads(verify.stdout.decode())[ETH_SPEED_PATH]
    assert str(row.get("override")) == str(speed), (
        f"expected override {ETH_SPEED_PATH}={speed}, got {row!r}"
    )


@pytest.mark.stress
def test_bh_mod_override_stress(arc_chip_dut, asic_id, board_name):
    """
    Write the TDP limit and ETH speed overrides with bh-mod repeatedly, and
    check that ARC comes back with both applied after every write. Each write
    is followed by a tt-smi reset, since bh-mod's own reset doesn't wait for
    ARC to boot.

    The TDP limit alternates between two values below the baseline, so every
    write changes flash and the telemetry check can't pass on the default.
    The ETH speed re-asserts the speed the board already trains at (200G on
    Galaxy, 400G elsewhere), so the ETH FW is never asked to retrain at a
    different speed.
    """
    bh_mod = shutil.which("bh-mod")
    if bh_mod is None:
        pytest.skip("bh-mod not found in PATH")

    chip = pyluwen.detect_chips()[asic_id]
    tdp_baseline = chip.get_telemetry().tdp_limit_max
    del chip
    tdp_targets = (tdp_baseline - 5, tdp_baseline - 10)

    is_galaxy = board_name is not None and "galaxy" in board_name.lower()
    speed = 200 if is_galaxy else 400

    test_name = "bh-mod override stress test"
    total_tries = min(MAX_TEST_ITERATIONS, 100)
    fail_count = 0
    try:
        for i in range(total_tries):
            if i % 10 == 0:
                logger.info(f"{test_name} iteration {i}/{total_tries}")
            try:
                _set_overrides(bh_mod, asic_id, board_name, tdp_targets[i % 2], speed)
            except AssertionError as e:
                logger.error(f"Failed iteration {i}: {e}")
                fail_count += 1
    finally:
        # Clear both overrides so the SPI flash goes back to inheriting from
        # cmfwcfg. Warn (don't fail) so a stale flash state is visible without
        # hiding the stress result.
        restore = _run_bh_mod(bh_mod, "res", TDP_PATH, ETH_SPEED_PATH)
        if restore.returncode != 0:
            logger.warning(
                f"Failed to clear overrides, SPI flash may be left modified: "
                f"{_describe(restore)}"
            )
        else:
            _smi_reset_after_bh_mod(board_name)
            wait_arc_boot(asic_id, timeout=60)

    logger.info(f"{test_name} completed. Failed {fail_count}/{total_tries} times.")
    assert fail_count == 0, f"{test_name} failed {fail_count}/{total_tries} times"
