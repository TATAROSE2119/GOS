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

#include "align.h"
#include "string.h"
#include "print.h"
#include "mm.h"
#include "block.h"
#include "nvme.h"
#include "fs.h"
#include "nvme_blk.h"

static int nvme_blk_read(struct blk_device *dev, unsigned long where,
			 char *buf, int len)
{
	struct nvme_ns *ns = (struct nvme_ns *)dev->priv;
	struct nvme_device *ndev;
	int blknr, blkcnt;
	unsigned long start, end;

	if (!ns)
		return -1;

	ndev = ns->ndev;
	if (!ndev)
		return -1;

	start = ALIGN_SIZE_UP(where, 1 << ns->lba_shift);
	end = ALIGN_SIZE(where + len, 1 << ns->lba_shift);

	blknr = start >> ns->lba_shift;
	blkcnt = (end - start) >> ns->lba_shift;

	return nvme_blk_rw(ndev, ns, blknr, blkcnt, buf, BLK_READ);
}

static int nvme_blk_write(struct blk_device *dev, unsigned long where,
			  char *buf, int len)
{
	return 0;
}

static struct blk_ops nvme_blk_ops = {
	.read = nvme_blk_read,
	.write = nvme_blk_write,
};

struct blk_device *nvme_blk_create_device(char *name, struct nvme_ns *ns)
{
	return blk_create_device(name, &nvme_blk_ops, (void *)ns);
}

static int nvme_partition_blk_read(struct blk_device *dev, unsigned long where,
				   char *buf, int len)
{
	struct nvme_partition *p = (struct nvme_partition *)dev->priv;
	struct blk_device *nvme_blk_dev;
	unsigned long addr;

	if (!p)
		return -1;

	nvme_blk_dev = p->nvme_device;

	addr = p->start_sector * p->sector_size + where;

	return nvme_blk_read(nvme_blk_dev, addr, buf, len);
}

static int nvme_partition_blk_write(struct blk_device *dev, unsigned long where,
				    char *buf, int len)
{
	return 0;
}

static struct blk_ops nvme_partition_blk_ops = {
	.read = nvme_partition_blk_read,
	.write = nvme_partition_blk_write,
};

static void nvme_create_partition_device(unsigned long sec_offset, int sec_count,
					 int partition_id, void *priv)
{
	struct nvme_partition *partition;
	struct blk_device *bdev = (struct blk_device *)priv;
	char name[128];

	if (!bdev)
		return;

	partition = (struct nvme_partition *)mm_alloc(sizeof(struct nvme_partition));
	if (!partition) {
		print("nvme_blk: Out of memory\n");
		return;
	}
	partition->nvme_device = bdev;
	partition->id = partition_id;
	partition->sector_size = 512;
	partition->start_sector = sec_offset;
	partition->sectors = sec_count;

	sprintf(name, "%sp%d", bdev->name, partition_id);
	print("  %s\n", name);
	blk_create_device(name, &nvme_partition_blk_ops, (void *)partition);
}

int nvme_parse_partition(struct blk_device *bdev, int section_size)
{
	return fs_parse_partition(bdev, section_size,
				  nvme_create_partition_device,
				  (void *)bdev);
}
