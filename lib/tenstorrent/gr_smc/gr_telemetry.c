/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Grendel telemetry core: collects the items components define with GR_TELEM_ITEM_DEFINE and
 * pushes their descriptors, static values and a ring of timestamped frames into a host buffer
 * over PCIe. See gr_telemetry.h for the buffer layout.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <tenstorrent/gr_telemetry.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>

#include <platform.h>

#if defined(HAS_APP_VERSION)
#include <zephyr/app_version.h>
#else
#define APPVERSION 0x00000000
#endif

LOG_MODULE_REGISTER(gr_telemetry, CONFIG_TT_APP_LOG_LEVEL);

#define MAX_ITEMS           CONFIG_TT_GR_TELEMETRY_MAX_ITEMS
#define TIMEBASE_HZ         1000000U
#define SAMPLE_VALID        BIT64(32)
#define VALUE_BLOCK_SIZE(n) (sizeof(uint32_t) * (GR_TELEM_VALID_WORDS(n) + (n)))

BUILD_ASSERT(sizeof(struct gr_telem_desc) == 32);
BUILD_ASSERT(sizeof(struct gr_telem_buffer_header) == 64);
BUILD_ASSERT(sizeof(struct gr_telem_frame_header) == 16);

GR_TELEM_ITEM_DEFINE(fw_version, GR_TELEM_GROUP_SYSTEM, 0, GR_TELEM_DEC_VERSION, GR_TELEM_UNIT_NONE,
		     0, GR_TELEM_FLAG_STATIC);
GR_TELEM_ITEM_DEFINE(chip_id, GR_TELEM_GROUP_SYSTEM, 1, GR_TELEM_DEC_HEX, GR_TELEM_UNIT_NONE, 0,
		     GR_TELEM_FLAG_STATIC);

static const struct device *const dma_dev = DEVICE_DT_GET(DT_NODELABEL(dma0));

/* Item counts, fixed at init. */
static uint32_t item_count;
static uint32_t static_count;
static bool too_many_items;

/*
 * Staging for one value block of up to MAX_ITEMS values, preceded by a frame header. The extra
 * word covers rounding the frame size up to 8 bytes.
 */
static struct {
	struct gr_telem_frame_header hdr;
	uint32_t words[GR_TELEM_VALID_WORDS(MAX_ITEMS) + MAX_ITEMS + 1];
} stage;

/* Set by gr_telem_set() when a static item changes; the push work rewrites the static block. */
static atomic_t static_dirty;

/* Push state. Only touched from the system work queue (message handlers and push_work). */
static struct {
	bool active;
	uint64_t host_addr;
	uint32_t period_ms;
	int64_t next_tick;
	struct gr_telem_buffer_header hdr;
} push;

static void push_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(push_work, push_work_handler);

static inline bool is_static(const struct gr_telem_item *item)
{
	return (item->desc.flags & GR_TELEM_FLAG_STATIC) != 0;
}

void gr_telem_set(struct gr_telem_item *item, uint32_t value)
{
	*(volatile uint64_t *)&item->sample = SAMPLE_VALID | value;
	if (is_static(item)) {
		atomic_set(&static_dirty, 1);
	}
}

void gr_telem_set_f32(struct gr_telem_item *item, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	gr_telem_set(item, bits);
}

void gr_telem_invalidate(struct gr_telem_item *item)
{
	*(volatile uint64_t *)&item->sample = 0;
	if (is_static(item)) {
		atomic_set(&static_dirty, 1);
	}
}

/* Copy len bytes of SMC memory to host IOVA host_addr. */
static int host_write(uint64_t host_addr, const void *src, uint32_t len)
{
	/* TODO: the SMC DMA probably needs the system address of src rather than its core-local
	 * address, and the outbound PCIe window needs to be programmed for host_addr.
	 */
	struct dma_block_config block = {
		.source_address = (uint64_t)(uintptr_t)src,
		.dest_address = CONFIG_TT_GR_TELEMETRY_HOST_WINDOW_BASE + host_addr,
		.block_size = len,
	};
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.block_count = 1,
		.head_block = &block,
	};
	int ret = dma_config(dma_dev, CONFIG_TT_GR_TELEMETRY_DMA_CHANNEL, &cfg);

	if (ret == 0) {
		ret = dma_start(dma_dev, CONFIG_TT_GR_TELEMETRY_DMA_CHANNEL);
	}
	return ret;
}

static int write_header_field(size_t offset, const uint32_t *field)
{
	return host_write(push.host_addr + offset, field, sizeof(*field));
}

