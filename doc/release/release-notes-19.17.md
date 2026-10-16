# v19.17.0

> This is a working draft for the up-coming 19.17.0 release.

We are pleased to announce the release of TT System Firmware version 19.17.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Blackhole

### Ethernet

- Updated Blackhole ERISC FW to v1.13.1, change list from v1.13.0:
  - UBB retimer ports now auto-train (speed=0) at 330G instead of 200G.
  - Reduced the default DFE ratio for UBB QSFP-DD ports to remove uncorrected errors.
  - Increased the instruction cache flush before handing the ERISC core to tt-metal, to avoid running stale instructions.
  - SerDes register reads now update the shared NOC read counters, so tt-metal's NOC barriers stay in sync.
  - Added an execution-stage breadcrumb (`eth_status.eth_fw_stage`), shared with tt-metal, showing what the Ethernet firmware is currently doing.

### Persistent SPI Flash Parameters

- Add `feature_enable.bypass_cable_fault_detection` to the firmware table and
  expose it to `bh-mod`. When set, the SMC skips the boot-time cable fault
  check (the 12V-2x6 power limit the DMC reports), so a 0 W limit no longer
  puts the chip into low-power cable fault mode. Off in every board table;
  intended for bring-up and debug. `bh-mod res` restores the cmfwcfg value.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.16.0 release can be found in [19.17 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.17.md).

## Full ChangeLog

The full ChangeLog from the previous v19.16.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.16.0...v19.17.0
