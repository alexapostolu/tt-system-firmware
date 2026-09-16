/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TT_D2D_TEST_CHIP_INIT_H_
#define TT_D2D_TEST_CHIP_INIT_H_

/**
 * @brief Lift D2D cold resets and open firewalls (chip-wide; BL1's job in prod).
 *
 * On Keraunos this also brings up the HSIO tile the D2D sits behind.
 *
 * @return 0 on success, -EIO if the drop rejected a filter configuration.
 */
int tt_d2d_test_chip_init(void);

/**
 * @brief Switch the CCE clock from the reference to the PLL.
 *
 * After strap release on Mimir; already done inside chip_init on Keraunos
 * (HSIO must be clocked before the straps answer). Callers use this order
 * on both platforms.
 *
 * @return 0 on success.
 */
int tt_d2d_test_chip_clock_up(void);

#endif /* TT_D2D_TEST_CHIP_INIT_H_ */
