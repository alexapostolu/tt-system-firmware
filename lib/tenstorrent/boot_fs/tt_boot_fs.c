/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <tenstorrent/tt_boot_fs.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tt_boot_fs, CONFIG_TT_APP_LOG_LEVEL);

uint32_t tt_boot_fs_cksum(uint32_t cksum, const uint8_t *data, size_t num_bytes)
{
	if (num_bytes == 0 || data == NULL) {
		return 0;
	}

	/* Always read 1 fewer word, and handle the 4 possible alignment cases outside the loop */
	const uint32_t num_dwords = num_bytes / sizeof(uint32_t) - 1;
	uint32_t *data_as_dwords = (uint32_t *)data;

	for (uint32_t i = 0; i < num_dwords; i++) {
		cksum += *data_as_dwords++;
	}

	switch (num_bytes % 4) {
	case 0:
		cksum += *data_as_dwords & 0xffffffff;
		break;
	default:
		__ASSERT(false, "size %zu is not a multiple of 4", num_bytes);
		break;
	}

	return cksum;
}

static struct tt_boot_fs_diag diag;

const struct tt_boot_fs_diag *tt_boot_fs_get_diag(void)
{
	return &diag;
}

/**
 * @brief Header and table pointer array as they sit on flash
 *
 * Read in one transfer from @ref TT_BOOT_FS_HEADER_ADDR, which is 16-byte
 * aligned, rather than as a 12-byte header followed by one 4-byte read per
 * pointer. A single transfer means the pointers can never be read at a
 * different sample point, or in a different bus state, from the header that
 * vouches for them, and the only read address involved ends in a zero nibble.
 */
struct boot_fs_layout {
	tt_boot_fs_header hdr;
	uint32_t table_addr[TT_BOOT_FS_TABLE_COUNT_MAX];
	/* Not on flash: true when @p hdr carried no magic and the fixed addresses were used */
	bool legacy;
};

#define BOOT_FS_LAYOUT_FLASH_BYTES                                                                 \
	(sizeof(tt_boot_fs_header) + TT_BOOT_FS_TABLE_COUNT_MAX * sizeof(uint32_t))

BUILD_ASSERT(offsetof(struct boot_fs_layout, table_addr) == sizeof(tt_boot_fs_header),
	     "table pointers must directly follow the header, as they do on flash");

/**
 * @brief Validates the embedded checksum of a boot-fs file descriptor.
 *
 * The descriptor stores its checksum in the trailing @p fd_crc word, computed
 * over every preceding byte of the structure.
 */
static bool fd_crc_ok(const tt_boot_fs_fd *fd)
{
	const size_t hashed_len = sizeof(*fd) - sizeof(fd->fd_crc);

	return tt_boot_fs_cksum(0, (const uint8_t *)fd, hashed_len) == fd->fd_crc;
}

static bool fd_is_filled_with(const tt_boot_fs_fd *fd, uint8_t byte)
{
	const uint8_t *raw = (const uint8_t *)fd;

	for (size_t i = 0; i < sizeof(*fd); i++) {
		if (raw[i] != byte) {
			return false;
		}
	}

	return true;
}

/**
 * @brief Reads the header and pointer array without judging them
 *
 * Images created before the multi-table header was introduced are represented
 * as two tables at the fixed ROM and failover addresses.
 *
 * @retval 0    @p lay populated
 * @retval -EIO Flash read failure
 */
static int read_layout_raw(const struct device *dev, struct boot_fs_layout *lay)
{
	int ret = flash_read(dev, TT_BOOT_FS_HEADER_ADDR, lay, BOOT_FS_LAYOUT_FLASH_BYTES);

	if (ret < 0) {
		LOG_ERR("%s(header @ 0x%08x) failed: %d", "flash_read",
			(uint32_t)TT_BOOT_FS_HEADER_ADDR, ret);
		return -EIO;
	}

	lay->legacy = lay->hdr.magic != TT_BOOT_FS_MAGIC;
	if (lay->legacy) {
		lay->hdr.version = 0;
		lay->hdr.table_count = 2;
		lay->table_addr[0] = TT_BOOT_FS_ROM_HEAD_ADDR;
		lay->table_addr[1] = TT_BOOT_FS_FAILOVER_HEAD_ADDR;
	}

	return 0;
}

