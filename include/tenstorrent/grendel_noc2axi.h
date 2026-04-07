/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GRENDEL_NOC2AXI_H
#define GRENDEL_NOC2AXI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define GR_NOC2AXI_D2D0_NIU_BASE     0x04280000UL
#define GR_NOC2AXI_D2D0_LOCAL_A_BASE 0x04281000UL
#define GR_NOC2AXI_D2D0_TLBS_A_BASE  0x04281200UL
#define GR_NOC2AXI_D2D0_ADDR_TBL_BASE 0x04290000UL

#define GR_NOC2AXI_D2D1_NIU_BASE     0x04480000UL
#define GR_NOC2AXI_D2D1_LOCAL_A_BASE 0x04481000UL
#define GR_NOC2AXI_D2D1_TLBS_A_BASE  0x04481200UL
#define GR_NOC2AXI_D2D1_ADDR_TBL_BASE 0x04490000UL

#define GR_NOC2AXI_REG_BASE       GR_NOC2AXI_D2D0_NIU_BASE
#define GR_NOC2AXI_NIU_REG_BASE   GR_NOC2AXI_D2D0_NIU_BASE
#define GR_NOC2AXI_NIU_REG_SIZE   0x500UL
#define GR_NOC2AXI_LOCAL_REG_BASE GR_NOC2AXI_D2D0_LOCAL_A_BASE
#define GR_NOC2AXI_LOCAL_REG_SIZE 0x144UL

#define GR_NOC2AXI_NUM_TLBS       16
#define GR_NOC2AXI_TLB_LOG_SIZE   24
#define GR_NOC2AXI_TLB_WINDOW_SIZE (1UL << GR_NOC2AXI_TLB_LOG_SIZE)
#define GR_NOC2AXI_TLB_ADDR_MASK  (GR_NOC2AXI_TLB_WINDOW_SIZE - 1)
#define GR_NOC2AXI_NUM_NOC_IF     4

#define GR_NOC2AXI_TLB_REG_OFFSET  0x1000UL
#define GR_NOC2AXI_RING_SEL_BIT    15

struct gr_noc2axi_tlb_addr_lo {
	uint32_t passthrough_bits: 24;
	uint32_t lower_addr_bits: 8;
};

struct gr_noc2axi_tlb_addr_hi {
	uint32_t middle_addr_bits: 32;
};

struct gr_noc2axi_tlb_cfg {
	uint32_t x_end: 6;
	uint32_t y_end: 6;
	uint32_t x_start: 6;
	uint32_t y_start: 6;
	uint32_t multicast_en: 1;
	uint32_t ordering_mode: 2;
	uint32_t linked: 1;
	uint32_t reserved: 4;
};

struct gr_noc2axi_tlb_mcast {
	uint32_t stride_x: 4;
	uint32_t stride_y: 4;
	uint32_t quad_exclude_x: 6;
	uint32_t quad_exclude_y: 6;
	uint32_t quad_exclude_ctrl: 4;
	uint32_t num_destinations: 8;
};

union gr_noc2axi_tlb_addr_lo_u {
	uint32_t val;
	struct gr_noc2axi_tlb_addr_lo f;
};

union gr_noc2axi_tlb_addr_hi_u {
	uint32_t val;
	struct gr_noc2axi_tlb_addr_hi f;
};

union gr_noc2axi_tlb_cfg_u {
	uint32_t val;
	struct gr_noc2axi_tlb_cfg f;
};

union gr_noc2axi_tlb_mcast_u {
	uint32_t val;
	struct gr_noc2axi_tlb_mcast f;
};

enum gr_noc2axi_ordering {
	GR_NOC2AXI_ORDERING_RELAXED = 0,
	GR_NOC2AXI_ORDERING_STRICT = 1,
	GR_NOC2AXI_ORDERING_POSTED = 2,
	GR_NOC2AXI_ORDERING_POSTED_STRICT = 3,
};

struct gr_noc2axi_mask_entry {
	uint64_t bar;
	uint64_t compare;
	uint8_t mask;
	uint8_t ep_id_size;
	uint16_t ep_idx;
	uint16_t table_offset;
	bool translate_addr;
	bool initialized;
	uint8_t index;
};

struct gr_noc2axi_ep_entry {
	uint16_t index;
	uint8_t x_coor;
	uint8_t y_coor;
	bool is_self;
	bool initialized;
};

void gr_noc2axi_tlb_setup(uint8_t ring, uint8_t tlb_idx, uint8_t x, uint8_t y, uint64_t addr);

volatile void *gr_noc2axi_get_tlb_window_addr(uint8_t noc_id, uint8_t tlb_idx, uint64_t addr);

void gr_noc2axi_write32(uint8_t noc_id, uint8_t tlb_idx, uint64_t addr, uint32_t data);

uint32_t gr_noc2axi_read32(uint8_t noc_id, uint8_t tlb_idx, uint64_t addr);

int gr_noc2axi_configure_remap(void);

int gr_noc2axi_init(void);

int gr_noc2axi_program_mask_table(const struct gr_noc2axi_mask_entry *entries, size_t count);

int gr_noc2axi_configure_mk(void);

int gr_noc2axi_configure_qsr0(void);

int gr_noc2axi_configure_qsr1(void);

int gr_noc2axi_configure_for_mimir(void);

struct remap_region {
	uint64_t local_start;
	uint64_t local_end;
	uint64_t spa_start;
	bool cacheable;
};

void program_remap_region(unsigned int idx, const struct remap_region *r);

#endif /* GRENDEL_NOC2AXI_H */
