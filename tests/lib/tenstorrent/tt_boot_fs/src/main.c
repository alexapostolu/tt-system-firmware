/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/devicetree.h>
#include <string.h>
#include <tenstorrent/tt_boot_fs.h>

#define FLASH_NODE DT_NODELABEL(flashcontroller0)

const struct device *FLASH_DEVICE = DEVICE_DT_GET(FLASH_NODE);

#define MAX_FDS CONFIG_TT_BOOT_FS_IMAGE_COUNT_MAX

#define IMAGE_ADDR     0x14000
#define TEST_ALIGNMENT 0x1000

static void setup_fd(tt_boot_fs_fd *fd, uint32_t spi_addr, uint32_t copy_dest, uint32_t flags,
		     const char *tag, const uint8_t *img, size_t img_len)
{
	memset(fd, 0, sizeof(*fd));
	fd->spi_addr = spi_addr;
	fd->copy_dest = copy_dest;
	fd->flags.val = flags;
	fd->data_crc = tt_boot_fs_cksum(0, img, img_len);
	fd->security_flags.val = 0;
	memset(fd->image_tag, 0, TT_BOOT_FS_IMAGE_TAG_SIZE);
	memcpy(fd->image_tag, tag, strlen(tag));
	fd->fd_crc = 0;
}

/* The three valid descriptors of the test filesystem, slots 0..2 */
static tt_boot_fs_fd fds[3];

/* Descriptor sector: 4 KiB at the head of the table */
#define FD_SECTOR_SIZE 4096

/*
 * Rewrite the descriptor sector: the three valid descriptors followed by
 * @p slot3, whatever the caller wants the walk to run into at slot 3. The
 * image payloads live in other sectors and are left alone.
 */
static void write_descriptor_sector(const tt_boot_fs_fd *slot3)
{
	int rc = flash_erase(FLASH_DEVICE, TT_BOOT_FS_FD_HEAD_ADDR, FD_SECTOR_SIZE);

	zassert_equal(rc, 0, "Failed to erase descriptor sector");

	for (size_t i = 0; i < ARRAY_SIZE(fds); ++i) {
		rc = flash_write(FLASH_DEVICE, TT_BOOT_FS_FD_HEAD_ADDR + i * sizeof(tt_boot_fs_fd),
				 &fds[i], sizeof(tt_boot_fs_fd));
		zassert_equal(rc, 0, "Failed to write fd[%zu] to flash", i);
	}

	if (slot3 != NULL) {
		rc = flash_write(FLASH_DEVICE, TT_BOOT_FS_FD_HEAD_ADDR + 3 * sizeof(tt_boot_fs_fd),
				 slot3, sizeof(tt_boot_fs_fd));
		zassert_equal(rc, 0, "Failed to write slot 3 to flash");
	}
}

/* The sentinel the pre-multi-table tooling wrote: zeros, invalid flag, valid checksum */
static void written_sentinel(tt_boot_fs_fd *fd)
{
	memset(fd, 0, sizeof(*fd));
	fd->flags.f.invalid = 1;
	fd->fd_crc = tt_boot_fs_cksum(0, (uint8_t *)fd, sizeof(*fd) - sizeof(fd->fd_crc));
}

