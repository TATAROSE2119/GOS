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
#include "../command.h"
#include "asm/sbi.h"
#include "fs.h"

static void Usage(void)
{
	print("chroot [blk_device_name]\n");
}

static int cmd_chroot_handler(int argc, char *argv[], void *priv)
{
	if (argc == 0) {
		Usage();
		return -1;
	}

	change_init_fs(argv[0]);

	return 0;
}

static const struct command cmd_chroot = {
	.cmd = "chroot",
	.handler = cmd_chroot_handler,
	.priv = NULL,
};

int cmd_chroot_init()
{
	register_command(&cmd_chroot);

	return 0;
}

APP_COMMAND_REGISTER(chroot, cmd_chroot_init);
