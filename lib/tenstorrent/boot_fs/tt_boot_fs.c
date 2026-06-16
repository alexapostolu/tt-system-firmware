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

tt_boot_fs boot_fs_data;

uint32_t tt_boot_fs_next(uint32_t last_fd_addr)
{
	return (last_fd_addr + sizeof(tt_boot_fs_fd));
}

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

static tt_checksum_res_t calculate_and_compare_checksum(uint8_t *data, size_t num_bytes,
							uint32_t expected, bool skip_checksum)
{
	uint32_t calculated_checksum;

	if (!skip_checksum) {
		calculated_checksum = tt_boot_fs_cksum(0, data, num_bytes);
		if (calculated_checksum != expected) {
			return TT_BOOT_FS_CHK_FAIL;
		}
	}

	return TT_BOOT_FS_CHK_OK;
}

/**
 * @brief Reads and validates the boot-fs header at @ref TT_BOOT_FS_HEADER_ADDR
 *
 * @retval 0 on success, @p header populated
 * @retval -EIO Flash read failure
 * @retval -ENXIO Magic or version mismatch
 */
static int read_boot_fs_header(const struct device *dev, tt_boot_fs_header *header)
{
	int ret = flash_read(dev, TT_BOOT_FS_HEADER_ADDR, header, sizeof(*header));

	if (ret < 0) {
		LOG_ERR("%s() failed: %d", "flash_read", ret);
		return -EIO;
	}
	if (header->magic != TT_BOOT_FS_MAGIC) {
		LOG_ERR("Invalid boot FS magic: 0x%08X", header->magic);
		return -ENXIO;
	}
	if (header->version != TT_BOOT_FS_CURRENT_VERSION) {
		LOG_ERR("Unsupported boot FS version: %d", header->version);
		return -ENXIO;
	}

	return 0;
}

/**
 * @brief Reads the @p table_index'th table base address from the boot-fs header
 *
 * @retval 0 on success, @p table_addr populated
 * @retval -EIO Flash read failure
 */
static int read_table_addr(const struct device *dev, size_t table_index, uint32_t *table_addr)
{
	uint32_t addr = TT_BOOT_FS_HEADER_ADDR + sizeof(tt_boot_fs_header) +
			table_index * sizeof(uint32_t);
	int ret = flash_read(dev, addr, table_addr, sizeof(*table_addr));

	if (ret < 0) {
		LOG_ERR("%s() failed: %d", "flash_read", ret);
		return -EIO;
	}

	return 0;
}

/**
 * @brief Reads and validates a single descriptor at absolute flash address @p fd_addr
 *
 * Streams one descriptor from flash rather than preloading multiple entries, so
 * callers can iterate the file system without committing a large on-stack
 * buffer that would scale with @ref CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX.
 *
 * @retval 0 If @p fd is populated with a valid descriptor
 * @retval 1 If end of table sentinel; caller should stop iterating this table
 * @retval -EIO Flash read failure
 * @retval -ENXIO Checksum failure
 */
static int read_and_validate_fd(const struct device *dev, uint32_t fd_addr, tt_boot_fs_fd *fd)
{
	int ret = flash_read(dev, fd_addr, fd, sizeof(*fd));

	if (ret < 0) {
		LOG_ERR("%s() failed: %d", "flash_read", ret);
		return -EIO;
	}

	if (fd->flags.f.invalid) {
		return 1;
	}

	if (calculate_and_compare_checksum((uint8_t *)fd, sizeof(*fd) - sizeof(uint32_t),
					   fd->fd_crc, false) != TT_BOOT_FS_CHK_OK) {
		return -ENXIO;
	}

	return 0;
}

int tt_boot_fs_ls(const struct device *dev, tt_boot_fs_fd *fds, size_t nfds, size_t offset)
{
	if (!dev || !device_is_ready(dev)) {
		return -ENXIO;
	}

	if (nfds == 0) {
		return 0;
	}

	tt_boot_fs_header header;
	int ret = read_boot_fs_header(dev, &header);

	if (ret < 0) {
		return ret;
	}

	size_t found = 0;
	size_t i = 0;

	for (size_t t = 0; t < header.table_count; t++) {
		uint32_t fd_addr;

		ret = read_table_addr(dev, t, &fd_addr);
		if (ret < 0) {
			return ret;
		}

		/* Stream descriptors from this table until sentinel or buffer full */
		while (found < nfds) {
			tt_boot_fs_fd fd;

			ret = read_and_validate_fd(dev, fd_addr, &fd);
			if (ret < 0) {
				return ret;
			}
			if (ret == 1) {
				break;
			}

			if (i >= offset) {
				if (fds != NULL && found < nfds) {
					fds[found] = fd;
				}
				found++;
				if (found == nfds) {
					return found;
				}
			}
			i++;
			fd_addr += sizeof(tt_boot_fs_fd);
		}
	}

	return found;
}

int tt_boot_fs_find_fd_by_tag(const struct device *flash_dev, const uint8_t *tag, tt_boot_fs_fd *fd)
{
	if (tag == NULL) {
		return -EINVAL;
	}

	if (!flash_dev || !device_is_ready(flash_dev)) {
		return -ENXIO;
	}

	tt_boot_fs_header header;
	int ret = read_boot_fs_header(flash_dev, &header);

	if (ret < 0) {
		return ret;
	}

	/*
	 * Stream descriptors one at a time across every table advertised by the
	 * boot-fs header. This avoids allocating a CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX
	 * -sized tt_boot_fs_fd[] on the stack, which is what previously caused a
	 * stack overflow when the descriptor cap was raised.
	 */
	for (size_t t = 0; t < header.table_count; t++) {
		uint32_t fd_addr;

		ret = read_table_addr(flash_dev, t, &fd_addr);
		if (ret < 0) {
			return ret;
		}

		for (size_t i = 0; i < CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX; i++) {
			tt_boot_fs_fd cur;

			ret = read_and_validate_fd(flash_dev, fd_addr, &cur);
			if (ret < 0) {
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
	}

	return -ENOENT;
}
