/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_GR_TELEMETRY_H_
#define TENSTORRENT_GR_TELEMETRY_H_

#include <stdint.h>

/**
 * @defgroup gr_telemetry Grendel telemetry
 * @brief Host-visible Grendel telemetry ABI.
 *
 * Telemetry is pushed, never polled. The host allocates a buffer and hands it to firmware with
 * @ref GR_TELEM_SUBCMD_PUSH_START; firmware then writes everything the host needs into it:
 *
 * - @ref gr_telem_buffer_header: layout, source chiplet and timebase.
 * - One @ref gr_telem_desc per item: key, name, decoder, unit and scale. The host needs no
 *   built-in knowledge of the items, and firmware publishes values in whatever unit their source
 *   produces.
 * - The static block: values that don't change until reset, written once (and again only if one
 *   becomes valid later, see @ref gr_telem_buffer_header::static_gen).
 * - A ring of timestamped frames holding the dynamic values, one frame per push period.
 *
 * Each value block (the static block and each frame's payload) is a validity bitmap of
 * GR_TELEM_VALID_WORDS(n) words followed by n 32-bit values. Static values follow the order of
 * the static descriptors and frame values the order of the dynamic descriptors.
 *
 * Items are keyed by @ref GR_TELEM_KEY: a group owned by one component (power, fabric, runtime,
 * ...) and an index allocated by that group. Keys are stable across firmware builds; positions
 * in the buffer are not.
 * @{
 */

#define GR_TELEM_VERSION 2

#define GR_TELEM_KEY(group, index) (((uint32_t)(group) << 16) | (uint16_t)(index))
#define GR_TELEM_KEY_GROUP(key)    ((uint16_t)((key) >> 16))
#define GR_TELEM_KEY_INDEX(key)    ((uint16_t)(key))

/** @brief Telemetry groups; each owns the index space of its keys */
enum gr_telem_group {
	GR_TELEM_GROUP_SYSTEM = 0x0001,
	GR_TELEM_GROUP_POWER = 0x0002,
	GR_TELEM_GROUP_THERMAL = 0x0003,
	GR_TELEM_GROUP_FABRIC = 0x0010,
	GR_TELEM_GROUP_RUNTIME = 0x0020,
};

/** @brief How the host decodes a 32-bit value */
enum gr_telem_decoder {
	/** @brief Unsigned 32-bit integer */
	GR_TELEM_DEC_U32 = 1,
	/** @brief Two's complement signed 32-bit integer */
	GR_TELEM_DEC_S32 = 2,
	/** @brief IEEE 754 binary32 floating point */
	GR_TELEM_DEC_IEEE754_F32 = 3,
	/** @brief Signed 16.16 fixed point (the Blackhole telemetry encoding) */
	GR_TELEM_DEC_FIXED_S16_16 = 4,
	/** @brief Boolean: 0 is false, anything else is true */
	GR_TELEM_DEC_BOOL = 5,
	/** @brief Opaque bits (IDs, bitfields), displayed as hex */
	GR_TELEM_DEC_HEX = 6,
	/** @brief Firmware version packed as major[31:24].minor[23:16].patch[15:8].rc[7:0] */
	GR_TELEM_DEC_VERSION = 7,
};

/** @brief Physical unit of a decoded value, before @ref gr_telem_desc::scale is applied */
enum gr_telem_unit {
	GR_TELEM_UNIT_NONE = 0,
	GR_TELEM_UNIT_VOLT = 1,
	GR_TELEM_UNIT_AMPERE = 2,
	GR_TELEM_UNIT_WATT = 3,
	GR_TELEM_UNIT_CELSIUS = 4,
	GR_TELEM_UNIT_SECOND = 5,
	GR_TELEM_UNIT_HERTZ = 6,
	GR_TELEM_UNIT_BYTE = 7,
};

/** @brief The value doesn't change until reset; it lives in the static block, not in frames */
#define GR_TELEM_FLAG_STATIC 0x01

#define GR_TELEM_NAME_LEN 24

/**
 * @brief Description of one item.
 *
 * The physical value is decoded_value * 10^scale in @ref unit, e.g. a U32 with unit
 * @ref GR_TELEM_UNIT_AMPERE and scale -2 counts 10 mA steps.
 */
struct gr_telem_desc {
	/** @brief @ref GR_TELEM_KEY */
	uint32_t key;
	/** @brief @ref gr_telem_decoder */
	uint8_t decoder;
	/** @brief @ref gr_telem_unit */
	uint8_t unit;
	/** @brief Decimal exponent applied to the decoded value */
	int8_t scale;
	/** @brief GR_TELEM_FLAG_* */
	uint8_t flags;
	/** @brief NUL-terminated name, e.g. "vcore" */
	char name[GR_TELEM_NAME_LEN];
};

/** @brief Words in the validity bitmap of a value block of @p n values */
#define GR_TELEM_VALID_WORDS(n) (((n) + 31) / 32)

#define GR_TELEM_MAGIC 0x4D545247 /* "GRTM" */

/** @brief @ref gr_telemetry_rqst::chiplet value selecting the chiplet that handles the request */
#define GR_TELEM_CHIPLET_SELF 0xFF

/**
 * @brief Start of the host buffer, written by firmware.
 *
 * Firmware clears @ref magic first and writes it last, so the rest of the header and everything
 * it points to is valid once the host sees @ref GR_TELEM_MAGIC. All offsets are from the start
 * of the buffer.
 *
 * Firmware writes frame N to slot N % num_frames and only then advances @ref write_seq to N + 1.
 * To read the newest frame the host takes slot (write_seq - 1) % num_frames and checks that the
 * frame's @ref gr_telem_frame_header::seq is write_seq - 1; a mismatch means firmware lapped the
 * reader.
 */
