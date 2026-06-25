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

#include "mm.h"
#include "string.h"
#include "print.h"
#include "block.h"

static int blk_ram_read(struct blk_device *dev, unsigned long where,
			char *buf, int len)
{
	struct blk_ram_device *brd = (struct blk_ram_device *)dev->priv;
	void *base = brd->addr;
	int l = brd->len;

	if ((where + len) > l) {
		print("%s fail, Invalid params... (addr:0x%lx len:0x%lx l:0x%lx)\n", __FUNCTION__, where, len, l);
		return -1;
	}
	memcpy((char *)buf, (char *)(base + where), len);

	return 0;
}

static int blk_ram_write(struct blk_device *dev, unsigned long where,
			 char *buf, int len)
{
	return 0;
}

static struct blk_ops blk_ram_device_ops = {
	.read = blk_ram_read,
	.write = blk_ram_write,
};

int blk_create_ram_device(char *name, void *addr, int len)
{
	struct blk_ram_device *brd;

	brd = (struct blk_ram_device *)mm_alloc(sizeof(struct blk_ram_device));
	if (!brd) {
		print("%s Out of memory\n", __FUNCTION__);
		return -1;
	}

	brd->addr = addr;
	brd->len = len;

	if (!blk_create_device(name, &blk_ram_device_ops, (void *)brd))
		return -1;

	return 0;
}
