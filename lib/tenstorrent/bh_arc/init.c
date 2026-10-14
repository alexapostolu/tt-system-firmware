/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "init.h"
#include "reg.h"
#include "status_reg.h"
#include "timer.h"
#include "cm2dm_msg.h"
#include <stdint.h>

#if defined(HAS_APP_VERSION)
#include <zephyr/app_version.h>
#else
#define APPVERSION         0x00000000
#define APP_VERSION_STRING "unknown"
#endif

#include <tenstorrent/post_code.h>
#include <tenstorrent/sys_init_defines.h>
#include <tenstorrent/tt_boot_fs.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_BH_FWTABLE) && DT_NODE_HAS_STATUS(DT_NODELABEL(fwtable), okay)
#define HAS_FWTABLE 1
static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));
#else
#define HAS_FWTABLE 0
#endif

#define FW_VERSION_SEMANTIC APPVERSION
#define FW_VERSION_DATE     0x00000000
#define FW_VERSION_LOW      0x00000000
#define FW_VERSION_HIGH     0x00000000

uint32_t FW_VERSION[4] __attribute__((section(".fw_version"))) = {
	FW_VERSION_SEMANTIC, FW_VERSION_DATE, FW_VERSION_LOW, FW_VERSION_HIGH};

static int tt_appversion_init(void)
{
	WriteReg(STATUS_FW_VERSION_REG_ADDR, APPVERSION);
	return 0;
}
SYS_INIT(tt_appversion_init, EARLY, 0);

static int record_cmfw_start_time(void)
{
	WriteReg(CMFW_START_TIME_REG_ADDR, TimerTimestamp());
	return 0;
}
SYS_INIT(record_cmfw_start_time, EARLY, 0);

/*
 * Clear the per-boot flash diagnostics before any flash access, so a value
 * left over from the previous boot can never be mistaken for this one's. The
 * training register is set to "not run" rather than zero, since 0 is a valid
 * delay.
 */
static int clear_flash_diag_regs(void)
{
	WriteReg(BOOT_FS_DIAG_REG_ADDR, 0);
	WriteReg(BOOT_FS_DIAG_WORD_REG_ADDR, 0);
	WriteReg(FLASH_RX_TRAINING_REG_ADDR, UINT32_MAX);
	WriteReg(BOOT_FS_DIAG_PTR_REG_ADDR, 0);
	WriteReg(BOOT_FS_DIAG_LAYOUT_REG_ADDR, 0);
	WriteReg(INIT_STACK_HEADROOM_REG_ADDR, UINT32_MAX);
	WriteReg(BOOT_FS_DIAG_END_PTR_REG_ADDR, 0);
	return 0;
}
SYS_INIT(clear_flash_diag_regs, EARLY, 0);

static uint32_t saturate(uint32_t value, uint32_t max)
{
	return MIN(value, max);
}

/*
 * Publish the boot-fs read counters where the host can get at them without
 * PCIe: the register survives until the next reset and is readable over JTAG.
 */
static void publish_boot_fs_diag(void)
{
	const struct tt_boot_fs_diag *diag = tt_boot_fs_get_diag();
	uint32_t retries = 0;
	uint32_t end_table = MIN(diag->last_end_table, TT_BOOT_FS_TABLE_COUNT_MAX - 1);

#if HAS_FWTABLE
	retries = tt_bh_fwtable_get_load_retries(fwtable_dev);
#endif

	WriteReg(BOOT_FS_DIAG_REG_ADDR,
		 FIELD_PREP(GENMASK(7, 0), saturate(diag->not_found, 0xFF)) |
			 FIELD_PREP(GENMASK(15, 8), saturate(diag->corrupt_fds, 0xFF)) |
			 FIELD_PREP(GENMASK(19, 16), saturate(diag->io_errors, 0xF)) |
			 FIELD_PREP(GENMASK(23, 20), saturate(retries, 0xF)) |
			 FIELD_PREP(GENMASK(31, 24), saturate(diag->last_end_slot, 0xFF)));
	WriteReg(BOOT_FS_DIAG_WORD_REG_ADDR, diag->last_end_word);
	WriteReg(BOOT_FS_DIAG_PTR_REG_ADDR, diag->last_table_addr[0]);
	WriteReg(BOOT_FS_DIAG_END_PTR_REG_ADDR, diag->last_table_addr[end_table]);
	WriteReg(BOOT_FS_DIAG_LAYOUT_REG_ADDR,
		 FIELD_PREP(GENMASK(7, 0), saturate(diag->legacy_fallbacks, 0xFF)) |
			 FIELD_PREP(GENMASK(15, 8), saturate(diag->ptr_rejects, 0xFF)) |
			 FIELD_PREP(GENMASK(23, 16), saturate(diag->layout_rereads_differ, 0xFF)) |
			 FIELD_PREP(GENMASK(27, 24), saturate(diag->last_end_table, 0xF)));
}

