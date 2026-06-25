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
#include "asm/pgtable.h"
#include "mm.h"
#include "print.h"
#include "align.h"
#include "string.h"
#include "list.h"
#include "block.h"

#define MAX_BLOCK_LEN 0x200000

static LIST_HEAD(blk_devices);

static int __get_blk_buffer(struct blk_device *bdev, unsigned long where, int len)
{
	struct blk_buffer *new;
	int page_nr = N_PAGE(len);
	int _len = page_nr * PAGE_SIZE;

	if (!bdev || !bdev->ops)
		return -1;

	new = (struct blk_buffer *)mm_alloc(sizeof(struct blk_buffer));
	if (!new)
		return -1;

	new->pages = mm_alloc(_len);
	if (!new->pages)
		goto free1;

	if (bdev->ops->read(bdev, where, new->pages, _len))
		goto free2;

	new->page_nr = page_nr;
	new->where = where;
	list_add_tail(&new->list, &bdev->buffers);

	return 0;

free2:
	mm_free(new->pages, _len);
free1:
	mm_free((void *)new, sizeof(struct blk_buffer));

	return -1;
}

static int get_blk_buffer(struct blk_device *bdev, unsigned long where, int len)
{
	unsigned long addr = ALIGN_SIZE_UP(where, PAGE_SIZE);
	int remain = where - addr + len;

	while (remain > 0) {
		int _len;
		if (remain < MAX_BLOCK_LEN)
			_len = remain;
		else
			_len = MAX_BLOCK_LEN;

		if (__get_blk_buffer(bdev, addr, _len))
			return -1;

		addr += _len;
		remain -= _len;
	}

	return 0;
}

static struct blk_buffer *blk_find_buffer(struct blk_device *bdev, unsigned long where,
					  int len, int *actual_len)
{
	struct blk_buffer *buf;

	*actual_len = 0;
retry:
	list_for_each_entry(buf, &bdev->buffers, list) {
		int buf_len = buf->page_nr * PAGE_SIZE;
		if ((buf->where <= where) &&
		    ((buf->where +  buf_len) > where)) {
			if ((where + len) <= (buf->where + buf_len))
				*actual_len = len;
			else
				*actual_len = buf->where + buf_len - where;
			return buf;
		}
	}

	if (get_blk_buffer(bdev, where, len))
		return NULL;
	else
		goto retry;

	return NULL;
}

static void copy_buffer_from_bb(struct blk_buffer *b, char *to,
				unsigned long where, int len)
{
	int remain = len;
	unsigned long _from = (unsigned long )(b->pages + where - b->where);
	unsigned long _to = (unsigned long)to;

	while (remain) {
		unsigned long *long_from;
		unsigned long *long_to;

		if (remain < 8) {
			char *char_from = (char *)_from;
			char *char_to = (char *)_to;
			int i;

			for (i = 0; i < remain; i++)
				char_to[i] = char_from[i];

			return;
		}

		long_from = (unsigned long *)_from;
		long_to = (unsigned long *)_to;
		*long_to = *long_from;

		remain -= 8;
		_from += 8;
		_to += 8;
	}
}

void scan_all_blk(void (*cb)(struct blk_device *bdev))
{
	struct blk_device *bdev;

	list_for_each_entry(bdev, &blk_devices, list)
		cb(bdev);
}

void *blk_get_buffer(struct blk_device *bdev, unsigned long where,
		     int len, int *actual_len)
{
	struct blk_buffer *bb;

	bb = blk_find_buffer(bdev, where, len, actual_len);
	if (!bb)
		return NULL;

	return (void *)(bb->pages + (where - bb->where));
}

int blk_read(struct blk_device *bdev, unsigned long where,
	     char *buf, int len)
{
	struct blk_buffer *bb;
	int remain_len = len;
	unsigned long addr = where;
	char *tmp = buf;

	while (remain_len > 0) {
		int _len;
		bb = blk_find_buffer(bdev, addr, remain_len, &_len);
		if (!bb)
			return -1;
		copy_buffer_from_bb(bb, tmp, addr, _len);
		addr += _len;
		remain_len -= _len;
		tmp += _len;
	}

	return 0;
}

int blk_write(struct blk_device *bdev, unsigned long where,
	      char *buf, int len)
{
	if (!bdev || !bdev->ops)
		return -1;

	return bdev->ops->write(bdev, where, buf, len);
}

struct blk_device *get_blk_device(char *name)
{
	struct blk_device *bdev;

	list_for_each_entry(bdev, &blk_devices, list) {
		if (!strcmp(name, bdev->name))
			return bdev;
	}

	return NULL;
}

struct blk_device *blk_create_device(char *name, struct blk_ops *ops, void *priv)
{
	struct blk_device *bdev;

	bdev = (struct blk_device *)mm_alloc(sizeof(struct blk_device));
	if (!bdev) {
		print("%s: create blk device fail\n");
		return NULL;
	}
	memset((char *)bdev, 0, sizeof(struct blk_device));

	strcpy(bdev->name, name);
	bdev->ops = ops;
	bdev->priv = priv;
	INIT_LIST_HEAD(&bdev->buffers);
	list_add_tail(&bdev->list, &blk_devices);

	return bdev;
}