struct gr_telem_buffer_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	/** @brief SMC reset-unit CHIP_ID of the chiplet these items come from */
	uint32_t chip_id;
	/** @brief Ticks per second of @ref gr_telem_frame_header::timestamp */
	uint32_t timebase_hz;
	uint32_t desc_count;
	uint32_t desc_offset;
	uint32_t static_count;
	uint32_t static_offset;
	/** @brief Values per frame */
	uint32_t dynamic_count;
	uint32_t ring_offset;
	/** @brief Bytes per frame, including @ref gr_telem_frame_header; a multiple of 8 */
	uint32_t frame_size;
	uint32_t num_frames;
	/** @brief Incremented after each rewrite of the static block */
	uint32_t static_gen;
	/** @brief Number of frames written so far */
	uint32_t write_seq;
	/** @brief Interval between frames, in ms */
	uint32_t period_ms;
	uint32_t reserved;
};

/** @brief Start of each frame, followed by its value block */
struct gr_telem_frame_header {
	/** @brief Sequence number of this frame */
	uint32_t seq;
	uint32_t reserved;
	/** @brief When the frame was sampled, in timebase_hz ticks since firmware start */
	uint64_t timestamp;
};

/** @brief Sub-commands of @ref TT_SMC_MSG_GR_TELEMETRY */
enum gr_telem_subcmd {
	/**
	 * @brief Start pushing to a host buffer.
	 *
	 * Restarts with the new buffer if already pushing. Response data[1] is the number of
	 * frames in the ring.
	 */
	GR_TELEM_SUBCMD_PUSH_START = 2,
	/** @brief Stop pushing; the host may free the buffer once this completes */
	GR_TELEM_SUBCMD_PUSH_STOP = 3,
};

/** @brief Exit codes of @ref TT_SMC_MSG_GR_TELEMETRY */
enum gr_telem_err {
	GR_TELEM_ERR_INVALID_SUBCMD = 1,
	/** @brief This firmware can't serve telemetry for the requested chiplet */
	GR_TELEM_ERR_INVALID_CHIPLET = 2,
	/** @brief Buffer too small for the header, descriptors, static block and one frame */
	GR_TELEM_ERR_BUFFER_TOO_SMALL = 3,
	/** @brief Push period below the firmware minimum */
	GR_TELEM_ERR_PERIOD_TOO_SHORT = 4,
	/** @brief Firmware could not write the buffer */
	GR_TELEM_ERR_DMA = 5,
	/** @brief The firmware build registers more items than CONFIG_TT_GR_TELEMETRY_MAX_ITEMS */
	GR_TELEM_ERR_TOO_MANY_ITEMS = 6,
};

/** @} */

#ifdef __ZEPHYR__

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/toolchain.h>

/**
 * @brief A telemetry item and its latest value.
 *
 * Define items with @ref GR_TELEM_ITEM_DEFINE in the component that owns them; the telemetry
 * core finds every item at link time, so adding one doesn't touch the core.
 */
struct gr_telem_item {
	struct gr_telem_desc desc;
	/** @brief value[31:0] and valid[32], replaced with one 64-bit store */
	uint64_t sample;
};

/**
 * @brief Define an item.
 *
 * @param _name   C identifier, also the name the host sees
 * @param _group  @ref gr_telem_group
 * @param _index  Index within the group, allocated by the group owner
 * @param _dec    @ref gr_telem_decoder
 * @param _unit   @ref gr_telem_unit
 * @param _scale  Decimal exponent, see @ref gr_telem_desc
 * @param _flags  GR_TELEM_FLAG_*
 */
#define GR_TELEM_ITEM_DEFINE(_name, _group, _index, _dec, _unit, _scale, _flags)                   \
	BUILD_ASSERT(sizeof(#_name) <= GR_TELEM_NAME_LEN, "telemetry name too long: " #_name);     \
	STRUCT_SECTION_ITERABLE(gr_telem_item, gr_telem_##_name) = {                               \
		.desc =                                                                            \
			{                                                                          \
				.key = GR_TELEM_KEY(_group, _index),                               \
				.decoder = (_dec),                                                 \
				.unit = (_unit),                                                   \
				.scale = (_scale),                                                 \
				.flags = (_flags),                                                 \
				.name = #_name,                                                    \
			},                                                                         \
	}

#define GR_TELEM_ITEM_DECLARE(_name) extern struct gr_telem_item gr_telem_##_name
#define GR_TELEM_ITEM(_name)         (&gr_telem_##_name)

/**
 * @brief Callback run right before each frame is sampled.
 *
 * Sources that are cheap to read register one so their values share the frame's timestamp.
 * Sources that sample on their own schedule just call @ref gr_telem_set whenever they have a
 * new value.
 */
struct gr_telem_sampler {
	void (*sample)(void);
};

#define GR_TELEM_SAMPLER_DEFINE(_name, _fn)                                                        \
	STRUCT_SECTION_ITERABLE(gr_telem_sampler, gr_telem_sampler_##_name) = {.sample = (_fn)}

/** @brief Publish a value; safe from any context */
void gr_telem_set(struct gr_telem_item *item, uint32_t value);

/** @brief Publish an IEEE 754 binary32 value */
void gr_telem_set_f32(struct gr_telem_item *item, float value);

/** @brief Mark an item's value as unavailable */
void gr_telem_invalidate(struct gr_telem_item *item);

#endif /* __ZEPHYR__ */

#endif
