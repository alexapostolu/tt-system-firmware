/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Mock power and thermal telemetry until the real sensors are wired up. */

#include <stdint.h>

#include <zephyr/kernel.h>

#include <tenstorrent/gr_telemetry.h>

/* Values are in each source's native units; the descriptor scale converts them. */
GR_TELEM_ITEM_DEFINE(vcore, GR_TELEM_GROUP_POWER, 0, GR_TELEM_DEC_U32, GR_TELEM_UNIT_VOLT, -3, 0);
GR_TELEM_ITEM_DEFINE(vcore_current, GR_TELEM_GROUP_POWER, 1, GR_TELEM_DEC_U32, GR_TELEM_UNIT_AMPERE,
		     -2, 0);
GR_TELEM_ITEM_DEFINE(power, GR_TELEM_GROUP_POWER, 2, GR_TELEM_DEC_FIXED_S16_16, GR_TELEM_UNIT_WATT,
		     0, 0);
GR_TELEM_ITEM_DEFINE(asic_temp, GR_TELEM_GROUP_THERMAL, 0, GR_TELEM_DEC_IEEE754_F32,
		     GR_TELEM_UNIT_CELSIUS, 0, 0);
GR_TELEM_ITEM_DEFINE(therm_trip, GR_TELEM_GROUP_THERMAL, 1, GR_TELEM_DEC_BOOL, GR_TELEM_UNIT_NONE,
		     0, 0);

static void mock_sample(void)
{
	uint32_t uptime_s = k_uptime_get_32() / 1000U;

	/* 750 mV, 16.67 A (10 mA steps), 12.5 W */
	gr_telem_set(GR_TELEM_ITEM(vcore), 750);
	gr_telem_set(GR_TELEM_ITEM(vcore_current), 1667);
	gr_telem_set(GR_TELEM_ITEM(power), (12U << 16) | 0x8000U);
	gr_telem_set_f32(GR_TELEM_ITEM(asic_temp), 45.0f + (float)(uptime_s % 20U) * 0.25f);
	gr_telem_set(GR_TELEM_ITEM(therm_trip), 0);
}
GR_TELEM_SAMPLER_DEFINE(mock, mock_sample);
