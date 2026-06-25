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
#include "asm/barrier.h"
#include "task.h"
#include "mm.h"
#include "print.h"
#include "dmac.h"
#include "string.h"

struct consumer_producer_cond_t {
	int flag;
};

struct consumer_producer_buffer {
	char dmac[64];
	void *buffer;
	int size;
	char test_data;
	void *dmac_producer_addr;
	void *cpu_producer_addr;
	struct consumer_producer_cond_t dmac_producer_complete;
	struct consumer_producer_cond_t dmac_consumer_complete;
	struct consumer_producer_cond_t cpu_consumer_complete;
	struct consumer_producer_cond_t cpu_producer_complete;
};

static struct consumer_producer_buffer buffer = { 0 };

static void consumer_producer_cond_init(struct consumer_producer_cond_t *cond, int flag)
{
	cond->flag = flag;
}

static void consumer_producer_cond_wait(struct consumer_producer_cond_t *cond)
{
	unsigned int tmp = 1, swap = 0;

	while (1) {
		__asm__ __volatile__ (
			"amocas.w.aqrl %0, %2, (%1)\n"
			: "+r"(tmp)
			: "r"(&cond->flag), "r"(swap)
			: "memory"
		);
		if (tmp)
			break;
		tmp = 1;
	}
}

static void consumer_producer_cond_signal(struct consumer_producer_cond_t *cond)
{
	__smp_store_release(&cond->flag, 1);
	mb();
}

static int cpu_producer_thread(void *data)
{
	struct consumer_producer_buffer *buffer = (struct consumer_producer_buffer *)data;
	char *ptr;
	int nn = 0;

	while (1) {
		ptr = buffer->buffer + buffer->size * 2;
		for (int i = 0; i < buffer->size; i++) {
			consumer_producer_cond_wait(&buffer->dmac_consumer_complete);
			buffer->test_data = nn;
			buffer->cpu_producer_addr = ptr + i;
			print("cpu producer %d times -- addr:0x%lx test_data:0x%x\n", nn++, buffer->cpu_producer_addr, buffer->test_data);
			memset(buffer->cpu_producer_addr, buffer->test_data, buffer->size);
			consumer_producer_cond_signal(&buffer->cpu_producer_complete);
			schedule();
		}
	}

	return 0;
}

static int cpu_consumer_thread(void *data)
{
	int nn = 0;
	struct consumer_producer_buffer *buffer = (struct consumer_producer_buffer *)data;
	char *ptr;

	while (1) {
		consumer_producer_cond_wait(&buffer->dmac_producer_complete);
		ptr = buffer->dmac_producer_addr;
		for (int n = 0; n < buffer->size; n++) {
			if (ptr[n] != buffer->test_data) {
				print("%s test fail!!!\n", __FUNCTION__);
				return -1;
			}
		}
		print("cpu consumer %d times pass -- addr:0x%lx size:0x%lx\n", nn++, ptr, buffer->size);
		consumer_producer_cond_signal(&buffer->cpu_consumer_complete);
		schedule();
	}

	return 0;
}

static int dmac_producer_thread(void *data)
{
	struct consumer_producer_buffer *buffer = (struct consumer_producer_buffer *)data;
	void *src;
	int i, j, ret = 0, nn = 0;

	src = mm_alloc(buffer->size * 2);
	if (!src) {
		print("%s -- alloc src failed!\n", __FUNCTION__);
		return -1;
	}

	while (1) {
		for (i = 0; i < buffer->size; i++) {
			for (j = 0; j < buffer->size; j++) {
				char *src_ptr = buffer->cpu_producer_addr + j;
				char *dst_ptr = buffer->buffer + j;

				consumer_producer_cond_wait(&buffer->cpu_consumer_complete);
				consumer_producer_cond_wait(&buffer->cpu_producer_complete);

				src_ptr = src + i;
				dst_ptr = buffer->buffer + j;
				buffer->test_data = nn;

				memset(src_ptr, buffer->test_data, buffer->size);
				memset(dst_ptr, 0, buffer->size);

				print("dmac consumer %d times addr:0x%lx size:0x%lx\n", nn, src_ptr, buffer->size);
				print("dmac producer %d times addr:0x%lx size:0x%lx\n", nn, dst_ptr, buffer->size);

				nn++;
				ret = memcpy_hw(buffer->dmac, dst_ptr, src_ptr, buffer->size);
				if (ret == -1) {
					print("memcpy_hw failed, timeout...\n");
					return -1;
				}
				buffer->dmac_producer_addr = (void *)dst_ptr;
				consumer_producer_cond_signal(&buffer->dmac_producer_complete);
				consumer_producer_cond_signal(&buffer->dmac_consumer_complete);
				schedule();
			}
		}
	}

	mm_free(src, buffer->size * 2);

	return 0;
}

int axi_dma_test_consumer_producer(char *name, int size)
{
	buffer.size = size;
	buffer.buffer = mm_alloc(buffer.size * 4);
	if (!buffer.buffer) {
		print("%s alloc buffer fail...\n", __FUNCTION__);
		return -1;
	}
	buffer.test_data = 0;
	buffer.dmac_producer_addr = 0;
	strcpy(buffer.dmac, name);

	consumer_producer_cond_init(&buffer.dmac_producer_complete, 0);
	consumer_producer_cond_init(&buffer.dmac_consumer_complete, 1);
	consumer_producer_cond_init(&buffer.cpu_consumer_complete, 1);
	consumer_producer_cond_init(&buffer.cpu_producer_complete, 0);

	create_task("dma_test_dmac_producer", dmac_producer_thread, (void *)&buffer, 0, NULL, 0, NULL);
	create_task("dma_test_cpu_consumer", cpu_consumer_thread, (void *)&buffer, 0, NULL, 0, NULL);
	create_task("dma_test_cpu_producer", cpu_producer_thread, (void *)&buffer, 0, NULL, 0, NULL);

	return 0;
}