static void *setup_bootfs(void)
{
	printk("FLASH_DEVICE: %p\n", FLASH_DEVICE);
	printk("Flash device name: %s\n", FLASH_DEVICE->name);
	printk("Flash device ready: %s\n", device_is_ready(FLASH_DEVICE) ? "YES" : "NO");
	zassert_not_null(FLASH_DEVICE, "FLASH_DEVICE is NULL!");

	uint8_t image_A[] = {0x73, 0x73, 0x42, 0x42};
	uint8_t image_B[] = {0x73, 0x73, 0x42, 0x42, 0x37, 0x37, 0x24, 0x24};
	uint8_t image_C[] = {0x73, 0x73, 0x42, 0x42};

	uint32_t spi_addr = IMAGE_ADDR;
#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

	setup_fd(&fds[0], spi_addr, 0x1000000, (sizeof(image_A) & 0xFFFFFF) | (1 << 25), "imageA",
		 image_A, sizeof(image_A));
	spi_addr += ALIGN_UP(sizeof(image_A), TEST_ALIGNMENT);

	setup_fd(&fds[1], spi_addr, 0, (sizeof(image_B) & 0xFFFFFF), "imageB", image_B,
		 sizeof(image_B));
	spi_addr += ALIGN_UP(sizeof(image_B), TEST_ALIGNMENT);

	setup_fd(&fds[2], spi_addr, 0x1000000, (sizeof(image_C) & 0xFFFFFF), "failover", image_C,
		 sizeof(image_C));

	for (size_t i = 0; i < 3; ++i) {
		fds[i].fd_crc = tt_boot_fs_cksum(0, (uint8_t *)&fds[i],
						 sizeof(tt_boot_fs_fd) - sizeof(fds[i].fd_crc));
	}

	uint32_t erase_size =
		(spi_addr + ALIGN_UP(sizeof(image_C), TEST_ALIGNMENT)) - TT_BOOT_FS_FD_HEAD_ADDR;
	int rc = flash_erase(FLASH_DEVICE, TT_BOOT_FS_FD_HEAD_ADDR, ROUND_UP(erase_size, 4096));

	zassert_equal(rc, 0, "Failed to erase test bootfs area in flash");

	tt_boot_fs_fd invalid_fd;

	written_sentinel(&invalid_fd);
	write_descriptor_sector(&invalid_fd);

	rc = flash_write(FLASH_DEVICE, fds[0].spi_addr, image_A, sizeof(image_A));
	zassert_equal(rc, 0, "Failed to write image_A to flash");
	rc = flash_write(FLASH_DEVICE, fds[1].spi_addr, image_B, sizeof(image_B));
	zassert_equal(rc, 0, "Failed to write image_B to flash");
	rc = flash_write(FLASH_DEVICE, fds[2].spi_addr, image_C, sizeof(image_C));
	zassert_equal(rc, 0, "Failed to write image_C to flash");

	return NULL;
}

/* Put the standard layout back so the remaining tests see what setup wrote */
static void restore_bootfs(void *fixture)
{
	ARG_UNUSED(fixture);

	tt_boot_fs_fd invalid_fd;

	written_sentinel(&invalid_fd);
	write_descriptor_sector(&invalid_fd);
}

/* all input must be aligned to a 4-byte boundary and be a multiple of 4 bytes */
__aligned(sizeof(uint32_t)) static const uint8_t one_byte[] = {0x42};
/*
 * __aligned(sizeof(uint32_t)) static const uint8_t two_bytes[] = {
 *   0x42,
 *   0x42,
 * };
 * __aligned(sizeof(uint32_t)) static const uint8_t three_bytes[] = {
 *   0x73,
 *   0x42,
 *   0x42,
 * };
 */
static const uint32_t four_bytes = 0x42427373;
/*
 * __aligned(sizeof(uint32_t)) static const uint8_t five_bytes[] = {
 *   0x73, 0x73, 0x42, 0x42, 0x37,
 * };
 * __aligned(sizeof(uint32_t)) static const uint8_t six_bytes[] = {
 *   0x73, 0x73, 0x42, 0x42, 0x37, 0x37,
 * };
 * __aligned(sizeof(uint32_t)) static const uint8_t seven_bytes[] = {
 *   0x73, 0x73, 0x42, 0x42, 0x37, 0x37, 0x24,
 * };
 */
static const uint64_t eight_bytes = 0x2424373742427373;

ZTEST(tt_boot_fs, test_tt_boot_fs_cksum)
{
	uint32_t cksum;

	static const struct harness_data {
		uint32_t expect;
		const uint8_t *data;
		size_t size;
	} harness[] = {
		{0, NULL, 0},
		{0, one_byte, 0},
		/*
		 * {0x00000042, one_byte, 1},
		 * {0x00004242, two_bytes, 2},
		 * {0x00000073, three_bytes, 3},
		 */
		{0x42427373, (uint8_t *)&four_bytes, 4},
		/*
		 *{0x4284e6e6, five_bytes, 5},
		 *{0x4242e6e6, six_bytes, 6},
		 *{0x424273e6, seven_bytes, 7},
		 */
		{0x6666aaaa, (uint8_t *)&eight_bytes, 8},
	};

	ARRAY_FOR_EACH_PTR(harness, it) {
		cksum = tt_boot_fs_cksum(0, it->data, it->size);
		zassert_equal(it->expect, cksum, "%d: expected: %08x actual: %08x", it - harness,
			      it->expect, cksum);
	}
}

