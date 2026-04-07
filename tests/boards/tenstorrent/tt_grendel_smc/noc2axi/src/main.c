/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/logging/log.h>

#include "tenstorrent/grendel_noc2axi.h"
#include "smc_cpu_reg.h"

LOG_MODULE_REGISTER(noc2axi_test);

#define CCE_0_MIMIR_CCE_CTRL_LOCAL_TL1_BASE_REG_ADDR (0x02300088)
#define CCE_0_PF_CTRL_RESET_REG_ADDR                 (0x02200000)

#define MIMIR_SRAM_SPA_BASE      0x001280000000ULL
#define MIMIR_CCE0_SRAM_SPA_BASE (MIMIR_SRAM_SPA_BASE + 0x00000000ULL)
#define MIMIR_CCE1_SRAM_SPA_BASE (MIMIR_SRAM_SPA_BASE + 0x00400000ULL)
#define MIMIR_SRAM_TLB_IDX       4
#define MIMIR_NOC_IF_ID          0

#define SMC_CPU_RESET_UNIT_SS_COLD_RESET_N_REG_ADDR (0x00002040)
#define SMC_CPU_RESET_UNIT_SS_WARM_RESET_N_REG_ADDR (0x00002044)

#define SRAM_CCE_0_MEM_BASE_ADDR 0x40000000ULL
#define SRAM_CCE_1_MEM_BASE_ADDR 0x40400000ULL

#define CCE_CTRL_REG_MAP_BASE_ADDR (0x02200000)
#define CCE_CTRL_REG_MAP_SIZE      (0x00000300) // up to 0x022002F8 + 8

#define CCE_CTRL_RESET_REG_OFFSET (0x00000000)
#define CCE_CTRL_RESET_REG_ADDR   (0x02200000)

#define CCE_CTRL_TILE_ID_REG_OFFSET (0x00000008)
#define CCE_CTRL_TILE_ID_REG_ADDR   (0x02200008)

#define CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_0__REG_ADDR 0x2000000
#define CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_1__REG_ADDR 0x2000008
#define CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_2__REG_ADDR 0x2000010
#define CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_3__REG_ADDR 0x2000018
#define CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_4__REG_ADDR 0x2000020
#define CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_5__REG_ADDR 0x2000028
#define CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_6__REG_ADDR 0x2000030
#define CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_7__REG_ADDR 0x2000038

#define CCE_0_TT_CLUSTER_CTRL_SCRATCH_3__REG_ADDR 0x200004C

#define FILTER_CTRL_CONFIG_OFFSET 0x00
#define FILTER_CTRL_START_OFFSET  0x08
#define FILTER_CTRL_END_OFFSET    0x10
#define FILTER_CTRL_STRIDE        0x20

#define SMC_ALIAS_REMAP_BASE 0xC0012000UL
#define REMAP_STRIDE         0x20UL

#define REMAP_START_LO(idx) (SMC_ALIAS_REMAP_BASE + (idx) * REMAP_STRIDE + 0x00)
#define REMAP_START_HI(idx) (SMC_ALIAS_REMAP_BASE + (idx) * REMAP_STRIDE + 0x04)
#define REMAP_END_LO(idx)   (SMC_ALIAS_REMAP_BASE + (idx) * REMAP_STRIDE + 0x08)
#define REMAP_END_HI(idx)   (SMC_ALIAS_REMAP_BASE + (idx) * REMAP_STRIDE + 0x0C)
#define REMAP_ATTRS_LO(idx) (SMC_ALIAS_REMAP_BASE + (idx) * REMAP_STRIDE + 0x10)
#define REMAP_ATTRS_HI(idx) (SMC_ALIAS_REMAP_BASE + (idx) * REMAP_STRIDE + 0x14)

#define GR_SYS_ADDR_DECODE_BASE GR_NOC2AXI_D2D0_ADDR_TBL_BASE
#define GR_SYS_MASK_TABLE_BASE  (GR_SYS_ADDR_DECODE_BASE + 0x0030UL)
#define GR_SYS_MASK_ENTRY_SIZE  0x18UL

