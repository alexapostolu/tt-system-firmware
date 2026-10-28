/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "telemetry.h"

ZTEST(telemetry, test_tag_name_matches_macro)
{
	zassert_str_equal(GetTelemetryTagName(TAG_BOARD_ID_HIGH), "TAG_BOARD_ID_HIGH");
	zassert_str_equal(GetTelemetryTagName(TAG_AICLK), "TAG_AICLK");
	zassert_str_equal(GetTelemetryTagName(TAG_FLASH_JEDEC_ID), "TAG_FLASH_JEDEC_ID");
	zassert_str_equal(GetTelemetryTagName(TAG_NAME_TABLE_ADDR), "TAG_NAME_TABLE_ADDR");
}

ZTEST(telemetry, test_tag_name_unpublished)
{
	zassert_is_null(GetTelemetryTagName(0));
	zassert_is_null(GetTelemetryTagName(TAG_ASIC_ID));
	zassert_is_null(GetTelemetryTagName(TAG_COUNT));
	zassert_is_null(GetTelemetryTagName(UINT16_MAX));
}

ZTEST(telemetry, test_tag_names_unique)
{
	for (uint16_t a = 0; a < TAG_COUNT; a++) {
		const char *name_a = GetTelemetryTagName(a);

		if (name_a == NULL) {
			continue;
		}
		zassert_true(strncmp(name_a, "TAG_", 4) == 0, "tag %u name %s", a, name_a);
		for (uint16_t b = a + 1; b < TAG_COUNT; b++) {
			const char *name_b = GetTelemetryTagName(b);

			zassert_true(name_b == NULL || strcmp(name_a, name_b) != 0,
				     "tags %u and %u are both named %s", a, b, name_a);
		}
	}
}

ZTEST_SUITE(telemetry, NULL, NULL, NULL, NULL, NULL);