/* Gather the static (or dynamic) items' values into stage.words; returns the block size. */
static uint32_t stage_values(bool want_static)
{
	uint32_t n = want_static ? static_count : item_count - static_count;
	uint32_t *valid = stage.words;
	uint32_t *values = stage.words + GR_TELEM_VALID_WORDS(n);
	uint32_t i = 0;

	memset(valid, 0, sizeof(uint32_t) * GR_TELEM_VALID_WORDS(n));
	STRUCT_SECTION_FOREACH(gr_telem_item, item) {
		if (is_static(item) != want_static) {
			continue;
		}
		uint64_t sample = *(volatile uint64_t *)&item->sample;

		if (sample & SAMPLE_VALID) {
			valid[i / 32] |= BIT(i % 32);
		}
		values[i++] = (uint32_t)sample;
	}
	return VALUE_BLOCK_SIZE(n);
}

static int write_static_block(void)
{
	atomic_set(&static_dirty, 0);

	uint32_t len = stage_values(true);
	int ret = host_write(push.host_addr + push.hdr.static_offset, stage.words, len);

	if (ret == 0) {
		push.hdr.static_gen++;
		ret = write_header_field(offsetof(struct gr_telem_buffer_header, static_gen),
					 &push.hdr.static_gen);
	}
	return ret;
}

static void push_stop(void)
{
	push.active = false;
	k_work_cancel_delayable(&push_work);
}

static void push_work_handler(struct k_work *work)
{
	if (!push.active) {
		return;
	}

	STRUCT_SECTION_FOREACH(gr_telem_sampler, sampler) {
		sampler->sample();
	}

	uint32_t seq = push.hdr.write_seq;
	uint64_t slot_addr = push.host_addr + push.hdr.ring_offset +
			     (uint64_t)(seq % push.hdr.num_frames) * push.hdr.frame_size;
	int ret = 0;

	/* The static block shares the staging buffer, so it goes out before the frame is staged. */
	if (atomic_get(&static_dirty)) {
		ret = write_static_block();
	}

	stage.hdr.seq = seq;
	stage.hdr.timestamp = k_ticks_to_us_floor64(k_uptime_ticks());
	stage_values(false);

	/* The frame must land before write_seq advances past it. host_write() returns only once
	 * the DMA has completed, so issuing the writes in order is enough.
	 */
	if (ret == 0) {
		ret = host_write(slot_addr, &stage, push.hdr.frame_size);
	}
	if (ret == 0) {
		push.hdr.write_seq = seq + 1;
		ret = write_header_field(offsetof(struct gr_telem_buffer_header, write_seq),
					 &push.hdr.write_seq);
	}
	if (ret != 0) {
		LOG_ERR("telemetry push to 0x%llx failed (%d), stopping", push.host_addr, ret);
		push_stop();
		return;
	}

	/* Absolute deadlines keep the frame period from drifting; after a stall, skip ahead. */
	push.next_tick =
		MAX(push.next_tick + k_ms_to_ticks_ceil64(push.period_ms), k_uptime_ticks());
	k_work_schedule(&push_work, K_TIMEOUT_ABS_TICKS(push.next_tick));
}

