/*
 * Copyright (c) 2024 Beijing Institute of Open Source Chip (BOSC)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "asm/type.h"
#include "print.h"
#include "device.h"
#include "mm.h"
#include "string.h"
#include "../command.h"
#include "block.h"
#include "fs.h"

static void Usage(void)
{
	print("mount [dev_name]\n");
}

static int cmd_mount_handler(int argc, char *argv[], void *priv)
{
	char *name;
	struct blk_device *bdev;

	if (argc < 1) {
		Usage();
		return -1;
	}

	name = argv[0];

	bdev = get_blk_device(name);
	if (!bdev) {
		print("get blk device(%s) fail\n", name);
		return -1;
	}

	load_fs(FS_TYPE_EXT4, bdev);

	return 0;
}

static const struct command cmd_mount = {
	.cmd = "mount",
	.handler = cmd_mount_handler,
	.priv = NULL,
};

int cmd_mount_init()
{
	register_command(&cmd_mount);

	return 0;
}

APP_COMMAND_REGISTER(mount, cmd_mount_init);