#define DECL_TEST_SPEC(d, f, n, o, e)                                                              \
	(struct test_spec)                                                                         \
	{                                                                                          \
		.dev = d, .fds = f, .nfds = n, .offset = o, .expect = e                            \
	}

ZTEST(tt_boot_fs, test_boot_fs_ls)
{
	static tt_boot_fs_fd fds[MAX_FDS];
	const struct device *valid_dev = FLASH_DEVICE;
	const struct device *null_dev = NULL;

	const int total_valid_fds_on_flash = 3;

	struct test_spec {
		const struct device *dev;
		tt_boot_fs_fd *fds;
		size_t nfds;
		size_t offset;
		int expect;
	} specs[10];

	size_t i = 0;

	specs[i++] = DECL_TEST_SPEC(null_dev, fds, MAX_FDS, 0, -ENXIO);
	specs[i++] = DECL_TEST_SPEC(valid_dev, NULL, MAX_FDS, 0, total_valid_fds_on_flash);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, 0, 0, 0);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, 1, 0, 1);

	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, MAX_FDS, 0, total_valid_fds_on_flash);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, 2, 0, 2);

	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, MAX_FDS, 1, 2);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, MAX_FDS, 2, 1);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, MAX_FDS, 3, 0);
	specs[i++] = DECL_TEST_SPEC(valid_dev, fds, MAX_FDS, 4, 0);

	ARRAY_FOR_EACH(specs, i) {
		struct test_spec *spec = &specs[i];

		int actual = tt_boot_fs_ls(spec->dev, spec->fds, spec->nfds, spec->offset);

		zassert_equal(
			actual, spec->expect,
			"Case %zu: tt_boot_fs_ls(dev:%p, fds:%p, nfds:%zu, offset:%zu) failed. "
			"Got %d, expected %d",
			i, spec->dev, spec->fds, spec->nfds, spec->offset, actual, spec->expect);
	}
}

#define DECL_TEST_FIND_SPEC(d, t, f, e)                                                            \
	(struct test_spec)                                                                         \
	{                                                                                          \
		.dev = (d), .tag = (t), .fd_out = (f), .expect = (e)                               \
	}

ZTEST(tt_boot_fs, test_find_fd_by_tag)
{
	tt_boot_fs_fd result_fd;
	const struct device *valid_dev = FLASH_DEVICE;
	const struct device *null_dev = NULL;

	const uint8_t found_tag[8] = "imageA";
	const uint8_t not_found_tag[8] = "notFound";

	struct test_spec {
		const struct device *dev;
		const uint8_t *tag;
		tt_boot_fs_fd *fd_out;
		int expect;
	} specs[6];

	size_t i = 0;

	specs[i++] = DECL_TEST_FIND_SPEC(null_dev, found_tag, &result_fd, -ENXIO);
	specs[i++] = DECL_TEST_FIND_SPEC(valid_dev, found_tag, NULL, 0);
	specs[i++] = DECL_TEST_FIND_SPEC(valid_dev, not_found_tag, NULL, -ENOENT);
	specs[i++] = DECL_TEST_FIND_SPEC(valid_dev, NULL, &result_fd, -EINVAL);
	specs[i++] = DECL_TEST_FIND_SPEC(valid_dev, found_tag, &result_fd, 0);
	specs[i++] = DECL_TEST_FIND_SPEC(valid_dev, not_found_tag, &result_fd, -ENOENT);

	ARRAY_FOR_EACH(specs, i) {
		struct test_spec *spec = &specs[i];

		int actual = tt_boot_fs_find_fd_by_tag(spec->dev, spec->tag, spec->fd_out);

		zassert_equal(actual, spec->expect,
			      "Case %zu: find(tag:\"%s\") failed. Got %d, expected %d", i,
			      spec->tag, actual, spec->expect);
		if (actual == 0 && spec->fd_out != NULL) {
			zassert_mem_equal(spec->fd_out->image_tag, found_tag,
					  TT_BOOT_FS_IMAGE_TAG_SIZE,
					  "Case %zu: Returned FD tag does not match", i);
		}
	}
}

