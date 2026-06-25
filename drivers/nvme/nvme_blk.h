#ifndef __NVME_BLK_H__
#define __NVME_BLK_H__

#include "block.h"

enum {
	BLK_READ,
	BLK_WRITE,
};

struct nvme_partition {
	int id;
	struct blk_device *nvme_device;
	int sector_size;
	unsigned long start_sector;
	unsigned long sectors;
};

int nvme_parse_partition(struct blk_device *bdev, int section_size);
struct blk_device *nvme_blk_create_device(char *name, struct nvme_ns *ns);

#endif