/*
 * How close the init sequence came to overflowing the thread stack it runs
 * on. Every SYS_INIT at POST_KERNEL executes on the main thread, so its
 * high-water mark at the end of init is the one that matters for a silent
 * overflow during boot.
 */
static void publish_init_stack_headroom(void)
{
#ifdef CONFIG_INIT_STACKS
	size_t unused;

	if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
		WriteReg(INIT_STACK_HEADROOM_REG_ADDR, unused);
		if (unused < CONFIG_MAIN_STACK_SIZE / 8) {
			printk("WARNING: init thread stack headroom is only %zu of %u bytes\n",
			       unused, CONFIG_MAIN_STACK_SIZE);
		}
	}
#endif
}

static int bh_arc_init_start(void)
{
	/* Write a status register indicating HW init progress */
	STATUS_BOOT_STATUS0_reg_u boot_status0 = {0};

	boot_status0.val = ReadReg(STATUS_BOOT_STATUS0_REG_ADDR);
	boot_status0.f.hw_init_status = kHwInitStarted;
	WriteReg(STATUS_BOOT_STATUS0_REG_ADDR, boot_status0.val);

	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP1);
	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ARC_INIT_STEP2);

	/*
	 * The firmware tables were loaded (or not) by the bh_fwtable driver
	 * before this point. Without them every later stage runs on zeroed
	 * tables and PCIe stays disabled, so name the real failure here rather
	 * than only the downstream ones.
	 */
#if HAS_FWTABLE
	if (!device_is_ready(fwtable_dev)) {
		record_init_failure(INIT_STAGE_FWTABLE);
	}
#endif
	publish_boot_fs_diag();

	return 0;
}
SYS_INIT_APP(bh_arc_init_start);

static int bh_arc_init_end(void)
{
	STATUS_BOOT_STATUS0_reg_u boot_status0 = {0};

	/* Indicate successful HW Init */
	boot_status0.val = ReadReg(STATUS_BOOT_STATUS0_REG_ADDR);
	/* Record FW ID */
	if (IS_ENABLED(CONFIG_TT_SMC_RECOVERY)) {
		boot_status0.f.fw_id = FW_ID_SMC_RECOVERY;
	} else {
		boot_status0.f.fw_id = FW_ID_SMC_NORMAL;
	}

	boot_status0.f.hw_init_status = (error_status0 != 0) ? kHwInitError : kHwInitDone;
	WriteReg(STATUS_BOOT_STATUS0_REG_ADDR, boot_status0.val);
	WriteReg(STATUS_ERROR_STATUS0_REG_ADDR, error_status0);

	/* Final snapshot: every boot-time flash lookup has run by now */
	publish_boot_fs_diag();
	publish_init_stack_headroom();

	SetPostCode(POST_CODE_SRC_CMFW, POST_CODE_ZEPHYR_INIT_DONE);
	printk("Tenstorrent Blackhole CMFW %s\n", APP_VERSION_STRING);

	Dm2CmReadyRequest();
	return 0;
}
SYS_INIT_APP(bh_arc_init_end);
