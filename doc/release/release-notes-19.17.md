# v19.17.0

We are pleased to announce the release of TT System Firmware version 19.17.0 🥳🎉.

Major enhancements with this release include:

## What's Changed

## Blackhole

### Persistent SPI Flash Parameters

- Expose `chip_limits.max_tdp_limit` to `bh-mod`. This is the ceiling in
  watts for a host-requested TDP limit (`TT_SMC_MSG_SET_TDP_LIMIT`, e.g. via
  `tt-smi`); firmware clamps it to `[chip_limits.tdp_limit, 500]`, so a value
  at or below `tdp_limit` only lets the host lower the limit. It does not
  change the boot-time `tdp_limit`. `bh-mod res` restores the cmfwcfg value.

## Migration guide

An overview of required and recommended changes to make when migrating from the previous v19.16.0 release can be found in [19.17 Migration Guide](https://github.com/tenstorrent/tt-system-firmware/tree/main/doc/release/migration-guide-19.17.md).

## Full ChangeLog

The full ChangeLog from the previous v19.16.0 release can be found at the link below.

https://github.com/tenstorrent/tt-system-firmware/compare/v19.16.0...v19.17.0
