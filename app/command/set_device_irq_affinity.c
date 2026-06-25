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

#include <asm/type.h>
#include <print.h>
#include <device.h>
#include "irq.h"
#include "string.h"
#include "../command.h"

static void Usage()
{
	print("set_device_irq_affinity [device_name] [cpu_id]\n");
}

static int cmd_set_device_irq_affinity_handler(int argc, char *argv[], void *priv)
{
	struct device *dev;
	int cpu, i;

	if (argc != 2) {
		print("Invalid input params.\n");
		Usage();
		return -1;
	}

	dev = get_device(argv[0]);
	if (!dev)
		return -1;

	cpu = atoi(argv[1]);

	for (i = 0; i < dev->irq_num; i++)
		irq_domain_set_affinity(dev, NULL, dev->irqs[i], cpu);

	return 0;
}

static const struct command cmd_set_device_irq_affinity = {
	.cmd = "set_device_irq_affinity",
	.handler = cmd_set_device_irq_affinity_handler,
	.priv = NULL,
};

int set_device_irq_affinity_init()
{
	register_command(&cmd_set_device_irq_affinity);

	return 0;
}

APP_COMMAND_REGISTER(set_device_irq_affinity, set_device_irq_affinity_init);
