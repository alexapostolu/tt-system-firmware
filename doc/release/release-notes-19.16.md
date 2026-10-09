# v19.16.0

We are pleased to announce the release of TT System Firmware version 19.16.0 🥳🎉.

Major enhancements with this release include:

- New Blackhole board revision: Galaxy CF
- Enhanced boot firmware table layout for improved robustness during firmware updates.

## What's Changed

## Blackhole

### Boards

- New board revision: Galaxy CF (`tt_blackhole@galaxy_cf`, board type `0x202`).

### Reliability

- Added GDDR CA latch-and-retest functionality (SYS-5042) to improve GDDR initialization reliability on Blackhole.
- Hardened the SPI firmware table load that gates PCIe bring-up:
  - The SPI RX sample delay is now trained before the firmware tables are read
    (`CONFIG_FLASH_TRAINING_PRIORITY` 88, ahead of `CONFIG_BH_FWTABLE_INIT_PRIORITY`),
    so the boot-critical tables are no longer the first reads at the full read
    frequency with an untrained sample point.
  - Each table load is retried (`CONFIG_BH_FWTABLE_LOAD_ATTEMPTS`, default 3) instead
    of failing the boot on a single bad SPI read.
  - The boot filesystem walk no longer accepts an arbitrary read with the invalid bit set
    as the end-of-table sentinel, and rejects all-zero descriptors, so a flash that did
    not answer a read fails loudly rather than reporting an empty table.
  - A failed table load now sets bit `INIT_STAGE_FWTABLE` (5) in `STATUS_ERROR_STATUS0`.
    Boot filesystem read diagnostics are published in `SCRATCH_RAM[30..31]` and the
    trained RX sample delay windows in `SCRATCH_RAM[32]` (see `status_reg.h`), readable
    over JTAG on a chip whose PCIe link never came up.

### Ethernet

- Manual EQ now recovers when the link partner loses signal mid-training
  (e.g. partner reset) instead of failing; removed the 2 s post-sigdet.
- Fixed retrain and train_status bugs (successful retrain treated as a
  failure, loopback statuses overwritten).

### PCIe

- Request per-rate PCIe EQ presets from the firmware table.
- Request different presets on Galaxy systems vs PCIe cards (SYS-5101).

## Boot & Firmware

### MCUBoot

- Derived MCUBoot RAM load window from device tree configuration for improved flexibility.

### Drivers

- **Serial/VUART**: Implemented placeable VUART driver for flexible virtual UART placement.
- **Remoteproc**: Split remoteproc load and execute operations for better control and test isolation.

## Libraries

- **ARC Library**: Added DVFS loop counters (dropped ticks, maximum period, and maximum pass duration) to support power analysis.
- **FW Common**: Enabled TT_FW_COMMON library compilation for mission FW.

## Bug Fixes & Improvements

- Fixed typos in code comments and Doxygen markers throughout the codebase.
- Fixed SPI-NOR flash (nv_flash) size configuration.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.15.0 release can be found in [19.16 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.16.md).

## Full ChangeLog

The full ChangeLog from the previous v19.15.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.15.0...v19.16.0
