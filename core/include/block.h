#ifndef __BLOCK_H__
#define __BLOCK_H__

#include "list.h"

struct blk_device;
struct blk_ops {
	int (*read)(struct blk_device *dev, unsigned long where, char *buf, int len);
	int (*write)(struct blk_device *dev, unsigned long where, char *buf, int len);
};

struct blk_buffer {
	struct list_head list;
	unsigned long where;
	void *pages;
	int page_nr;
};

struct blk_device {
	char name[64];
	struct list_head list;
	struct blk_ops *ops;
	void *priv;
	struct list_head buffers;
};

struct blk_ram_device {
	void *addr;
	int len;
};

void *blk_get_buffer(struct blk_device *bdev, unsigned long where,
		     int len, int *actual_len);
int blk_read(struct blk_device *bdev, unsigned long where,
	     char *buf, int len);
int blk_write(struct blk_device *bdev, unsigned long where,
	      char *buf, int len);
struct blk_device *get_blk_device(char *name);
void scan_all_blk(void (*cb)(struct blk_device *bdev));
struct blk_device *blk_create_device(char *name, struct blk_ops *ops, void *priv);
int blk_create_ram_device(char *name, void *addr, int len);

#endif
