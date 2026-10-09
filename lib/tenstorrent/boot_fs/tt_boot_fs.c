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
static tt_boot_fs_fd boot_fs_cache[CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX];

uint32_t tt_boot_fs_next(uint32_t last_fd_addr)
{
	return (last_fd_addr + sizeof(tt_boot_fs_fd));
}

static int tt_boot_fs_load_cache(tt_boot_fs *tt_boot_fs)
{
	tt_boot_fs->hal_spi_read_f(TT_BOOT_FS_FD_HEAD_ADDR, sizeof(boot_fs_cache),
				   (uint8_t *)boot_fs_cache);

	return TT_BOOT_FS_OK;
}

/* Sets up hardware abstraction layer (HAL) callbacks, initializes HEAD fd */
int tt_boot_fs_mount(tt_boot_fs *tt_boot_fs, tt_boot_fs_read hal_read, tt_boot_fs_write hal_write,
		     tt_boot_fs_erase hal_erase)
{
	tt_boot_fs->hal_spi_read_f = hal_read;
	tt_boot_fs->hal_spi_write_f = hal_write;
	tt_boot_fs->hal_spi_erase_f = hal_erase;

	return tt_boot_fs_load_cache(tt_boot_fs);
}

/* Allocate new file descriptor on SPI device and write associated data to correct address */
int tt_boot_fs_add_file(const tt_boot_fs *tt_boot_fs, tt_boot_fs_fd fd,
			const uint8_t *image_data_src, bool isFailoverEntry,
			bool isSecurityBinaryEntry)
{
	uint32_t curr_fd_addr;

	/* Failover image has specific file descriptor location (BOOT_START + DESC_REGION_SIZE) */
	if (isFailoverEntry) {
		curr_fd_addr = TT_BOOT_FS_FAILOVER_HEAD_ADDR;
	} else if (isSecurityBinaryEntry) {
		curr_fd_addr = TT_BOOT_FS_SECURITY_BINARY_FD_ADDR;
	} else {
		/* Regular file descriptor */
		tt_boot_fs_fd head = {0};

		curr_fd_addr = TT_BOOT_FS_FD_HEAD_ADDR;

		tt_boot_fs->hal_spi_read_f(TT_BOOT_FS_FD_HEAD_ADDR, sizeof(tt_boot_fs_fd),
					   (uint8_t *)&head);

		/* Traverse until we find an invalid file descriptor entry in SPI device array */
		while (head.flags.f.invalid == 0) {
			curr_fd_addr = tt_boot_fs_next(curr_fd_addr);
			tt_boot_fs->hal_spi_read_f(curr_fd_addr, sizeof(tt_boot_fs_fd),
						   (uint8_t *)&head);
		}
	}

	tt_boot_fs->hal_spi_write_f(curr_fd_addr, sizeof(tt_boot_fs_fd), (uint8_t *)&fd);

	/*
	 * Now copy total image size from image_data_src pointer into the specified address.
	 * Total image size = image_size + signature_size (security) + padding.
	 */
	uint32_t total_image_size = fd.flags.f.image_size + fd.security_flags.f.signature_size;

	tt_boot_fs->hal_spi_write_f(fd.spi_addr, total_image_size, image_data_src);

	return TT_BOOT_FS_OK;
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

static int find_fd_by_tag(const tt_boot_fs *tt_boot_fs, const uint8_t *tag, tt_boot_fs_fd *fd_data)
{
	for (uint32_t i = 0; i < ARRAY_SIZE(boot_fs_cache); i++) {
		if (boot_fs_cache[i].flags.f.invalid) {
			continue;
		}

		if (memcmp(boot_fs_cache[i].image_tag, tag, TT_BOOT_FS_IMAGE_TAG_SIZE) != 0) {
			continue;
		}

		tt_checksum_res_t chk_res = calculate_and_compare_checksum(
			(uint8_t *)&boot_fs_cache[i], sizeof(tt_boot_fs_fd) - sizeof(uint32_t),
			boot_fs_cache[i].fd_crc, false);

		if (chk_res == TT_BOOT_FS_CHK_FAIL) {
			continue;
		}

		/* Found the right file descriptor */
		*fd_data = boot_fs_cache[i];
		return TT_BOOT_FS_OK;
	}

	/* File descriptor not found */
	return TT_BOOT_FS_ERR;
}

int tt_boot_fs_get_file(const tt_boot_fs *tt_boot_fs, const uint8_t *tag, uint8_t *buf,
			size_t buf_size, size_t *file_size)
{
	tt_boot_fs_fd fd_data;

	if (tt_boot_fs == NULL || tag == NULL || buf == NULL || file_size == NULL) {
		return TT_BOOT_FS_ERR;
	}

	if (find_fd_by_tag(tt_boot_fs, tag, &fd_data) != TT_BOOT_FS_OK) {
		return TT_BOOT_FS_ERR;
	}

	if (fd_data.flags.f.image_size > buf_size) {
		return TT_BOOT_FS_ERR;
	}
	*file_size = fd_data.flags.f.image_size;

	tt_boot_fs->hal_spi_read_f(fd_data.spi_addr, fd_data.flags.f.image_size, buf);
	if (calculate_and_compare_checksum(buf, fd_data.flags.f.image_size, fd_data.data_crc,
					   false) != TT_BOOT_FS_CHK_OK) {
		return TT_BOOT_FS_ERR;
	}

	return TT_BOOT_FS_OK;
}

static struct tt_boot_fs_diag diag;

const struct tt_boot_fs_diag *tt_boot_fs_get_diag(void)
{
	return &diag;
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

static bool fd_crc_ok(const tt_boot_fs_fd *fd)
{
	return calculate_and_compare_checksum((uint8_t *)fd, sizeof(*fd) - sizeof(fd->fd_crc),
					      fd->fd_crc, false) == TT_BOOT_FS_CHK_OK;
}

/**
 * @brief Reads and validates the descriptor at slot @p index
 *
 * A read is only accepted as the end-of-table sentinel if it is a genuine one:
 * either an erased slot (all 0xFF) or a descriptor that was deliberately written
 * with the invalid flag set and a valid checksum. A read that merely has bit 24
 * set, or that came back as all zeros (which the word-sum checksum accepts), is
 * reported as corrupt instead of silently ending the walk, so that a flash that
 * did not answer a read cannot masquerade as an empty table.
 *
 * @retval 0 If @p fd is populated with a valid descriptor
 * @retval 1 If end of table sentinel; caller should stop iterating
 * @retval -EIO Flash read failure
 * @retval -ENXIO Corrupt descriptor (checksum failure, blank or all-zero read)
 */
static int read_and_validate_fd(const struct device *dev, size_t index, tt_boot_fs_fd *fd)
{
	const uint32_t fd_addr = TT_BOOT_FS_FD_HEAD_ADDR + index * sizeof(*fd);
	int ret = flash_read(dev, fd_addr, fd, sizeof(*fd));

	if (ret < 0) {
		diag.io_errors++;
		LOG_ERR("%s() failed: %d", "flash_read", ret);
		return -EIO;
	}

	if (fd->flags.f.invalid) {
		if (fd_is_filled_with(fd, 0xFF) || fd_crc_ok(fd)) {
			return 1;
		}

		diag.corrupt_fds++;
		LOG_ERR("slot %zu @ 0x%08x: invalid flag set on a corrupt read "
			"(spi_addr 0x%08x flags 0x%08x fd_crc 0x%08x)",
			index, fd_addr, fd->spi_addr, fd->flags.val, fd->fd_crc);
		return -ENXIO;
	}

	if (fd_is_filled_with(fd, 0x00)) {
		diag.corrupt_fds++;
		LOG_ERR("slot %zu @ 0x%08x: descriptor read back as all zeros", index, fd_addr);
		return -ENXIO;
	}

	if (!fd_crc_ok(fd)) {
		diag.corrupt_fds++;
		LOG_ERR("slot %zu @ 0x%08x: descriptor checksum mismatch "
			"(spi_addr 0x%08x flags 0x%08x fd_crc 0x%08x)",
			index, fd_addr, fd->spi_addr, fd->flags.val, fd->fd_crc);
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

	size_t found = 0;

	for (size_t i = 0; i < CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX; i++) {
		tt_boot_fs_fd fd;
		int ret = read_and_validate_fd(dev, i, &fd);

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
				break;
			}
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

	diag.lookups++;

	tt_boot_fs_fd cur = {0};
	size_t i;

	for (i = 0; i < CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX; i++) {
		int ret = read_and_validate_fd(flash_dev, i, &cur);

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
	}

	/*
	 * Record how the walk ended so the failure can be told apart after the
	 * fact: a sentinel at slot 0 means the very first read looked like the
	 * end of the table, which no written image has.
	 */
	diag.not_found++;
	diag.last_end_slot = i;
	diag.last_end_word = cur.flags.val;
	strncpy((char *)diag.last_tag, (const char *)tag, sizeof(diag.last_tag));

	/* Tags are at most TT_BOOT_FS_IMAGE_TAG_SIZE bytes and need not be NUL-terminated */
	char tag_str[TT_BOOT_FS_IMAGE_TAG_SIZE + 1] = {0};

	memcpy(tag_str, diag.last_tag, sizeof(diag.last_tag));

	if (i == 0) {
		LOG_ERR("'%s' not found: first descriptor at 0x%08x reads as end of table "
			"(flags 0x%08x)",
			tag_str, (uint32_t)TT_BOOT_FS_FD_HEAD_ADDR, cur.flags.val);
	} else {
		LOG_WRN("'%s' not found after %zu descriptor(s) (last flags 0x%08x)", tag_str, i,
			cur.flags.val);
	}

	return -ENOENT;
}
