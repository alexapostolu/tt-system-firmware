/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/drivers/misc/tt_grendel_hsio.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <basic_init.h>
#include <firewall.h>
#include <platform.h>

#include "regs.h"

DEFINE_FFF_GLOBALS;

DEFINE_FAKE_VOID_FUNC(write16_reg, uint64_t, uint16_t);
DEFINE_FAKE_VALUE_FUNC(uint16_t, read16_reg, uint64_t);
DEFINE_FAKE_VOID_FUNC(write32_reg, uint64_t, uint32_t);
DEFINE_FAKE_VALUE_FUNC(uint32_t, read32_reg, uint64_t);
DEFINE_FAKE_VOID_FUNC(write64_reg, uint64_t, uint64_t);
DEFINE_FAKE_VALUE_FUNC(uint64_t, read64_reg, uint64_t);

#define FILTER_CONFIG_READ_EN     BIT64(0)
#define FILTER_CONFIG_WRITE_EN    BIT64(1)
#define FILTER_CONFIG_ADDR_MODE   BIT64(4)
#define FILTER_CONFIG_ALLOW_NS    BIT64(8)
#define FILTER_CONFIG_ALLOW_BURST BIT64(24)

#define ALLOW_ALL_NS                                                                               \
	(FILTER_CONFIG_READ_EN | FILTER_CONFIG_WRITE_EN | FILTER_CONFIG_ADDR_MODE |                \
	 FILTER_CONFIG_ALLOW_NS | FILTER_CONFIG_ALLOW_BURST)
#define ALLOW_ALL_S                                                                                \
	(FILTER_CONFIG_READ_EN | FILTER_CONFIG_WRITE_EN | FILTER_CONFIG_ADDR_MODE |                \
	 FILTER_CONFIG_ALLOW_BURST)

static void before(void *f)
{
	ARG_UNUSED(f);
	RESET_FAKE(write16_reg);
	RESET_FAKE(read16_reg);
	RESET_FAKE(write32_reg);
	RESET_FAKE(read32_reg);
	RESET_FAKE(write64_reg);
	RESET_FAKE(read64_reg);
}

static void assert_write32(uint64_t addr, uint32_t mask, uint32_t value)
{
	for (unsigned int i = write32_reg_fake.call_count; i > 0; i--) {
		unsigned int index = i - 1;

		if (write32_reg_fake.arg0_history[index] == addr) {
			zassert_equal(write32_reg_fake.arg1_history[index] & mask, value);
			return;
		}
	}

	zassert_true(false, "Expected write to 0x%llx", addr);
}

static void assert_write64(uint64_t addr, uint64_t value)
{
	for (unsigned int i = write64_reg_fake.call_count; i > 0; i--) {
		unsigned int index = i - 1;

		if (write64_reg_fake.arg0_history[index] == addr) {
			zassert_equal(write64_reg_fake.arg1_history[index], value);
			return;
		}
	}

	zassert_true(false, "Expected write to 0x%llx", addr);
}

static void assert_allow_all_bank(uint64_t base)
{
	uint32_t start_off = FIREWALL_START_ADDR_OFFSET;
	uint32_t end_off = FIREWALL_END_ADDR_OFFSET;
	uint32_t stride = FIREWALL_FILTER_SIZE;
	uint64_t mask = FIREWALL_ADDR_MASK;

	assert_write64(base + start_off, 0);
	assert_write64(base + end_off, mask);
	assert_write64(base, ALLOW_ALL_NS);
	assert_write64(base + stride + start_off, 0);
	assert_write64(base + stride + end_off, mask);
	assert_write64(base + stride, ALLOW_ALL_S);
}

ZTEST(tt_grendel_hsio, test_init_programs_reset_clock_and_filters)
{
	int ret = tt_grendel_hsio_init(0);

	zassert_equal(ret, 0);
	assert_write32(SMC_CPU_RESET_UNIT_SS_COLD_RESET_N_REG_ADDR, KER_RST_HSIO0, KER_RST_HSIO0);
	assert_write32(HSIO_TILE_0_HSIO_PLL_RESET_CTRL_SS_COLD_RESET_N_REG_ADDR, UINT32_MAX,
		       0x1FFFF);
	assert_write32(HSIO_TILE_0_HSIO_PLL_RESET_CTRL_AG_MUX_SELECT_REG_ADDR, GENMASK(11, 0),
		       0xFFF);
	assert_allow_all_bank(SMN0_BASE_ADDR + SMN_HSIO2HSIO_FIREWALL_OFFSET +
			      FIREWALL_FILTER_CONFIG_OFFSET);
	assert_allow_all_bank(SMN0_BASE_ADDR + SMN_HSIO2SMN_FIREWALL_OFFSET +
			      FIREWALL_FILTER_CONFIG_OFFSET);
	assert_allow_all_bank(HSIO0_BASE_ADDR + HSIO_NOC2AXI_FIREWALL_OFFSET +
			      FIREWALL_FILTER_CONFIG_OFFSET);
	assert_allow_all_bank(HSIO0_BASE_ADDR + HSIO_PCIE_FIREWALL_OFFSET +
			      FIREWALL_FILTER_CONFIG_OFFSET);
}

ZTEST(tt_grendel_hsio, test_init_rejects_bad_tile)
{
	zassert_equal(tt_grendel_hsio_init(5), -EINVAL);
	zassert_equal(write32_reg_fake.call_count, 0);
}

ZTEST_SUITE(tt_grendel_hsio, NULL, NULL, before, NULL, NULL);
