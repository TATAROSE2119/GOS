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
#include <string.h>
#include <mm.h>
#include <asm/mmio.h>
#include "../command.h"
#include "generic/align.h"
#include "gos/cache_flush.h"

typedef unsigned long uint64_t;

extern char bss_end;
static unsigned long MEM_PHY_ADDR_START;
void pr_help()
{
	print("mem_range_check [Per Byte check(Will using Long time) 1:en 0:dis def:0] [{start size(Bytes)}] \n");
}

static unsigned long mem_range_check_scan(const unsigned long mem_sz_bytes,
								const unsigned long mem_sz_bits,
								const int per_bit_check)
{
	unsigned long checked_bits = 0, mem_at = 0;
	unsigned long mem_sz_q = mem_sz_bytes >> 3;
	unsigned long val, ret = 0;

	if (per_bit_check) {
		print("Per Byte checking, Take Long times\n");
		while (mem_at <= mem_sz_q) {
			val = readq((MEM_PHY_ADDR_START + mem_at));
			if ((MEM_PHY_ADDR_START + mem_at) != val) {
				print("get addr 0x%lx val 0x%lx ERROR\n", (MEM_PHY_ADDR_START + mem_at), val);
				return (MEM_PHY_ADDR_START + mem_at);
			}
			mem_at += 8;
		}
	} else {
		checked_bits = 3; // Start from 8 bytes
		print("Per Byte check Disable\n");
		while (mem_at <= mem_sz_bytes) {
			val = readq((MEM_PHY_ADDR_START + mem_at));
			if ((MEM_PHY_ADDR_START + mem_at) != val) {
				print("get addr 0x%lx val 0x%lx ERROR\n", (MEM_PHY_ADDR_START + mem_at), val);
				return (MEM_PHY_ADDR_START + mem_at);
			}
			mem_at = (1ULL << ++checked_bits);
		}
	}

	return ret;
}

static int cmd_mem_range_check_handler(int argc, char *argv[], void *priv)
{
	unsigned long ret;
	unsigned long mem_sz, mem_sz_q, mem_at = 0, mem_sz_bits, mem_sz_MB;
	unsigned int per_byte_check = 0;

	if (argc && !is_digit(argv[0])) {
		print("invalid input param.\n");
		pr_help();
		return -1;
	} else if (argc == 1)
		per_byte_check = atoi(argv[0]);

	if (argc == 3) {
		if (!is_digit(argv[1]) || !is_digit(argv[2])) {
			print("invalid input param for addr/size.\n");
			pr_help();
			return -1;
		}

		if (MEM_PHY_ADDR_START > atoi(argv[1])) {
			print("invalid input param for addr 0x%lx. should larger 0x%lx\n", atoi(argv[1]), MEM_PHY_ADDR_START);
			return -1;
		}
		MEM_PHY_ADDR_START = atoi(argv[1]);
		mem_sz = atoi(argv[2]);
		if (MEM_PHY_ADDR_START+ mem_sz > get_phy_end()) {
			print("!!!WARN size + start over than DTS size\n");
		}
	} else
		mem_sz = get_phy_end() - MEM_PHY_ADDR_START;

	mem_sz_MB = mem_sz >> 20; // Convert to MB
	mem_sz_q = mem_sz >> 3; // Convert to quadword size
	print("!!! test phy start 0x%lx test size %ldMB per_byte_check? %d\n",
						MEM_PHY_ADDR_START, mem_sz_MB, per_byte_check);

	mem_sz_bits = 0;
	mem_at = 0;
	if (per_byte_check) {
		print("Per Byte check Enable, Take Long times\n");
		while (mem_at <= mem_sz_q) {
			print("set val 0x%016lx to 0x%016lx\n", (MEM_PHY_ADDR_START + mem_at), MEM_PHY_ADDR_START + mem_at);
			writeq((MEM_PHY_ADDR_START + mem_at), MEM_PHY_ADDR_START + mem_at);
			mem_at += 8;
		}
	} else {
		mem_sz_bits = 3;
		print("Per Byte check Disable\n");
		while (mem_at <= mem_sz) {
			print("set val 0x%016lx to 0x%016lx\n", (MEM_PHY_ADDR_START + mem_at), MEM_PHY_ADDR_START + mem_at);
			writeq((MEM_PHY_ADDR_START + mem_at), MEM_PHY_ADDR_START + mem_at);
			mb();
			cbo_cache_flush((MEM_PHY_ADDR_START + mem_at));
			mem_at = (1ULL << ++mem_sz_bits);
		}
	}

	ret = mem_range_check_scan(mem_sz, mem_sz_bits, per_byte_check);
	if (ret != 0) {
		print("Memory Range Check Failed at 0x%lx\n", ret);
		return TEST_FAIL;
	}

	print("Memory Range Check Done\n");
	return TEST_PASS;
}

static const struct command cmd_mem_range_check = {
	.cmd = "mem_range",
	.handler = cmd_mem_range_check_handler,
	.priv = NULL,
};

int cmd_mem_range_check_init()
{
	MEM_PHY_ADDR_START = ALIGN_SIZE((unsigned long)(&bss_end), 0x1000000);
	register_command(&cmd_mem_range_check);

	return 0;
}

APP_COMMAND_REGISTER(mem_range_check, cmd_mem_range_check_init);
