/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include "tenstorrent/grendel_noc2axi.h"
#include "smc_cpu_reg.h"

LOG_MODULE_REGISTER(grendel_noc2axi);

#define GR_NOC0_AXI_BASE 0xC0000000UL
#define GR_NOC1_AXI_BASE 0xD0000000UL
#define GR_NOC2_AXI_BASE 0xE0000000UL
#define GR_NOC3_AXI_BASE 0xF0000000UL

static const uint32_t noc_axi_base[GR_NOC2AXI_NUM_NOC_IF] = {
	GR_NOC0_AXI_BASE,
	GR_NOC1_AXI_BASE,
	GR_NOC2_AXI_BASE,
	GR_NOC3_AXI_BASE,
};

#define GR_SYS_ADDR_DECODE_BASE    GR_NOC2AXI_D2D0_ADDR_TBL_BASE

#define GR_SYS_MASK_TABLE_BASE    (GR_SYS_ADDR_DECODE_BASE + 0x0030UL)
#define GR_SYS_MASK_ENTRY_SIZE    0x18UL

#define MASK_REG_ENTRY(idx) (GR_SYS_MASK_TABLE_BASE + (idx) * GR_SYS_MASK_ENTRY_SIZE + 0x00)
#define MASK_REG_EP_LO(idx) (GR_SYS_MASK_TABLE_BASE + (idx) * GR_SYS_MASK_ENTRY_SIZE + 0x08)
#define MASK_REG_EP_HI(idx) (GR_SYS_MASK_TABLE_BASE + (idx) * GR_SYS_MASK_ENTRY_SIZE + 0x0C)
#define MASK_REG_BAR_LO(idx) (GR_SYS_MASK_TABLE_BASE + (idx) * GR_SYS_MASK_ENTRY_SIZE + 0x10)
#define MASK_REG_BAR_HI(idx) (GR_SYS_MASK_TABLE_BASE + (idx) * GR_SYS_MASK_ENTRY_SIZE + 0x14)

#define SMC_ALIAS_REMAP_BASE       0xC0012000UL
#define SMC_ALIAS_REMAP_REGION_STRIDE 0x20UL  /* 3x64b regs padded to 0x20 */

#define REMAP_START_LO(idx)  (SMC_ALIAS_REMAP_BASE + (uint32_t)(idx) * SMC_ALIAS_REMAP_REGION_STRIDE + 0x00U)
#define REMAP_START_HI(idx)  (SMC_ALIAS_REMAP_BASE + (uint32_t)(idx) * SMC_ALIAS_REMAP_REGION_STRIDE + 0x04U)

#define REMAP_END_LO(idx)    (SMC_ALIAS_REMAP_BASE + (uint32_t)(idx) * SMC_ALIAS_REMAP_REGION_STRIDE + 0x08U)
#define REMAP_END_HI(idx)    (SMC_ALIAS_REMAP_BASE + (uint32_t)(idx) * SMC_ALIAS_REMAP_REGION_STRIDE + 0x0CU)

#define REMAP_ATTRS_LO(idx)  (SMC_ALIAS_REMAP_BASE + (uint32_t)(idx) * SMC_ALIAS_REMAP_REGION_STRIDE + 0x10U)
#define REMAP_ATTRS_HI(idx)  (SMC_ALIAS_REMAP_BASE + (uint32_t)(idx) * SMC_ALIAS_REMAP_REGION_STRIDE + 0x14U)

void program_remap_region(unsigned int idx, const struct remap_region *r)
{
	uint64_t start_pg  = r->local_start >> 12;           // bits [55:12]
	uint64_t end_pg    = r->local_end   >> 12;           // bits [55:12]
	uint64_t offset_pg = (r->spa_start - r->local_start) >> 12;

	uint32_t start_lo  = (uint32_t)(start_pg & 0xFFFFFFFFULL);
	uint32_t start_hi  = (uint32_t)(start_pg >> 32);
	uint32_t end_lo    = (uint32_t)(end_pg & 0xFFFFFFFFULL);
	uint32_t end_hi    = (uint32_t)(end_pg >> 32);

	uint64_t attrs     = 0;
	attrs |= (offset_pg & ((1ULL << 44) - 1)) << 12;  // offset into [55:12]
	if (r->cacheable)
		attrs |= (1ULL << 62);
	attrs |= (1ULL << 63);                            // valid

	uint32_t attrs_lo  = (uint32_t)(attrs & 0xFFFFFFFFULL);
	uint32_t attrs_hi  = (uint32_t)(attrs >> 32);

	sys_write32(start_lo,  REMAP_START_LO(idx));
	sys_write32(start_hi,  REMAP_START_HI(idx));
	sys_write32(end_lo,    REMAP_END_LO(idx));
	sys_write32(end_hi,    REMAP_END_HI(idx));
	sys_write32(attrs_lo,  REMAP_ATTRS_LO(idx));
	sys_write32(attrs_hi,  REMAP_ATTRS_HI(idx));
}