/**
 * @brief Checks that a table pointer could plausibly point at a descriptor table
 *
 * A descriptor table is an array of 32-byte descriptors, so its base is
 * 32-byte aligned; it cannot overlap the header that advertises it; and it must
 * lie within the flash device when the device reports its size.
 */
static bool table_addr_plausible(const struct device *dev, uint32_t addr)
{
	uint64_t flash_size;

	if ((addr % sizeof(tt_boot_fs_fd)) != 0) {
		return false;
	}
	if (addr >= TT_BOOT_FS_HEADER_ADDR &&
	    addr < TT_BOOT_FS_HEADER_ADDR + BOOT_FS_LAYOUT_FLASH_BYTES) {
		return false;
	}
	if (flash_get_size(dev, &flash_size) == 0 && flash_size != 0 &&
	    (uint64_t)addr + sizeof(tt_boot_fs_fd) > flash_size) {
		return false;
	}

	return true;
}

static void record_layout(const struct boot_fs_layout *lay)
{
	diag.last_header[0] = lay->hdr.magic;
	diag.last_header[1] = lay->hdr.version;
	diag.last_header[2] = lay->hdr.table_count;
	memcpy(diag.last_table_addr, lay->table_addr, sizeof(diag.last_table_addr));
}

/**
 * @brief Reads and validates the boot-fs header and table pointers
 *
 * @retval 0      Success, @p lay populated (possibly with the legacy addresses)
 * @retval -EIO   Flash read failure
 * @retval -ENXIO Unsupported version, bad table count or implausible table pointer
 */
static int read_layout(const struct device *dev, struct boot_fs_layout *lay)
{
	static bool warned_legacy;
	int ret = read_layout_raw(dev, lay);

	if (ret < 0) {
		diag.io_errors++;
		return ret;
	}

	if (lay->legacy) {
		diag.legacy_fallbacks++;
		if (!warned_legacy) {
			warned_legacy = true;
			LOG_WRN("No multi-table boot FS header at 0x%08x (read %08x %08x %08x); "
				"using legacy table addresses",
				(uint32_t)TT_BOOT_FS_HEADER_ADDR, lay->hdr.magic, lay->hdr.version,
				lay->hdr.table_count);
		}
		return 0;
	}

	if (lay->hdr.version != TT_BOOT_FS_CURRENT_VERSION) {
		record_layout(lay);
		LOG_ERR("Unsupported boot FS version: %u", lay->hdr.version);
		return -ENXIO;
	}
	if (lay->hdr.table_count == 0 || lay->hdr.table_count > TT_BOOT_FS_TABLE_COUNT_MAX) {
		record_layout(lay);
		LOG_ERR("Invalid boot FS table count: %u", lay->hdr.table_count);
		return -ENXIO;
	}

	for (uint32_t t = 0; t < lay->hdr.table_count; t++) {
		if (!table_addr_plausible(dev, lay->table_addr[t])) {
			diag.ptr_rejects++;
			record_layout(lay);
			LOG_ERR("Boot FS table %u pointer 0x%08x rejected (header %08x %08x %u)", t,
				lay->table_addr[t], lay->hdr.magic, lay->hdr.version,
				lay->hdr.table_count);
			return -ENXIO;
		}
	}

	return 0;
}

/**
 * @brief Reads and validates a single descriptor at absolute flash address @p fd_addr
 *
 * A read is only accepted as the end-of-table sentinel if it is a genuine one:
 * either an erased slot (all 0xFF) or a descriptor that was deliberately written
 * with the invalid flag set and a valid checksum. A read that merely has bit 24
 * set, or that came back as all zeros (which the word-sum checksum accepts), is
 * reported as corrupt instead of silently ending the walk, so that a flash that
 * did not answer a read cannot masquerade as an empty table.
 *
 * @retval 0 If @p fd is populated with a valid descriptor
 * @retval 1 If end of table sentinel; caller should stop iterating this table
 * @retval -EIO Flash read failure
 * @retval -ENXIO Corrupt descriptor (checksum failure, blank or all-zero read)
 */
