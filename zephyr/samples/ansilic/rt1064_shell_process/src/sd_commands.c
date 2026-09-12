/*
 * SD card commands for the zepLinux shell.
 *
 *   sdinfo   - reported capacity, filesystem size and free space
 *   sdtest   - raw sector write/read-back verification
 *   sdclear  - fast clear: delete every file/directory, optionally re-format
 *   sdwipe   - secure erase: overwrite the whole card with zeros
 *
 * Two layers are used deliberately:
 *
 *   - disk_access_* works on raw sectors and needs no filesystem, so
 *     sdtest/sdwipe stay usable on a blank or unrecognised card.
 *   - the Zephyr FS API is used where file semantics are actually wanted
 *     (free-space reporting, deleting files, formatting).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdlib.h>

/* ELM FATFS volume work area (ff.h comes from the fatfs module). */
#include <ff.h>

#include "shell_process.h"

/* Disk name comes from the board devicetree: sdmmc { disk-name = "SD"; } */
#define SD_DISK_NAME "SD"

/*
 * Zephyr mount points are rooted paths; the FATFS backend strips the leading
 * '/' and hands the rest ("SD:") to f_mount(). Declaring this as bare "SD:"
 * makes the VFS reject the mount with -EINVAL, because fat_fs.c asserts the
 * path begins with '/'.
 */
#define SD_MOUNT_POINT "/" SD_DISK_NAME ":"

/* Raw-phase chunk. 32 KiB keeps the command stack small. */
#define SD_CHUNK_BYTES (32 * 1024)

#define SD_PATH_MAX 256

static uint8_t sd_buf[SD_CHUNK_BYTES] __aligned(4);

/*
 * The filesystem core keeps a pointer to this for as long as the volume stays
 * mounted, so it must outlive the command that mounts it. A stack-local
 * fs_mount_t would leave the VFS holding a dangling pointer.
 *
 * fs_data must point at a FATFS volume work area. Leaving it NULL makes
 * f_mount() unregister the drive instead of registering it, and every later
 * call then fails with FR_NOT_ENABLED - which surfaces here as -ENODEV (-19).
 */
static FATFS sd_fatfs;

static struct fs_mount_t sd_mnt = {
	.type = FS_FATFS,
	.fs_data = &sd_fatfs,
	.storage_dev = (void *)SD_DISK_NAME,
	.mnt_point = SD_MOUNT_POINT,
};

static bool sd_mounted;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int sd_disk_open(uint32_t *sector_count, uint32_t *sector_size)
{
	int ret;

	ret = disk_access_init(SD_DISK_NAME);
	if (ret != 0) {
		printk("sd: disk_access_init(\"%s\") failed: %d\n", SD_DISK_NAME, ret);
		return ret;
	}
	printk("sd: disk \"%s\" initialised\n", SD_DISK_NAME);

	ret = disk_access_status(SD_DISK_NAME);
	if (ret != DISK_STATUS_OK) {
		printk("sd: card not usable (status 0x%02X: %s%s%s)\n", ret,
		       (ret & DISK_STATUS_UNINIT) ? "uninitialised " : "",
		       (ret & DISK_STATUS_NOMEDIA) ? "no-media " : "",
		       (ret & DISK_STATUS_WR_PROTECT) ? "write-protected" : "");
		return -ENODEV;
	}

	ret = disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT,
				sector_count);
	if (ret != 0) {
		printk("sd: cannot read sector count: %d\n", ret);
		return ret;
	}

	ret = disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE,
				sector_size);
	if (ret != 0) {
		printk("sd: cannot read sector size: %d\n", ret);
		return ret;
	}

	if (*sector_count == 0 || *sector_size == 0) {
		printk("sd: card reports zero capacity\n");
		return -ENODEV;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* volume management                                                   */
/* ------------------------------------------------------------------ */

/*
 * Guards every mount/unmount, because the hotplug thread below and the shell
 * commands (which each run in their own process) both drive the volume.
 */
static K_MUTEX_DEFINE(sd_lock);

/*
 * Whether the socket currently holds a card. This works even before the disk
 * is initialised: the sdmmc disk driver's status op checks its card-detect
 * line before reporting anything else.
 */
static bool sd_card_present(void)
{
	int st = disk_access_status(SD_DISK_NAME);

	return (st >= 0) && ((st & DISK_STATUS_NOMEDIA) == 0);
}

/* Bring the disk up and mount the volume. Caller holds sd_lock. */
static int sd_mount_locked(void)
{
	int ret = disk_access_init(SD_DISK_NAME);

	if (ret != 0) {
		return ret;
	}

	ret = fs_mount(&sd_mnt);
	if (ret == 0) {
		sd_mounted = true;
	}

	return ret;
}

/*
 * Tear the volume down and force the disk refcount back to zero. The refcount
 * matters for hotplug: disk_access_init() only runs the driver's init while the
 * count is zero, so without this a card removed and re-inserted would never be
 * re-initialised. Caller holds sd_lock.
 */
static void sd_unmount_locked(void)
{
	if (sd_mounted) {
		fs_unmount(&sd_mnt);
		sd_mounted = false;
	}

	bool force = true;

	disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, &force);
}

