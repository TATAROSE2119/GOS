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

#include "print.h"
#include "mm.h"
#include "dmac.h"
#include "string.h"

static int __axi_dma_test(char *name, void *src, void *dst, int size)
{
	int i, j, ret = 0, nn = 0;

	for (i = 0; i < size; i++) {
		for (j = 0; j < size; j++) {
			char *src_ptr = src + i;
			char *dst_ptr = dst + j;
			int n = 0;

			memset(src_ptr, 0x88, size);
			memset(dst_ptr, 0, size);

			print("%d ", nn++);
			ret = memcpy_hw(name, dst_ptr, src_ptr, size);
			if (ret == -1) {
				print("memcpy_hw failed, timeout...\n");
				return -1;
			}

			for (n = 0; n < size; n++) {
				if (src_ptr[n] != dst_ptr[n]) {
					print("dma_test failed!! src_ptr:0x%lx:0x%x dst_ptr:0x%lx\n:0x%x",
						&src_ptr[n], &dst_ptr[n], src_ptr[n], dst_ptr[n]);
					return -1;
				}
			}
		}
	}

	return 0;
}

static int __axi_dma_simple_test(char *name, int _size)
{
	int size = _size;
	void *dst, *src;

	src = mm_alloc(size * 2);
	if (!src) {
		print("%s -- alloc src failed!\n", __FUNCTION__);
		return -1;
	}
	dst = mm_alloc(size * 2);
	if (!dst) {
		print("%s -- alloc dst failed!\n", __FUNCTION__);
		return -1;
	}

	if (__axi_dma_test(name, src, dst, size))
		print("%s test fail\n", __FUNCTION__);
	else
		print("%s test pass!!\n", __FUNCTION__);

	mm_free(src, size * 2);
	mm_free(dst, size * 2);

	return 0;
}

int axi_dma_test_fix(char *name, int _size,
		     unsigned long fix_src, unsigned long fix_dst)
{
	int size = _size, ret;
	void *dst, *src;

	src = mm_alloc_fix(fix_src, size);
	if (!src) {
		print("%s -- alloc src failed!\n", __FUNCTION__);
		return -1;
	}
	dst = mm_alloc_fix(fix_dst, size);
	if (!dst) {
		print("%s -- alloc dst failed!\n", __FUNCTION__);
		return -1;
	}

	ret = memcpy_hw(name, dst, src, size);
	if (ret == -1) {
		print("memcpy_hw failed, timeout...\n");
		return -1;
	}

	mm_free(src, size);
	mm_free(dst, size);

	return 0;
}

int axi_dma_test_1bytes_handler(char *name)
{
	return __axi_dma_simple_test(name, 1);
}

int axi_dma_test_2bytes_handler(char *name)
{
	return __axi_dma_simple_test(name, 2);
}

int axi_dma_test_4bytes_handler(char *name)
{
	return __axi_dma_simple_test(name, 4);
}

int axi_dma_test_8bytes_handler(char *name)
{
	return __axi_dma_simple_test(name, 8);
}

int axi_dma_test_16bytes_handler(char *name)
{
	return __axi_dma_simple_test(name, 16);
}

int axi_dma_test_32bytes_handler(char *name)
{
	return __axi_dma_simple_test(name, 32);
}

int axi_dma_test_64bytes_handler(char *name)
{
	return __axi_dma_simple_test(name, 64);
}

int axi_dma_test_128bytes_handler(char *name)
{
	return __axi_dma_simple_test(name, 128);
}

int axi_dma_test_4Kbytes_handler(char *name)
{
	return __axi_dma_simple_test(name, 4096);
}

int axi_dma_test_8Kbytes_handler(char *name)
{
	return __axi_dma_simple_test(name, 8192);
}