int gr_noc2axi_configure_remap(void)
{
	static const struct remap_region regions[] = {
		/* [0] Keraunos Config -> 0x0012_0000_0000 */
		{
			.local_start = 0xC0000000ULL,
			.local_end   = 0xC3FFFFFFULL,
			.spa_start   = 0x001200000000ULL,
			.cacheable   = false,
		},
		/* [1] Mimir SRAM / CCE -> 0x0012_8000_0000 */
		{
			.local_start = 0xC4000000ULL,
			.local_end   = 0xC7FFFFFFULL,
			.spa_start   = 0x001280000000ULL,
			.cacheable   = true,
		},
		/* [2] Mimir Config -> 0x0013_0000_0000 */
		{
			.local_start = 0xC8000000ULL,
			.local_end   = 0xCBFFFFFFULL,
			.spa_start   = 0x001300000000ULL,
			.cacheable   = false,
		},
		/* [3] Quasar[0] Config -> 0x0018_0000_0000 */
		{
			.local_start = 0xCC000000ULL,
			.local_end   = 0xCFFFFFFFULL,
			.spa_start   = 0x001800000000ULL,
			.cacheable   = false,
		},
		/* [4] Quasar[1] Config -> 0x001C_0000_0000 */
		{
			.local_start = 0xD0000000ULL,
			.local_end   = 0xD3FFFFFFULL,
			.spa_start   = 0x001C00000000ULL,
			.cacheable   = false,
		},
	};

	LOG_INF("Configuring alias remap table for Q1.A1");

	for (unsigned int i = 0; i < ARRAY_SIZE(regions); i++) {
		program_remap_region(i, &regions[i]);
	}

	return 0;
}

static inline volatile uint32_t *get_tlb_reg_start(uint8_t ring)
{
	return (volatile uint32_t *)((GR_NOC2AXI_NIU_REG_BASE + GR_NOC2AXI_TLB_REG_OFFSET) |
				     ((uint32_t)ring << GR_NOC2AXI_RING_SEL_BIT));
}

static void write_tlb(uint8_t ring, uint8_t tlb_idx, union gr_noc2axi_tlb_addr_lo_u addr_lo,
		       union gr_noc2axi_tlb_addr_hi_u addr_hi, union gr_noc2axi_tlb_cfg_u cfg,
		       union gr_noc2axi_tlb_mcast_u mcast)
{
	volatile uint32_t *base = get_tlb_reg_start(ring);

	base[tlb_idx * 2] = addr_lo.val;
	base[tlb_idx * 2 + 1] = addr_hi.val;
	base[GR_NOC2AXI_NUM_TLBS * 2 + tlb_idx] = cfg.val;
	base[GR_NOC2AXI_NUM_TLBS * 3 + tlb_idx] = mcast.val;
}

void gr_noc2axi_tlb_setup(uint8_t ring, uint8_t tlb_idx, uint8_t x, uint8_t y, uint64_t addr)
{
	union gr_noc2axi_tlb_addr_lo_u addr_lo = {.val = 0};
	union gr_noc2axi_tlb_addr_hi_u addr_hi = {.val = 0};
	union gr_noc2axi_tlb_cfg_u cfg = {.val = 0};
	union gr_noc2axi_tlb_mcast_u mcast = {.val = 0};

	addr_lo.f.lower_addr_bits = (uint8_t)(addr >> 24);
	addr_hi.f.middle_addr_bits = (uint32_t)(addr >> 32);

	cfg.f.x_end = x;
	cfg.f.y_end = y;
	cfg.f.ordering_mode = GR_NOC2AXI_ORDERING_STRICT;

	write_tlb(ring, tlb_idx, addr_lo, addr_hi, cfg, mcast);
}