/*
 * An erased slot (all 0xFF) is the sentinel the current tooling writes; the
 * walk must stop there exactly as it does on the explicitly written one.
 */
ZTEST(tt_boot_fs, test_erased_slot_is_a_sentinel)
{
	const uint8_t not_found_tag[8] = "notFound";
	const struct tt_boot_fs_diag *diag = tt_boot_fs_get_diag();

	/* Leaving slot 3 unwritten after the erase is what an erased sentinel is */
	write_descriptor_sector(NULL);

	zassert_equal(tt_boot_fs_ls(FLASH_DEVICE, NULL, MAX_FDS, 0), 3,
		      "ls should count the three descriptors before the erased slot");

	uint32_t not_found_before = diag->not_found;

	zassert_equal(tt_boot_fs_find_fd_by_tag(FLASH_DEVICE, not_found_tag, NULL), -ENOENT,
		      "a missing tag is -ENOENT when the walk reaches a genuine sentinel");
	zassert_equal(diag->not_found, not_found_before + 1, "not_found should count the miss");
	zassert_equal(diag->last_end_slot, 3, "the walk should have stopped at slot 3");
	zassert_equal(diag->last_end_word, 0xFFFFFFFF, "the terminating word should be erased");
	zassert_mem_equal(diag->last_tag, not_found_tag, TT_BOOT_FS_IMAGE_TAG_SIZE,
			  "the missed tag should be recorded");
}

/*
 * A read that has the invalid flag set but is neither erased nor a checksummed
 * descriptor is not a sentinel: it is what a flash that did not answer, or a
 * corrupted read, looks like. It must fail loudly rather than end the table.
 */
ZTEST(tt_boot_fs, test_corrupt_invalid_descriptor_is_rejected)
{
	const uint8_t found_tag[8] = "imageA";
	const uint8_t not_found_tag[8] = "notFound";
	const struct tt_boot_fs_diag *diag = tt_boot_fs_get_diag();
	tt_boot_fs_fd corrupt;

	memset(&corrupt, 0xA5, sizeof(corrupt));
	corrupt.flags.f.invalid = 1;
	write_descriptor_sector(&corrupt);

	uint32_t corrupt_before = diag->corrupt_fds;

	zassert_equal(tt_boot_fs_find_fd_by_tag(FLASH_DEVICE, not_found_tag, NULL), -ENXIO,
		      "a corrupt slot must be reported, not treated as end of table");
	zassert_equal(diag->corrupt_fds, corrupt_before + 1, "corrupt_fds should count it");
	zassert_equal(tt_boot_fs_ls(FLASH_DEVICE, NULL, MAX_FDS, 0), -ENXIO,
		      "ls must report the corrupt slot too");

	/* Descriptors before the corrupt slot are still found */
	zassert_equal(tt_boot_fs_find_fd_by_tag(FLASH_DEVICE, found_tag, NULL), 0,
		      "a tag ahead of the corrupt slot is unaffected");
}

/*
 * An all-zero read passes the word-sum checksum (0 == 0) and would previously
 * have been walked over as a nameless descriptor. Nothing valid is all zeros.
 */
ZTEST(tt_boot_fs, test_all_zero_descriptor_is_rejected)
{
	const uint8_t not_found_tag[8] = "notFound";
	const struct tt_boot_fs_diag *diag = tt_boot_fs_get_diag();
	tt_boot_fs_fd zero;

	memset(&zero, 0, sizeof(zero));
	write_descriptor_sector(&zero);

	uint32_t corrupt_before = diag->corrupt_fds;

	zassert_equal(tt_boot_fs_find_fd_by_tag(FLASH_DEVICE, not_found_tag, NULL), -ENXIO,
		      "an all-zero slot must be reported as corrupt");
	zassert_equal(diag->corrupt_fds, corrupt_before + 1, "corrupt_fds should count it");
	zassert_equal(tt_boot_fs_ls(FLASH_DEVICE, NULL, MAX_FDS, 0), -ENXIO,
		      "ls must report the all-zero slot too");
}

ZTEST_SUITE(tt_boot_fs, NULL, setup_bootfs, NULL, restore_bootfs, NULL);