#define MASK_REG_ENTRY(idx) (GR_SYS_MASK_TABLE_BASE + (idx) * GR_SYS_MASK_ENTRY_SIZE + 0x00)

enum filter_type {
	SMC_INBOUND,
	SMC_OUTBOUND,
	MIMIR_ITN_MEM_TILE0,
	MIMIR_ITN_MEM_TILE1,
	MIMIR_ITN_CCE0,
	MIMIR_ITN_CCE1,
	MIMIR_ITN_CCE0_CFG,
	MIMIR_ITN_CCE1_CFG,
	FILTER_TYPE_COUNT,
};

static const uintptr_t filter_type_base[FILTER_TYPE_COUNT] = {
	[SMC_INBOUND] = 0xC0015000UL,  /* smc_inbound_filter_ctrl[0] */
	[SMC_OUTBOUND] = 0xC0016000UL, /* smc_outbound_filter_ctrl[0] */
	[MIMIR_ITN_MEM_TILE0] = 0x0,   /* TODO: fill from Mimir register map */
	[MIMIR_ITN_MEM_TILE1] = 0x0,   /* TODO */
	[MIMIR_ITN_CCE0] = 0x0,        /* TODO */
	[MIMIR_ITN_CCE1] = 0x0,        /* TODO */
	[MIMIR_ITN_CCE0_CFG] = 0x0,    /* TODO */
	[MIMIR_ITN_CCE1_CFG] = 0x0,    /* TODO */
};

static void filter_config(enum filter_type type, unsigned int idx, unsigned int lock,
			  uint64_t start_addr, uint64_t end_addr,
			  FILTER_CTRL_FILTER_CONFIG_reg_t cfg)
{
	uintptr_t base = filter_type_base[type] + idx * FILTER_CTRL_STRIDE;

	FILTER_CTRL_FILTER_CONFIG_reg_u cu = {.f = cfg};

	sys_write32((uint32_t)(cu.val), base + FILTER_CTRL_CONFIG_OFFSET);
	sys_write32((uint32_t)(cu.val >> 32), base + FILTER_CTRL_CONFIG_OFFSET + 4);
	sys_write32((uint32_t)(start_addr), base + FILTER_CTRL_START_OFFSET);
	sys_write32((uint32_t)(start_addr >> 32), base + FILTER_CTRL_START_OFFSET + 4);
	sys_write32((uint32_t)(end_addr), base + FILTER_CTRL_END_OFFSET);
	sys_write32((uint32_t)(end_addr >> 32), base + FILTER_CTRL_END_OFFSET + 4);

	if (lock) {
		cu.f.locked = 1;
		sys_write32((uint32_t)(cu.val), base + FILTER_CTRL_CONFIG_OFFSET);
		sys_write32((uint32_t)(cu.val >> 32), base + FILTER_CTRL_CONFIG_OFFSET + 4);
	}
}

ZTEST(noc2axi, test_smc_alias_remap_is_live)
{
	uint32_t pattern = 0xABCDE000;

	sys_write32(pattern, SMC_ALIAS_REMAP_BASE);

	uint32_t rb = sys_read32(SMC_ALIAS_REMAP_BASE);

	LOG_INF("SMC_ALIAS_REMAP @ 0x%08lx: wrote 0x%08x read 0x%08x",
		(unsigned long)SMC_ALIAS_REMAP_BASE, pattern, rb);

	/* Bits [11:0] are reserved; only compare [31:12] */
	zassert_equal(rb & 0xFFFFF000, pattern & 0xFFFFF000,
		      "SMC_ALIAS_REMAP not live: wrote 0x%08x got 0x%08x", pattern, rb);
}