static int read_and_validate_fd(const struct device *dev, uint32_t fd_addr, tt_boot_fs_fd *fd)
{
	int ret = flash_read(dev, fd_addr, fd, sizeof(*fd));

	if (ret < 0) {
		diag.io_errors++;
		LOG_ERR("%s(fd @ 0x%08x) failed: %d", "flash_read", fd_addr, ret);
		return -EIO;
	}

	if (fd->flags.f.invalid) {
		if (fd_is_filled_with(fd, 0xFF) || fd_crc_ok(fd)) {
			return 1;
		}

		diag.corrupt_fds++;
		LOG_ERR("fd @ 0x%08x: invalid flag set on a corrupt read "
			"(spi_addr 0x%08x flags 0x%08x fd_crc 0x%08x)",
			fd_addr, fd->spi_addr, fd->flags.val, fd->fd_crc);
		return -ENXIO;
	}

	if (fd_is_filled_with(fd, 0x00)) {
		diag.corrupt_fds++;
		LOG_ERR("fd @ 0x%08x: descriptor read back as all zeros", fd_addr);
		return -ENXIO;
	}

	if (!fd_crc_ok(fd)) {
		diag.corrupt_fds++;
		LOG_ERR("fd @ 0x%08x: descriptor checksum mismatch "
			"(spi_addr 0x%08x flags 0x%08x fd_crc 0x%08x)",
			fd_addr, fd->spi_addr, fd->flags.val, fd->fd_crc);
		return -ENXIO;
	}

	return 0;
}

/**
 * @brief Records how a failed walk ended and checks whether the layout reads back the same
 *
 * The re-read is the cheapest possible discriminator between "the flash holds
 * these words" and "this read of them was wrong": if the header and pointers
 * come back different a moment later, the read path, not the image, is at
 * fault.
 */
static void record_failed_walk(const struct device *dev, const struct boot_fs_layout *lay,
			       size_t table, size_t slot, const tt_boot_fs_fd *last,
			       const uint8_t *tag)
{
	struct boot_fs_layout again;

	record_layout(lay);
	diag.last_end_table = table;
	diag.last_end_slot = slot;
	diag.last_end_word = last->flags.val;
	if (tag != NULL) {
		strncpy((char *)diag.last_tag, (const char *)tag, sizeof(diag.last_tag));
	}

	if (lay->legacy || read_layout_raw(dev, &again) < 0) {
		return;
	}

	if (memcmp(&lay->hdr, &again.hdr, sizeof(again.hdr)) != 0 ||
	    memcmp(lay->table_addr, again.table_addr,
		   lay->hdr.table_count * sizeof(lay->table_addr[0])) != 0) {
		diag.layout_rereads_differ++;
		LOG_ERR("Boot FS layout re-read differs: header %08x %08x %08x -> %08x %08x %08x, "
			"table0 0x%08x -> 0x%08x",
			lay->hdr.magic, lay->hdr.version, lay->hdr.table_count, again.hdr.magic,
			again.hdr.version, again.hdr.table_count, lay->table_addr[0],
			again.table_addr[0]);
	}
}

int tt_boot_fs_ls(const struct device *dev, tt_boot_fs_fd *fds, size_t nfds, size_t offset)
{
	if (!dev || !device_is_ready(dev)) {
		return -ENXIO;
	}

	bool count_only = fds == NULL;

	if (!count_only && nfds == 0) {
		return 0;
	}

	struct boot_fs_layout lay;
	int ret = read_layout(dev, &lay);

	if (ret < 0) {
		return ret;
	}

	/*
	 * input_index counts every valid descriptor encountered across all
	 * tables, so it can be compared against @p offset to skip prefixes.
	 * output_index counts descriptors accepted into @p fds (or, when
	 * @p fds is NULL, the total match count returned to the caller).
	 */
	size_t input_index = 0;
	size_t output_index = 0;

	for (size_t t = 0; t < lay.hdr.table_count; t++) {
		uint32_t fd_addr = lay.table_addr[t];
		size_t entry;

		/* Stream descriptors from this table until sentinel or configured limit. */
		for (entry = 0; entry < CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX; entry++) {
			tt_boot_fs_fd fd;

			ret = read_and_validate_fd(dev, fd_addr, &fd);
			if (ret < 0) {
				return ret;
			}
			if (ret == 1) {
				break;
			}

			if (count_only) {
				output_index++;
			} else if (input_index >= offset) {
				fds[output_index++] = fd;
				if (output_index == nfds) {
					return output_index;
				}
			}
			input_index++;
			fd_addr += sizeof(tt_boot_fs_fd);
		}

		/* An advertised table has at least one descriptor; see tt_boot_fs_find_fd_by_tag */
		if (entry == 0 && !lay.legacy) {
			diag.ptr_rejects++;
			record_layout(&lay);
			LOG_ERR("Boot FS table %zu at 0x%08x is empty; pointer not trusted", t,
				lay.table_addr[t]);
			return -ENXIO;
		}
	}

	return output_index;
}