/* Mount the volume once and remember it; returns 0 when mounted. */
static int sd_ensure_mounted(void)
{
	int ret;

	k_mutex_lock(&sd_lock, K_FOREVER);

	if (sd_mounted) {
		ret = 0;
	} else {
		ret = sd_mount_locked();
	}

	k_mutex_unlock(&sd_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* hotplug                                                             */
/* ------------------------------------------------------------------ */

/*
 * Poll the card-detect line and keep the volume in step with the socket, so a
 * card inserted after boot is usable without any command.
 *
 * Polling rather than an interrupt: the CD line is already muxed as a plain
 * GPIO for cd-gpios, and the SD stack's own detect handling runs off the driver
 * rather than exposing an event. A few hundred ms costs nothing here and
 * avoids fighting the driver over the same pin.
 */
#define SD_HOTPLUG_POLL_MS 500

static K_THREAD_STACK_DEFINE(sd_hotplug_stack, 2048);
static struct k_thread sd_hotplug_thread;

static void sd_hotplug_entry(void *p1, void *p2, void *p3)
{
	bool present = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Give the SDHC driver a moment to probe whatever is in the socket. */
	k_msleep(SD_HOTPLUG_POLL_MS);

	while (true) {
		bool now = sd_card_present();

		if (now != present) {
			k_mutex_lock(&sd_lock, K_FOREVER);

			if (now) {
				int ret = sd_mount_locked();

				if (ret == 0) {
					printk("\n[sd] card inserted - mounted at %s\n",
					       SD_MOUNT_POINT);
				} else {
					printk("\n[sd] card inserted - mount failed (%d)\n",
					       ret);
				}
			} else {
				sd_unmount_locked();
				printk("\n[sd] card removed - unmounted\n");
			}

			k_mutex_unlock(&sd_lock);
			present = now;
		}

		k_msleep(SD_HOTPLUG_POLL_MS);
	}
}

static int sd_hotplug_start(void)
{
	k_tid_t tid = k_thread_create(&sd_hotplug_thread, sd_hotplug_stack,
				      K_THREAD_STACK_SIZEOF(sd_hotplug_stack),
				      sd_hotplug_entry, NULL, NULL, NULL,
				      K_PRIO_PREEMPT(10), 0, K_NO_WAIT);

	k_thread_name_set(tid, "sd_hotplug");
	return 0;
}

/* After the command registrations, so `ls` output stays stable. */
SYS_INIT(sd_hotplug_start, APPLICATION, 103);

static void sd_print_size(uint64_t bytes)
{
	uint32_t mib = (uint32_t)(bytes >> 20);

	if (mib >= 1024U) {
		printk("%u.%02u GiB (%llu bytes)",
		       mib / 1024U, (mib % 1024U) * 100U / 1024U,
		       (unsigned long long)bytes);
	} else {
		printk("%u MiB (%llu bytes)", mib, (unsigned long long)bytes);
	}
}

/* ------------------------------------------------------------------ */
/* sdinfo                                                              */
/* ------------------------------------------------------------------ */

static int cmd_sdinfo(int argc, char **argv)
{
	uint32_t sectors = 0, ssize = 0;
	struct fs_statvfs vfs;
	int ret;

	printk("=== SD card information ===\n");

	ret = sd_disk_open(&sectors, &ssize);
	if (ret != 0) {
		return ret;
	}

	printk("disk name      : %s\n", SD_DISK_NAME);
	printk("sector size    : %u bytes\n", ssize);
	printk("sector count   : %u\n", sectors);
	printk("card capacity  : ");
	sd_print_size((uint64_t)sectors * ssize);
	printk("\n");

	ret = sd_ensure_mounted();
	if (ret != 0) {
		printk("filesystem     : not mounted (%d)\n", ret);
		printk("                 card may be blank or not FAT - try "
		       "'sdclear format'\n");
		return 0;
	}

	if (fs_statvfs(SD_MOUNT_POINT, &vfs) != 0) {
		printk("filesystem     : mounted at %s (size unavailable)\n",
		       SD_MOUNT_POINT);
		return 0;
	}

	{
		uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
		uint64_t freeb = (uint64_t)vfs.f_bfree * vfs.f_frsize;

		printk("filesystem     : mounted at %s\n", SD_MOUNT_POINT);
		printk("  volume total : ");
		sd_print_size(total);
		printk("\n");
		printk("  used         : ");
		sd_print_size(total - freeb);
		printk("\n");
		printk("  free         : ");
		sd_print_size(freeb);
		printk("\n");
		printk("  cluster size : %lu bytes\n", (unsigned long)vfs.f_frsize);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* sdtest                                                              */
/* ------------------------------------------------------------------ */

/*
 * Write a position-dependent pattern and read it back. The per-sector seed
 * makes a stale-but-identical block detectable, which constant filler would
 * not catch.
 */
static int cmd_sdtest(int argc, char **argv)
{
	uint32_t sectors = 0, ssize = 0;
	uint32_t test_sectors = 64;   /* 64 * 512 = 32 KiB by default */
	uint32_t per_chunk;
	int errors = 0;
	int ret;

	if (argc > 1) {
		test_sectors = (uint32_t)strtoul(argv[1], NULL, 0);
	}

	printk("=== SD read/write verification ===\n");

	if (sd_disk_open(&sectors, &ssize) != 0) {
		return -EIO;
	}

	per_chunk = SD_CHUNK_BYTES / ssize;

	if (test_sectors == 0 || test_sectors > sectors) {
		test_sectors = (sectors < 64U) ? sectors : 64U;
	}
	if (test_sectors > per_chunk) {
		test_sectors = per_chunk;
	}
	if (test_sectors == 0) {
		printk("sdtest: nothing to test\n");
		return -EINVAL;
	}

	printk("writing %u sectors (%u bytes) at sector 0...\n",
	       test_sectors, test_sectors * ssize);

	for (uint32_t s = 0; s < test_sectors; s++) {
		uint32_t *w = (uint32_t *)(sd_buf + s * ssize);
		uint32_t seed = 0xA5A50000U ^ s;

		for (uint32_t i = 0; i < ssize / 4U; i++) {
			w[i] = seed + i;
		}
	}

	ret = disk_access_write(SD_DISK_NAME, sd_buf, 0, test_sectors);
	if (ret != 0) {
		printk("sdtest: WRITE FAILED: %d\n", ret);
		return ret;
	}

	disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_SYNC, NULL);

	memset(sd_buf, 0, test_sectors * ssize);

	ret = disk_access_read(SD_DISK_NAME, sd_buf, 0, test_sectors);
	if (ret != 0) {
		printk("sdtest: READ FAILED: %d\n", ret);
		return ret;
	}

	for (uint32_t s = 0; s < test_sectors; s++) {
		uint32_t *w = (uint32_t *)(sd_buf + s * ssize);
		uint32_t seed = 0xA5A50000U ^ s;

		for (uint32_t i = 0; i < ssize / 4U; i++) {
			if (w[i] != seed + i) {
				printk("  mismatch at sector %u word %u: "
				       "got 0x%08X want 0x%08X\n",
				       s, i, w[i], seed + i);
				if (++errors > 8) {
					printk("  (further mismatches suppressed)\n");
					goto done;
				}
				break;
			}
		}
	}

done:
	if (errors == 0) {
		printk("sdtest: PASS - %u sectors written and read back intact\n",
		       test_sectors);
	} else {
		printk("sdtest: FAIL - %d sector(s) mismatched\n", errors);
	}

	printk("note: this overwrites the first %u sectors, so any filesystem "
	       "metadata there is now invalid\n", test_sectors);

	return errors ? -EIO : 0;
}

/* ------------------------------------------------------------------ */
/* sdclear - fast clear                                                */
/* ------------------------------------------------------------------ */

static void sd_remove_tree(const char *path)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	char child[SD_PATH_MAX];
	int ret;

	fs_dir_t_init(&dir);

	ret = fs_opendir(&dir, path);
	if (ret != 0) {
		return;
	}

	while (true) {
		ret = fs_readdir(&dir, &ent);
		if (ret != 0 || ent.name[0] == '\0') {
			break;
		}

		if (snprintf(child, sizeof(child), "%s/%s", path, ent.name)
		    >= (int)sizeof(child)) {
			continue;
		}

		if (ent.type == FS_DIR_ENTRY_DIR) {
			sd_remove_tree(child);
			fs_unlink(child);
		} else {
			fs_unlink(child);
		}
	}

	fs_closedir(&dir);
}

static int cmd_sdclear(int argc, char **argv)
{
	uint32_t sectors = 0, ssize = 0;
	bool do_format = (argc > 1) && (strcmp(argv[1], "format") == 0);
	int ret;

	printk("=== SD fast clear ===\n");

	if (sd_disk_open(&sectors, &ssize) != 0) {
		return -EIO;
	}

	ret = sd_ensure_mounted();

	if (ret != 0 && do_format) {
		/* Nothing mountable: build a fresh filesystem first. */
		printk("no mountable volume (%d); formatting...\n", ret);
		ret = fs_mkfs(FS_FATFS, (uintptr_t)SD_DISK_NAME, NULL, 0);
		if (ret != 0) {
			printk("sdclear: format failed: %d\n", ret);
			return ret;
		}
		ret = sd_ensure_mounted();
	}

	if (ret != 0) {
		printk("sdclear: cannot mount %s (%d)\n", SD_MOUNT_POINT, ret);
		printk("         run 'sdclear format' to create a filesystem\n");
		return ret;
	}

	printk("deleting files under %s ...\n", SD_MOUNT_POINT);
	sd_remove_tree(SD_MOUNT_POINT);
	printk("done\n");

	if (do_format) {
		printk("re-formatting...\n");

		/* Unmount (under the lock) before re-creating the volume, then
		 * let sd_ensure_mounted() bring it back.
		 */
		k_mutex_lock(&sd_lock, K_FOREVER);
		sd_unmount_locked();
		k_mutex_unlock(&sd_lock);

		ret = fs_mkfs(FS_FATFS, (uintptr_t)SD_DISK_NAME, NULL, 0);
		if (ret != 0) {
			printk("sdclear: format failed: %d\n", ret);
			return ret;
		}
		printk("filesystem recreated\n");

		/* The hotplug thread only acts on a presence *change*, so a
		 * card that stays seated would otherwise stay unmounted.
		 */
		if (sd_ensure_mounted() == 0) {
			printk("remounted at %s\n", SD_MOUNT_POINT);
		} else {
			printk("warning: remount failed\n");
		}
	}

	printk("note: entries are gone, but the old sector contents remain on the "
	       "card.\n      Use 'sdwipe confirm' to make them unrecoverable.\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* sdwipe - secure erase                                               */
/* ------------------------------------------------------------------ */

static int cmd_sdwipe(int argc, char **argv)
{
	uint32_t sectors = 0, ssize = 0;
	uint32_t per_chunk;
	uint32_t done = 0;
	int64_t t0, t1;
	int ret;

	printk("=== SD secure erase (overwrite with zeros) ===\n");

	if (argc < 2 || strcmp(argv[1], "confirm") != 0) {
		printk("this destroys all data on the card.\n");
		printk("run 'sdwipe confirm' to proceed - no changes made.\n");
		return 0;
	}

	if (sd_disk_open(&sectors, &ssize) != 0) {
		return -EIO;
	}

	per_chunk = SD_CHUNK_BYTES / ssize;

	printk("card: %u sectors x %u bytes = ", sectors, ssize);
	sd_print_size((uint64_t)sectors * ssize);
	printk("\noverwriting...\n");

	memset(sd_buf, 0, SD_CHUNK_BYTES);
	t0 = k_uptime_get();

	while (done < sectors) {
		uint32_t n = sectors - done;

		if (n > per_chunk) {
			n = per_chunk;
		}

		ret = disk_access_write(SD_DISK_NAME, sd_buf, done, n);
		if (ret != 0) {
			printk("\nsdwipe: write failed at sector %u: %d\n", done, ret);
			return ret;
		}

		done += n;

		if ((done % (per_chunk * 8U)) == 0 || done == sectors) {
			printk("\r  %u%% (%u/%u sectors)",
			       (uint32_t)(((uint64_t)done * 100U) / sectors),
			       done, sectors);
		}
	}

	disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_SYNC, NULL);

	t1 = k_uptime_get();
	printk("\n");

	{
		uint32_t ms = (uint32_t)(t1 - t0);
		uint64_t kib = ((uint64_t)sectors * ssize) >> 10;

		printk("overwrote %llu KiB in %u ms", (unsigned long long)kib, ms);
		if (ms > 0) {
			printk(" (~%llu KiB/s)",
			       (unsigned long long)((kib * 1000U) / ms));
		}
		printk("\n");
	}

	/* The FAT signatures are gone; leave a usable volume behind. */
	k_mutex_lock(&sd_lock, K_FOREVER);
	sd_unmount_locked();
	k_mutex_unlock(&sd_lock);

	printk("formatting to leave the card usable...\n");
	ret = fs_mkfs(FS_FATFS, (uintptr_t)SD_DISK_NAME, NULL, 0);
	if (ret != 0) {
		printk("format failed: %d (card is wiped but not formatted)\n", ret);
	} else {
		printk("card erased and re-formatted\n");

		/* Presence has not changed, so bring the volume back here. */
		if (sd_ensure_mounted() == 0) {
			printk("mounted at %s\n", SD_MOUNT_POINT);
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

/*
 * This sample runs shell commands in a child process, so a command has to be
 * registered twice:
 *
 *   1. in the sample's own command_registry, which is what shell_cmd_lookup()
 *      searches and what `ls` lists - that is the table below;
 *   2. as a Zephyr root command in main.c, whose handler is what the serial
 *      shell actually dispatches to.
 *
 * Registering only the Zephyr side (SHELL_CMD_REGISTER here) makes the command
 * appear in `ls` but fail at dispatch with "command not found", because the
 * fork's lookup never sees it.
 */
static const struct shell_cmd sd_cmd_info = {
	.name  = "sdinfo",
	.exec  = cmd_sdinfo,
	.brief = "Show SD card capacity and free space",
};

static const struct shell_cmd sd_cmd_test = {
	.name  = "sdtest",
	.exec  = cmd_sdtest,
	.brief = "SD write/read verification [sectors]",
};

static const struct shell_cmd sd_cmd_clear = {
	.name  = "sdclear",
	.exec  = cmd_sdclear,
	.brief = "Fast clear SD (add 'format' to re-create FAT)",
};

static const struct shell_cmd sd_cmd_wipe = {
	.name  = "sdwipe",
	.exec  = cmd_sdwipe,
	.brief = "Securely erase SD with zeros (needs 'confirm')",
};

static int register_sd_commands(void)
{
	shell_cmd_register(&sd_cmd_info);
	shell_cmd_register(&sd_cmd_test);
	shell_cmd_register(&sd_cmd_clear);
	shell_cmd_register(&sd_cmd_wipe);
	return 0;
}

/*
 * Priority 102: after the VFS registrations at 101, so `ls` lists the SD
 * commands last.
 */
SYS_INIT(register_sd_commands, APPLICATION, 102);