ZTEST(noc2axi, test_remap_region0_start_write_read)
{
	/* Region 0 START: Keraunos Config, start = 0xC0000000 */
	uint32_t val_lo = 0xC0000000;

	sys_write32(val_lo, REMAP_START_LO(0));

	uint32_t rb = sys_read32(REMAP_START_LO(0));

	zassert_equal(rb & 0xFFFFF000, val_lo & 0xFFFFF000,
		      "Region 0 START_LO: expected 0x%08x got 0x%08x", val_lo & 0xFFFFF000,
		      rb & 0xFFFFF000);
}

ZTEST(noc2axi, test_remap_region1_attrs_valid_bit)
{
	/* Write ATTRS with valid=1 (bit 31 of hi word, which is bit 63) */
	sys_write32(0x00000000, REMAP_ATTRS_LO(1));
	sys_write32(0x80000000, REMAP_ATTRS_HI(1)); /* valid bit = bit 63 */

	uint32_t rb_hi = sys_read32(REMAP_ATTRS_HI(1));

	LOG_INF("Region 1 ATTRS_HI: wrote 0x80000000 read 0x%08x", rb_hi);

	zassert_true(rb_hi & 0x80000000, "Region 1 ATTRS valid bit not set: got 0x%08x", rb_hi);
}

static uint8_t cce_fw_bin[] = {
#include "cce_fw_bin.inc"
};

static void setup_filters(void)
{
	FILTER_CTRL_FILTER_CONFIG_reg_u config;

	config.val = 0;
	config.f.read_en = 1;
	config.f.write_en = 1;
	config.f.addr_mode = 1;
	config.f.data_bus_width = 7; /* 128 bits */
	config.f.allow_burst = 1;

	/* Configure filter 0 for all endpoints to allow all secure traffic */
	config.f.allow_ns = 0;
	filter_config(SMC_OUTBOUND, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, config.f);
	filter_config(SMC_INBOUND, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, config.f);
	filter_config(MIMIR_ITN_MEM_TILE0, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
		      config.f);
	filter_config(MIMIR_ITN_MEM_TILE1, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
		      config.f);
	filter_config(MIMIR_ITN_CCE0, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, config.f);
	filter_config(MIMIR_ITN_CCE1, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, config.f);
	filter_config(MIMIR_ITN_CCE0_CFG, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
		      config.f);
	filter_config(MIMIR_ITN_CCE1_CFG, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
		      config.f);

	/* Configure filter 1 for all endpoints to allow all traffic */
	config.f.allow_ns = 1;
	filter_config(SMC_OUTBOUND, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, config.f);
	filter_config(SMC_INBOUND, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, config.f);
	filter_config(MIMIR_ITN_MEM_TILE0, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
		      config.f);
	filter_config(MIMIR_ITN_MEM_TILE1, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
		      config.f);
	filter_config(MIMIR_ITN_CCE0, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, config.f);
	filter_config(MIMIR_ITN_CCE1, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, config.f);
	filter_config(MIMIR_ITN_CCE0_CFG, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
		      config.f);
	filter_config(MIMIR_ITN_CCE1_CFG, 0, 0, 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
		      config.f);
}

struct cce_instance {
	uint64_t sram_base;
	uint64_t pf_ctrl_reset_reg;
	uint64_t reset_vec_regs[8];
	uint64_t scratch3_reg;
};

static const struct cce_instance cce_instances[] = {
	[0] =
		{
			.sram_base = SRAM_CCE_0_MEM_BASE_ADDR,
			.pf_ctrl_reset_reg = CCE_CTRL_RESET_REG_ADDR,
			.reset_vec_regs =
				{
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_0__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_1__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_2__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_3__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_4__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_5__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_6__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_7__REG_ADDR,
				},
			.scratch3_reg = CCE_0_TT_CLUSTER_CTRL_SCRATCH_3__REG_ADDR,
		},
	[1] =
		{
			.sram_base = SRAM_CCE_1_MEM_BASE_ADDR,
			.pf_ctrl_reset_reg = CCE_CTRL_RESET_REG_ADDR,
			.reset_vec_regs =
				{
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_0__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_1__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_2__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_3__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_4__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_5__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_6__REG_ADDR,
					CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_7__REG_ADDR,
				},
			.scratch3_reg = CCE_0_TT_CLUSTER_CTRL_SCRATCH_3__REG_ADDR,
		},
};