int tt_boot_fs_find_fd_by_tag(const struct device *flash_dev, const uint8_t *tag, tt_boot_fs_fd *fd)
{
	if (tag == NULL) {
		return -EINVAL;
	}

	if (!flash_dev || !device_is_ready(flash_dev)) {
		return -ENXIO;
	}

	diag.lookups++;

	struct boot_fs_layout lay;
	int ret = read_layout(flash_dev, &lay);

	if (ret < 0) {
		return ret;
	}

	/*
	 * Stream descriptors one at a time across every table advertised by the
	 * boot-fs header. This avoids allocating a CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX
	 * -sized tt_boot_fs_fd[] on the stack, which is what previously caused a
	 * stack overflow when the descriptor cap was raised.
	 */
	tt_boot_fs_fd cur = {0};
	bool rom_table_empty = false;
	size_t t;
	size_t i = 0;

	for (t = 0; t < lay.hdr.table_count; t++) {
		uint32_t fd_addr = lay.table_addr[t];

		for (i = 0; i < CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX; i++) {
			ret = read_and_validate_fd(flash_dev, fd_addr, &cur);
			if (ret < 0) {
				record_failed_walk(flash_dev, &lay, t, i, &cur, tag);
				return ret;
			}
			if (ret == 1) {
				break;
			}

			if (strncmp(tag, cur.image_tag, sizeof(cur.image_tag)) == 0) {
				if (fd != NULL) {
					*fd = cur;
				}
				return 0;
			}

			fd_addr += sizeof(tt_boot_fs_fd);
		}

		/*
		 * Every table the header advertises was written with at least one
		 * descriptor, so a sentinel at slot 0 means this pointer did not
		 * point at the table: either the pointer or the slot was misread.
		 * Report it rather than treating the table as empty, so the caller
		 * retries and the raw pointer is kept for the host to see.
		 */
		if (i == 0) {
			if (!lay.legacy) {
				diag.ptr_rejects++;
				record_failed_walk(flash_dev, &lay, t, i, &cur, tag);
				LOG_ERR("Boot FS table %zu at 0x%08x is empty (flags 0x%08x); "
					"pointer "
					"not trusted",
					t, lay.table_addr[t], cur.flags.val);
				return -ENXIO;
			}
			if (t == 0) {
				/* Legacy images have no header to vouch for the table; note it */
				rom_table_empty = true;
			}
		}
	}

	diag.not_found++;
	record_failed_walk(flash_dev, &lay, lay.hdr.table_count - 1, i, &cur, tag);

	/* Tags are at most TT_BOOT_FS_IMAGE_TAG_SIZE bytes and need not be NUL-terminated */
	char tag_str[TT_BOOT_FS_IMAGE_TAG_SIZE + 1] = {0};

	memcpy(tag_str, diag.last_tag, sizeof(diag.last_tag));

	if (rom_table_empty) {
		LOG_ERR("'%s' not found: first descriptor at 0x%08x reads as end of table", tag_str,
			lay.table_addr[0]);
	} else {
		LOG_WRN("'%s' not found in %u table(s) (%s, table0 0x%08x, last table ended at "
			"slot "
			"%zu with flags 0x%08x)",
			tag_str, lay.hdr.table_count, lay.legacy ? "legacy" : "header",
			lay.table_addr[0], i, cur.flags.val);
	}

	return -ENOENT;
}
