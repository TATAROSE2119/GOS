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
#include "../command.h"
#include "cache_flush.h"
#include "string.h"
#include "mm.h"
#include "clock.h"
#include "asm/barrier.h"
#include "vmap.h"
#include "asm/pgtable.h"
#include "cpu.h"
#include "asm/sbi.h"
#include "irq.h"

#define RANGE (1 << 21)  // 2 MiB
#define ITERATIONS 1000000  // set iteration times to 1,000,000

static void Usage(void)
{
	print("cbo inval [addr]\n");
	print("cbo clean [addr]\n");
	print("cbo flush [addr]\n");
	print("cbo zero [addr]\n");
}

static int cmd_cbo_handler(int argc, char *argv[], void *priv)
{
	unsigned long base;
	unsigned long va;
	pgprot_t pgprot =
		__pgprot(_PAGE_BASE | _PAGE_READ | _PAGE_WRITE | _PAGE_DIRTY);

	if (argc != 2) {
		print("Invalid input params\n");
		Usage();
		return -1;
	}

	va = (unsigned long) vmap_alloc(PAGE_SIZE);
	if (!va) {
		print("%s -- alloc va failed\n", __FUNCTION__);
		return -1;
	}

	base = atoi(argv[1]);
	mmu_page_mapping(base, va, PAGE_SIZE, pgprot);

	if (!strncmp(argv[0], "inval", sizeof("inval"))) {
		cbo_cache_inval(va);
	} else if (!strncmp(argv[0], "clean", sizeof("clean"))) {
		cbo_cache_clean(va);
	} else if (!strncmp(argv[0], "flush", sizeof("flush"))) {
		cbo_cache_flush(va);
	} else if (!strncmp(argv[0], "zero", sizeof("zero"))) {
		cbo_cache_zero(va);
	}

	vmap_free((void *) va, PAGE_SIZE);

	return 0;
}

static int prefetch_w_test(void)
{
	static int test __attribute__((used));
	int *addr, *addr2;
	unsigned long start;
	unsigned long cost_pre, cost_unpre;
	unsigned long mcounteren;
	unsigned long new;

	mcounteren = sbi_get_cpu_mcounteren();
	new = mcounteren | (1UL << 0);
	sbi_set_mcounteren(new);

	addr = (int *) mm_alloc(4096);
	addr2 = (int *) mm_alloc(4096);

	disable_local_irq();

	prefetch_r((unsigned long) addr);

	start = read_csr(cycle);
	test = *addr;
	cost_pre = read_csr(cycle) - start;

	start = read_csr(cycle);
	test = *addr2;
	cost_unpre = read_csr(cycle) - start;

	enable_local_irq();

	print("cost_pre: %dcycles cosr_unpre: %dcycles\n", cost_pre, cost_unpre);

	sbi_set_mcounteren(mcounteren);
	mm_free((void *) addr, 4096);
	mm_free((void *) addr2, 4096);

	return 0;
}

static int cmd_prefetch_handler(int argc, char *argv[], void *priv)
{
	unsigned long base;

	if (argc < 1) {
		print("Invalid input params\n");
		Usage();
		return -1;
	}

	if (argc == 2)
		base = atoi(argv[1]);
	else
		return -1;

	if (!strncmp(argv[0], "i", sizeof("i"))) {
		prefetch_i(base);
		print("TEST PASS\n");
	} else if (!strncmp(argv[0], "w", sizeof("w"))) {
		prefetch_w(base);
		print("TEST PASS\n");
	} else if (!strncmp(argv[0], "r", sizeof("r"))) {
		prefetch_r(base);
		print("TEST PASS\n");
	} else if (!strncmp(argv[0], "test", sizeof("test"))) {
		prefetch_w_test();
		print("TEST PASS\n");
	}

	return 0;
}

static void cache_inval_test(void)
{
	void *addr;

	print("menvcfg: 0x%lx\n", sbi_get_csr_menvcfg());
	addr = mm_alloc(4096);
	if (!addr) {
		print("%s -- Out of memory!!\n", __FUNCTION__);
		return;
	}
	//set the content in memeory of addr to 0, make sure it won't impact by other cache test cases
	memset((char *)addr, 0, 4096);
	cbo_cache_flush((unsigned long) addr);
	//step1: write some content to cache
	strcpy((char *) addr, "This is content in cache");
	print("before cache inval\n");
	print("%s\n", (char *) addr);
	//step2: cache inval, cache block been deallocated
	cbo_cache_inval((unsigned long) addr);
	mb();

	//step3: expect the content not in memory
	print("after cache inval\n");
	print("%s\n", (char *) addr);
	if (strcmp(addr, "This is content in cache")) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}

	mm_free(addr, 4096);
}


static void cache_clean_test(void)
{
	void *addr;
	unsigned long start, before, after;
	unsigned long mcounteren, new;

	mcounteren = sbi_get_cpu_mcounteren();
	new = mcounteren | (1UL << 0);
	sbi_set_mcounteren(new);

	addr = mm_alloc(4096);
	if (!addr) {
		print("%s -- Out of memory!!\n", __FUNCTION__);
		return;
	}

	//step1: write some content to cache
	strcpy((char *) addr, "This is content in cache");

	start = read_csr(cycle);
	print("before clean:%s\n", (char *) addr);
	before = read_csr(cycle) - start;

	//step2: cache clean, content copy to memory, and the content in cache still available
	cbo_cache_clean((unsigned long) addr);
	mb();

	start = read_csr(cycle);
	print("after clean:%s\n", (char *) addr);
	after = read_csr(cycle) - start;
	print("before clean cost : %dticks\n", before);
	print("after clean cost : %dticks\n", after);

	//strcpy((char *) addr, "This is content in cache after clean");
	//print("new content after clean:%s\n", (char *) addr);

	//step3: cache inval, content should be in memory only (to avoid clean did nothing)
	cbo_cache_inval((unsigned long) addr);
	print("after inval:%s\n", (char *) addr);
	if (!strcmp(addr, "This is content in cache")) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}

	sbi_set_mcounteren(mcounteren);

	mm_free((void *) addr, 4096);
}