typedef struct {
	uint64_t uncore_reset: 1;
	uint64_t core_reset: 8;
} CCE_CTRL_RESET_reg_t;

typedef union {
	uint64_t val;
	CCE_CTRL_RESET_reg_t f;
} CCE_CTRL_RESET_reg_u;

static void boot_single_cce(int idx)
{
    const struct cce_instance *c = &cce_instances[idx];
    CCE_CTRL_RESET_reg_u pf_ctrl;
    uint64_t load_addr = c->sram_base;

    sys_write64(load_addr, CCE_0_MIMIR_CCE_CTRL_LOCAL_TL1_BASE_REG_ADDR);

    pf_ctrl.val = sys_read64(c->pf_ctrl_reset_reg);
    pf_ctrl.f.uncore_reset = 1;
    pf_ctrl.f.core_reset = 0x00; /* Keep cores in reset while programming SRAM */
    printk("Releasing CCE%d uncore reset to access SRAM...\n", idx);
    sys_write64(pf_ctrl.val, c->pf_ctrl_reset_reg);

    /* Zero CCE memory */
    for (size_t i = 0; i < 0x10000; i += 8) {
        sys_write64(0, load_addr + i);
    }

    printk("CCE%d memory zeroed. Proceeding to load firmware binary...\n", idx);

    /* Load CCE firmware */
    memcpy((void *)load_addr, cce_fw_bin, sizeof(cce_fw_bin));

    printk("CCE%d firmware loaded. Programming reset vectors...\n", idx);

    for (int i = 0; i < 8; ++i) {
        sys_write64(load_addr, c->reset_vec_regs[i]);
    }

    /* Release CCE core reset to start execution */
    pf_ctrl.val = sys_read64(c->pf_ctrl_reset_reg);
    pf_ctrl.f.core_reset = 0x1; /* Release core 0 from reset */
    printk("Releasing CCE%d core reset...\n", idx);
    sys_write64(pf_ctrl.val, c->pf_ctrl_reset_reg);
}

ZTEST(noc2axi_mimir_cce, test_cce0_cce1_noc2axi_rw)
{
	int ret;

	printk("Starting CCE0/CCE1 NOC2AXI RW test (with CCE FW checks)...\n");

	/* Remove all resets */
	sys_write32(0xFFFFFFFF, SMC_CPU_RESET_UNIT_SS_COLD_RESET_N_REG_ADDR);
	sys_write32(0xFFFFFFFF, SMC_CPU_RESET_UNIT_SS_WARM_RESET_N_REG_ADDR);

	setup_filters();

	ret = gr_noc2axi_init();
	zassert_equal(ret, 0);

	boot_single_cce(0);

	k_msleep(100);

	const uint64_t cce0_addr = MIMIR_CCE0_SRAM_SPA_BASE + 0x100;
	const uint32_t pattern0 = 0x11223344;

	// gr_noc2axi_write32(MIMIR_NOC_IF_ID, MIMIR_SRAM_TLB_IDX, cce0_addr, pattern0);

	uint32_t rd0 = gr_noc2axi_read32(MIMIR_NOC_IF_ID, MIMIR_SRAM_TLB_IDX, cce0_addr);

	zassert_equal(rd0, pattern0, "CCE0 SRAM: expected 0x%08X, got 0x%08X", pattern0, rd0);
}

