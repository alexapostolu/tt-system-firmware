/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Drop basic_init/firewall/clock, not restated here. Mimir D2D is on the
 * management network; Keraunos D2D sits inside an HSIO tile that must be up
 * first.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include <basic_init.h>
#include <clock.h>
#include <firewall.h>
#include <platform.h>

#include "chip_init.h"

#if defined(CONFIG_SOC_TT_KERAUNOS_SMC)

#include <hsio_tile_init.h>

/* MMK K2M link is HSIO/D2D tile 2. Ring tiles 0..N must all be out of reset. */
#define KER_LINK_HSIO_TILE 2U

#define KER_HSIO_RESET_MASK                                                                        \
	(KER_RST_HSIO0 | (KER_RST_HSIO0 << 1) | (KER_RST_HSIO0 << KER_LINK_HSIO_TILE))

int tt_d2d_test_chip_init(void)
{
	grendel_err_t err;

	config_cce_clock(0);

	smc_release_reset(KER_RST_SMN | KER_HSIO_RESET_MASK | KER_RST_D2D2);

	enable_hsio_clk(KER_LINK_HSIO_TILE, 0);

	/* Not disable_ker_smn_filters(): that also walks PCIe filters, and
	 * PCIe is still in cold reset here.
	 */
	err = disable_ker_smc_filters();
	if (err != GRENDEL_ERR_OK) {
		return -EIO;
	}

	err = disable_firewall_filters(KER_SMN_HSIO2HSIO, KER_LINK_HSIO_TILE);
	if (err != GRENDEL_ERR_OK) {
		return -EIO;
	}

	err = disable_firewall_filters(KER_SMN_HSIO2SMN, KER_LINK_HSIO_TILE);
	if (err != GRENDEL_ERR_OK) {
		return -EIO;
	}

	hsio_tile_release_cold_reset(KER_LINK_HSIO_TILE, HSIO_RST_CCE0 | HSIO_RST_CCE1 |
								 HSIO_RST_TL1 | HSIO_RST_FABRIC);

	/* PLL before strap release: straps are inside the tile and do not
	 * answer until it is clocked. See tt_d2d_test_chip_clock_up().
	 */
	config_cce_clock(1);
	enable_hsio_clk(KER_LINK_HSIO_TILE, 1);

	err = disable_ker_hsio_filters(KER_LINK_HSIO_TILE);
	if (err != GRENDEL_ERR_OK) {
		return -EIO;
	}

	return 0;
}

int tt_d2d_test_chip_clock_up(void)
{
	return 0;
}

#else /* Mimir */

int tt_d2d_test_chip_init(void)
{
	grendel_err_t err;

	smc_release_reset(MIMIR_RST_SMN | MIMIR_RST_ITN | MIMIR_RST_D2D0 | MIMIR_RST_D2D1);

	err = disable_all_mimir_firewall_filters();
	if (err != GRENDEL_ERR_OK) {
		return -EIO;
	}

	return 0;
}

int tt_d2d_test_chip_clock_up(void)
{
	config_cce_clock(1);

	return 0;
}

#endif
