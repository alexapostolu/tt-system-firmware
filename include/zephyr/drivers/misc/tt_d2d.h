/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_TT_D2D_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_TT_D2D_H_

/**
 * @file
 * @brief Tenstorrent Grendel die-to-die (D2D) firmware loading APIs
 *
 * Sequence: reset_release (before the CCE PLL switch, straps sample the
 * current clock) -> load_fw (Rocket stays halted) -> start (both ends close
 * together; sideband sync is off on emu) -> wait_link.
 *
 * Cold reset and firewalls are chip-wide and are not done here. Until they
 * are, load_fw() fails the SRAM probe with -ENODEV.
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

/**
 * @brief Deassert a D2D tile's subsystem resets, in hardware order.
 *
 * @param dev D2D tile device
 *
 * @retval 0 on success
 */
int tt_d2d_reset_release(const struct device *dev);

/**
 * @brief Load firmware into a D2D tile, leaving the Rocket halted.
 *
 * @param dev D2D tile device
 * @param img Firmware image
 * @param img_size Size of @p img in bytes. Multiple of 4; must not overrun
 *                 the configuration block at the top of SRAM.
 *
 * @retval 0 on success
 * @retval -EINVAL if @p img is NULL, empty, or not a multiple of 4 bytes
 * @retval -ENOSPC if @p img would overrun the configuration block
 * @retval -ENODEV if the tile does not answer a write to its SRAM
 * @retval -EIO if the image read back from SRAM does not match @p img
 */
int tt_d2d_load_fw(const struct device *dev, const uint8_t *img, size_t img_size);

/**
 * @brief Release a D2D tile's Rocket so loaded firmware begins executing.
 *
 * @param dev D2D tile device
 *
 * @retval 0 on success
 */
int tt_d2d_start(const struct device *dev);

/**
 * @brief Wait for this end of the link to finish training.
 *
 * Polls the firmware progress code in SRAM (link-layer status is not modelled
 * on emu). Both ends must already have been started.
 *
 * @param dev D2D tile device
 * @param timeout How long to wait, or K_FOREVER
 *
 * @retval 0 if the link trained
 * @retval -EIO if the firmware reported a training failure
 * @retval -ETIMEDOUT if it did neither
 */
int tt_d2d_wait_link(const struct device *dev, k_timeout_t timeout);

/**
 * @brief Firmware training progress code (drop d2d_api_fw_progress_codes.h).
 *
 * @param dev D2D tile device
 *
 * @return the firmware's current progress code, or 0 if none yet
 */
uint32_t tt_d2d_progress_code(const struct device *dev);

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_TT_D2D_H_ */
