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
#include "string.h"
#include "../../../command.h"
#include "axi_dma_test.h"
#include "consumer_producer.h"

struct axi_dma_test_case {
	const char name[64];
	int (*test_case_handler)(char *name);
};

static struct axi_dma_test_case _case[] = {
	{"1bytes",      axi_dma_test_1bytes_handler },
	{"2bytes",      axi_dma_test_2bytes_handler },
	{"4bytes",      axi_dma_test_4bytes_handler },
	{"8bytes",      axi_dma_test_8bytes_handler },
	{"16bytes",     axi_dma_test_16bytes_handler },
	{"32bytes",     axi_dma_test_32bytes_handler },
	{"64bytes",     axi_dma_test_64bytes_handler },
	{"128bytes",    axi_dma_test_128bytes_handler },
	{"4Kbytes",     axi_dma_test_4Kbytes_handler },
	{"8Kbytes",     axi_dma_test_8Kbytes_handler },
};
#define TEST_CASE_COUNT (sizeof(_case) / sizeof(_case[0]))

static int cmd_axi_dma_test_handler(int argc, char *argv[], void *priv)
{
	int i;
	char name[64];

	if (argc < 2) {
		print("Invalid input params...\n");
		return -1;
	}

	strcpy(name, argv[0]);

	for (i = 0; i < TEST_CASE_COUNT; i++) {
		if (!strcmp(argv[1], _case[i].name))
			_case[i].test_case_handler(name);
	}

	return 0;
}

static int cmd_axi_dma_test_fix_handler(int argc, char *argv[], void *priv)
{
	int size;
	char name[64];
	unsigned long fix_src, fix_dst;

	if (argc < 4) {
		print("Invalid input params...\n");
		return -1;
	}

	strcpy(name, argv[0]);
	size = atoi(argv[1]);
	fix_src = atoi(argv[2]);
	fix_dst = atoi(argv[3]);

	axi_dma_test_fix(name, size, fix_src, fix_dst);

	return 0;
}

static int cmd_axi_dma_test_cp_handler(int argc, char *argv[], void *priv)
{
	char name[64];
	int size;

	if (argc < 2) {
		print("Invalid input params...\n");
		return -1;
	}

	strcpy(name, argv[0]);
	size = atoi(argv[1]);

	return axi_dma_test_consumer_producer(name, size);
}

static const struct command cmd_axi_dma_test = {
	.cmd = "axi_dma_test",
	.handler = cmd_axi_dma_test_handler,
	.priv = NULL,
};

static const struct command cmd_axi_dma_test_fix = {
	.cmd = "axi_dma_test_fix",
	.handler = cmd_axi_dma_test_fix_handler,
	.priv = NULL,
};

static const struct command cmd_axi_dma_test_consumer_producer = {
	.cmd = "axi_dma_test_cp",
	.handler = cmd_axi_dma_test_cp_handler,
	.priv = NULL,
};

int cmd_axi_dma_test_init()
{
	register_command(&cmd_axi_dma_test);
	register_command(&cmd_axi_dma_test_consumer_producer);
	register_command(&cmd_axi_dma_test_fix);

	return 0;
}

APP_COMMAND_REGISTER(axi_dma_test, cmd_axi_dma_test_init);