static void cache_flush_test(void)
{
	void *addr;
	unsigned long start, before, after;
	unsigned long mcounteren, new;

	mcounteren = sbi_get_cpu_mcounteren();
	new = mcounteren | (1UL << 0);
	sbi_set_mcounteren(new);

	addr = mm_alloc(4096);
	if (!addr) {
		print("%s -- Out of memory!!\n", __FUNCTION__);
		return;
	}

	strcpy((char *) addr, "This is content in cache");

	disable_local_irq();

	start = read_csr(cycle);
	print("%s\n", (char *) addr);
	before = read_csr(cycle) - start;

	cbo_cache_flush((unsigned long) addr);
	mb();

	start = read_csr(cycle);
	print("%s\n", (char *) addr);
	after = read_csr(cycle) - start;

	enable_local_irq();
	print("before flush cost : %dticks\n", before);
	print("after flush cost : %dticks\n", after);
	if (!strcmp(addr, "This is content in cache") && before < after) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}

	sbi_set_mcounteren(mcounteren);

	mm_free((void *) addr, 4096);
}

size_t step(size_t k)
{
	if (k < 1024) {
		k = k * 2;
	} else if (k < 4 * 1024) {
		k += 1024;
	} else {
		size_t s;

		for (s = 4 * 1024; s <= k; s *= 2);
		k += s / 4;
	}
	return (k);
}

// calculate the memory latency for different stride
void test_mem_latency(size_t stride, size_t range)
{
//	unsigned long start, after;
	unsigned long mcounteren, new;
//	unsigned long sum = 0;
	mcounteren = sbi_get_cpu_mcounteren();
	new = mcounteren | (1UL << 0);
	sbi_set_mcounteren(new);

	// allocate memory and initialize it as a pointer linked list
	char *mem = (char *) mm_alloc(range);
	if (!mem) {
		print("%s -- Out of memory!!\n", __FUNCTION__);
		return;
	}

	for (size_t i = stride; i < range; i += stride) {
		*(char **) (mem + i - stride) = mem + i;
	}
	*(char **) (mem + range - stride) = mem;  // loop list

	char **ptr = (char **)mem;
	int i = 0;
	int iteration = ITERATIONS;
	// run test
	unsigned int time_start = get_clocksource_counter_us();
	//print("start time in us:%d \n",time_start);
	for (i = 0; i < iteration; i++) {
		ptr = (char **) *ptr;

	}
	unsigned int time_end = get_clocksource_counter_us();
	//print("end time in us:%d \n",time_end);
	// calculate latency（us/op）
	double elapsed_time = (double) (time_end - time_start);
	double avg_latency = elapsed_time / ITERATIONS;

	print("range=%d bytes| stride=%d bytes | latency=%f us/op\n", range, stride, avg_latency);

	mm_free(mem,range);
	sbi_set_mcounteren(mcounteren);
}

static void lat_test(void)
{
	size_t strides[] = {16, 32, 64, 128, 256, 512, 1024};
	int num_strides = sizeof(strides) / sizeof(strides[0]);
	int lower = 512;

	print("Testing memory latency with %d MiB range, %d iterations:\n", RANGE/(1024*1024),ITERATIONS);
	for (int i = 0; i < num_strides; i++) {
		for (int j = lower; j <= RANGE; j = step(j))
			test_mem_latency(strides[i],j);
	}
}

static void cache_zero_test(void)
{
	void *addr;

	addr = mm_alloc(4096);
	if (!addr) {
		print("%s -- Out of memory!!\n", __FUNCTION__);
		return;
	}

	strcpy((char *) addr, "This is content in cache");
	print("before cache zero: \n");
	print("    %s\n", (char *) addr);

	cbo_cache_zero((unsigned long) addr);
	mb();

	print("after cache zero: \n");
	print("    %s\n", (char *) addr);
	if (!strcmp(addr, "")) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}

	mm_free(addr, 4096);
}

static int cmd_cache_flush_test_handler(int argc, char *argv[], void *priv)
{
	if (!strncmp(argv[0], "inval", sizeof("inval"))) {
		cache_inval_test();
	} else if (!strncmp(argv[0], "clean", sizeof("clean"))) {
		cache_clean_test();
	} else if (!strncmp(argv[0], "flush", sizeof("flush"))) {
		cache_flush_test();
	} else if (!strncmp(argv[0], "zero", sizeof("zero"))) {
		cache_zero_test();
	} else if (!strncmp(argv[0], "lat_test", sizeof("lat_test"))) {
		lat_test();
	}


	return 0;
}

static const struct command cmd_cbo_test = {
	.cmd = "cbo",
	.handler = cmd_cbo_handler,
	.priv = NULL,
};

static const struct command cmd_prefetch_test = {
	.cmd = "prefetch",
	.handler = cmd_prefetch_handler,
	.priv = NULL,
};

static const struct command cmd_cache_flush_test = {
	.cmd = "cache_flush_test",
	.handler = cmd_cache_flush_test_handler,
	.priv = NULL,
};

int cmd_cache_flush_test_init()
{
	register_command(&cmd_cbo_test);
	register_command(&cmd_prefetch_test);
	register_command(&cmd_cache_flush_test);

	return 0;
}

APP_COMMAND_REGISTER(cache_flush_test, cmd_cache_flush_test_init);