volatile void *gr_noc2axi_get_tlb_window_addr(uint8_t noc_id, uint8_t tlb_idx, uint64_t addr)
{
	uint32_t base = noc_axi_base[noc_id];

	return (volatile void *)(uintptr_t)(base +
					    ((uint32_t)tlb_idx << GR_NOC2AXI_TLB_LOG_SIZE) +
					    ((uint32_t)addr & GR_NOC2AXI_TLB_ADDR_MASK));
}

void gr_noc2axi_write32(uint8_t noc_id, uint8_t tlb_idx, uint64_t addr, uint32_t data)
{
	volatile uint32_t *p = gr_noc2axi_get_tlb_window_addr(noc_id, tlb_idx, addr);
	*p = data;
}

uint32_t gr_noc2axi_read32(uint8_t noc_id, uint8_t tlb_idx, uint64_t addr)
{
	volatile uint32_t *p = gr_noc2axi_get_tlb_window_addr(noc_id, tlb_idx, addr);
	return *p;
}

static void write_mask_entry(const struct gr_noc2axi_mask_entry *entry)
{
	uint32_t ctrl = 0;

	ctrl |= (entry->mask & 0x3F);
	ctrl |= ((entry->ep_idx & 0x3F) << 6);
	ctrl |= ((entry->ep_id_size & 0x3F) << 12);
	ctrl |= ((entry->table_offset & 0x3FF) << 18);
	if (entry->translate_addr) {
		ctrl |= BIT(28);
	}

	sys_write32(ctrl, MASK_REG_ENTRY(entry->index));
	sys_write32((uint32_t)(entry->compare & 0xFFFFFFFFUL), MASK_REG_EP_LO(entry->index));
	sys_write32((uint32_t)(entry->compare >> 32), MASK_REG_EP_HI(entry->index));
	sys_write32((uint32_t)(entry->bar & 0xFFFFFFFFUL), MASK_REG_BAR_LO(entry->index));
	sys_write32((uint32_t)(entry->bar >> 32), MASK_REG_BAR_HI(entry->index));
}

int gr_noc2axi_program_mask_table(const struct gr_noc2axi_mask_entry *entries, size_t count)
{
	if (entries == NULL || count == 0) {
		return -EINVAL;
	}

	for (size_t i = 0; i < count; i++) {
		write_mask_entry(&entries[i]);
	}

	return 0;
}

int gr_noc2axi_configure_for_mimir(void)
{
	static const struct gr_noc2axi_mask_entry mimir_mask_entries[] = {
		/* Mimir SRAM / CCE -- mask table index 2 */
		{
			.compare = 0x1280000000ULL,
			.mask = 27,
			.ep_id_size = 5,
			.ep_idx = 22,
			.table_offset = 64,
			.index = 2,
		},
		/* Mimir Config -- mask table index 3 */
		{
			.compare = 0x1300000000ULL,
			.mask = 31,
			.ep_id_size = 4,
			.ep_idx = 27,
			.table_offset = 96,
			.index = 3,
		},
	};

	int ret;

	LOG_INF("Configuring NOC2AXI for Mimir CCE M2M DMA");

	ret = gr_noc2axi_program_mask_table(mimir_mask_entries, ARRAY_SIZE(mimir_mask_entries));
	if (ret != 0) {
		return ret;
	}

	/* TLB windows for Mimir access */
	gr_noc2axi_tlb_setup(0, 4, 4, 7, 0x1280000000ULL); /* Mimir CCE SRAM */
	gr_noc2axi_tlb_setup(0, 5, 4, 7, 0x1300000000ULL); /* Mimir Config */

	return 0;
}

int gr_noc2axi_init(void)
{
	int ret;

	LOG_INF("Initializing NOC2AXI for Q1.A1");

	/* Alias remap IS modeled in Mimir emulation – required for SPA-based TLBs */
	LOG_INF("Configuring alias remap table (SMC_ALIAS_REMAP)...");
	ret = gr_noc2axi_configure_remap();
	if (ret != 0) {
		LOG_ERR("Failed to configure alias remap: %d", ret);
		return ret;
	}

	LOG_INF("Configuring Mimir...");
	ret = gr_noc2axi_configure_for_mimir();
	if (ret != 0) {
		LOG_ERR("Failed to configure Mimir: %d", ret);
		return ret;
	}

	LOG_INF("NOC2AXI init complete");
	return 0;
}