static uint8_t push_start(const struct gr_telemetry_rqst *rqst, struct response *rsp)
{
	uint64_t host_addr = ((uint64_t)rqst->buffer_addr_hi << 32) | rqst->buffer_addr_lo;
	uint32_t period_ms =
		rqst->period_ms ? rqst->period_ms : CONFIG_TT_GR_TELEMETRY_PUSH_PERIOD_DEFAULT_MS;
	uint32_t dynamic_count = item_count - static_count;
	uint32_t desc_offset = sizeof(struct gr_telem_buffer_header);
	uint32_t static_offset = desc_offset + item_count * sizeof(struct gr_telem_desc);
	uint32_t ring_offset = ROUND_UP(static_offset + VALUE_BLOCK_SIZE(static_count), 8);
	uint32_t frame_size =
		ROUND_UP(sizeof(struct gr_telem_frame_header) + VALUE_BLOCK_SIZE(dynamic_count), 8);

	if (rqst->chiplet != GR_TELEM_CHIPLET_SELF) {
		return GR_TELEM_ERR_INVALID_CHIPLET;
	}
	if (too_many_items) {
		return GR_TELEM_ERR_TOO_MANY_ITEMS;
	}
	if (rqst->buffer_size < ring_offset + frame_size) {
		return GR_TELEM_ERR_BUFFER_TOO_SMALL;
	}
	if (period_ms < CONFIG_TT_GR_TELEMETRY_PUSH_PERIOD_MIN_MS) {
		return GR_TELEM_ERR_PERIOD_TOO_SHORT;
	}

	push_stop();

	push.host_addr = host_addr;
	push.period_ms = period_ms;
	push.hdr = (struct gr_telem_buffer_header){
		.magic = 0,
		.version = GR_TELEM_VERSION,
		.header_size = sizeof(struct gr_telem_buffer_header),
		.chip_id = sys_read32(SMC_CPU_RESET_UNIT_CHIP_ID_REG_ADDR),
		.timebase_hz = TIMEBASE_HZ,
		.desc_count = item_count,
		.desc_offset = desc_offset,
		.static_count = static_count,
		.static_offset = static_offset,
		.dynamic_count = dynamic_count,
		.ring_offset = ring_offset,
		.frame_size = frame_size,
		.num_frames = (rqst->buffer_size - ring_offset) / frame_size,
		.period_ms = period_ms,
	};

	/* Invalidate a header left over from an earlier session before rewriting the layout. */
	int ret =
		write_header_field(offsetof(struct gr_telem_buffer_header, magic), &push.hdr.magic);
	uint32_t i = 0;

	/* Static descriptors first, then dynamic, matching the value block orders. */
	for (int pass = 0; pass < 2 && ret == 0; pass++) {
		STRUCT_SECTION_FOREACH(gr_telem_item, item) {
			if (is_static(item) != (pass == 0)) {
				continue;
			}
			ret = host_write(host_addr + desc_offset + i++ * sizeof(item->desc),
					 &item->desc, sizeof(item->desc));
			if (ret != 0) {
				break;
			}
		}
	}
	if (ret == 0) {
		ret = write_static_block();
	}
	if (ret == 0) {
		push.hdr.magic = GR_TELEM_MAGIC;
		ret = host_write(host_addr, &push.hdr, sizeof(push.hdr));
	}
	if (ret != 0) {
		return GR_TELEM_ERR_DMA;
	}

	push.active = true;
	push.next_tick = k_uptime_ticks();
	k_work_schedule(&push_work, K_NO_WAIT);

	LOG_INF("telemetry push: buffer=0x%llx items=%u (%u static) frames=%u period=%u ms",
		host_addr, item_count, static_count, push.hdr.num_frames, period_ms);
	rsp->data[1] = push.hdr.num_frames;
	return 0;
}

/**
 * @brief Handler for @ref TT_SMC_MSG_GR_TELEMETRY
 * @param[in] request @ref gr_telemetry_rqst
 * @param[out] response For PUSH_START, data[1] is the number of frames in the ring.
 * @return 0 on success, otherwise a @ref gr_telem_err
 */
static uint8_t gr_telemetry_handler(const union request *request, struct response *response)
{
	const struct gr_telemetry_rqst *rqst = &request->gr_telemetry;

	switch (rqst->subcmd) {
	case GR_TELEM_SUBCMD_PUSH_START:
		return push_start(rqst, response);
	case GR_TELEM_SUBCMD_PUSH_STOP:
		push_stop();
		return 0;
	default:
		return GR_TELEM_ERR_INVALID_SUBCMD;
	}
}
REGISTER_MESSAGE(TT_SMC_MSG_GR_TELEMETRY, gr_telemetry_handler);

static int gr_telemetry_init(void)
{
	STRUCT_SECTION_FOREACH(gr_telem_item, item) {
		STRUCT_SECTION_FOREACH(gr_telem_item, other) {
			if (other < item && other->desc.key == item->desc.key) {
				LOG_ERR("telemetry key 0x%08x used by both %s and %s",
					item->desc.key, other->desc.name, item->desc.name);
			}
		}
		item_count++;
		static_count += is_static(item);
	}
	too_many_items = MAX(static_count, item_count - static_count) > MAX_ITEMS;
	if (too_many_items) {
		LOG_ERR("%u telemetry items (%u static) exceed TT_GR_TELEMETRY_MAX_ITEMS=%u",
			item_count, static_count, MAX_ITEMS);
	}

	gr_telem_set(GR_TELEM_ITEM(fw_version), APPVERSION);
	gr_telem_set(GR_TELEM_ITEM(chip_id), sys_read32(SMC_CPU_RESET_UNIT_CHIP_ID_REG_ADDR));
	return 0;
}
SYS_INIT(gr_telemetry_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