ZTEST(remoteproc_cce, test_load_fw)
{
	printk("Starting CCE firmware load test!\n");

	/* Remove all resets */
	sys_write32(0xFFFFFFFF, SMC_CPU_RESET_UNIT_SS_COLD_RESET_N_REG_ADDR);
	sys_write32(0xFFFFFFFF, SMC_CPU_RESET_UNIT_SS_WARM_RESET_N_REG_ADDR);

	setup_filters();

	printk("All firewalls disabled. Proceeding to load CCE firmware...\n");

	printk("Attempting to access CCE registers...\n");
	sys_write64(SRAM_CCE_0_MEM_BASE_ADDR, CCE_0_MIMIR_CCE_CTRL_LOCAL_TL1_BASE_REG_ADDR);
	CCE_CTRL_RESET_reg_u pf_ctrl;

	pf_ctrl.val = sys_read64(CCE_0_PF_CTRL_RESET_REG_ADDR);
	pf_ctrl.f.uncore_reset = 1;
	pf_ctrl.f.core_reset = 0x00; /* Keep all cores in reset */
	printk("Releasing CCE uncore reset to access SRAM...\n");
	sys_write64(pf_ctrl.val, CCE_0_PF_CTRL_RESET_REG_ADDR);

	/* Program CCE firmware to remote processor */
	uint64_t load_addr = SRAM_CCE_0_MEM_BASE_ADDR;

	printk("Loading CCE firmware to address 0x%08llX...\n", load_addr);

	/* Zero CCE memory */
	for (size_t i = 0; i < 0x10000; i += 8) {
		sys_write64(0, load_addr + i);
	}

	printk("CCE memory zeroed. Proceeding to load firmware binary...\n");

	/* Load CCE firmware */
	memcpy((void *)load_addr, cce_fw_bin, sizeof(cce_fw_bin));

	printk("CCE firmware loaded. Releasing CCE core reset to start execution...\n");

	sys_write64(SRAM_CCE_0_MEM_BASE_ADDR, CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_0__REG_ADDR);
	sys_write64(SRAM_CCE_0_MEM_BASE_ADDR, CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_1__REG_ADDR);
	sys_write64(SRAM_CCE_0_MEM_BASE_ADDR, CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_2__REG_ADDR);
	sys_write64(SRAM_CCE_0_MEM_BASE_ADDR, CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_3__REG_ADDR);
	sys_write64(SRAM_CCE_0_MEM_BASE_ADDR, CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_4__REG_ADDR);
	sys_write64(SRAM_CCE_0_MEM_BASE_ADDR, CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_5__REG_ADDR);
	sys_write64(SRAM_CCE_0_MEM_BASE_ADDR, CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_6__REG_ADDR);
	sys_write64(SRAM_CCE_0_MEM_BASE_ADDR, CCE_0_TT_CLUSTER_CTRL_RESET_VECTOR_7__REG_ADDR);

	/* Release CCE reset */
	pf_ctrl.val = sys_read64(CCE_0_PF_CTRL_RESET_REG_ADDR);
	pf_ctrl.f.core_reset = 0x1; /* Release core 0 from reset */
	sys_write64(pf_ctrl.val, CCE_0_PF_CTRL_RESET_REG_ADDR);

	/* Wait for firmware execution */
	k_msleep(100);

	/* Read from cce scratch to verify firmware execution */
	uint32_t scratch_val = sys_read32(CCE_0_TT_CLUSTER_CTRL_SCRATCH_3__REG_ADDR);

	zassert_equal(scratch_val, 0xdeadbeef, "Unexpected value from CCE firmware: 0x%08X",
		      scratch_val);

	scratch_val = sys_read32(SRAM_CCE_0_MEM_BASE_ADDR + 0x1000);
	zassert_equal(scratch_val, 0xcafebabe, "Unexpected value from CCE firmware: 0x%08X",
		      scratch_val);

	printk("CCE firmware wrote 0x%08X to SRAM\n", scratch_val);
}

ZTEST_SUITE(remoteproc_cce, NULL, NULL, NULL, NULL, NULL);

ZTEST_SUITE(noc2axi_mimir_cce, NULL, NULL, NULL, NULL, NULL);

ZTEST_SUITE(noc2axi, NULL, NULL, NULL, NULL, NULL);
