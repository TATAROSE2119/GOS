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

#include "irq.h"
#include "device.h"
#include "string.h"
#include "print.h"
#include "asm/type.h"
#include "asm/mmio.h"
#include "asm/sbi.h"
#include "vmap.h"
#include "asm/pgtable.h"
#include "list.h"
#include "mm.h"

#define IMSIC_TEST_SET_COUNT 0
#define IMSIC_TEST_TRIGGER 1
#define IMSIC_TEST_SET_AFFINITY 2

extern int mmu_is_on;

struct imsic_test_ioctl_set_count {
	int count;
	int ret_count;
	int hwirq_base;
};

struct imsic_test_ioctl_trigger {
	int hwirq;
};

struct imsic_test_ioctl_set_affinity {
	int hwirq;
	int cpu;
};

union imsic_test_ioctl {
	struct imsic_test_ioctl_set_count set_count;
	struct imsic_test_ioctl_trigger trigger;
	struct imsic_test_ioctl_set_affinity affinity;
};

struct msi_msg {
	struct list_head list;
	int hwirq;
	unsigned long msi_addr;
	unsigned long msi_data;
};

struct imsic_test_struct {
	struct device *dev;
	struct list_head msis;
};

static struct imsic_test_struct imsic_test = { 0 };

static void imsic_test_irq_handler(void *data)
{
	struct msi_msg *msi = (struct msi_msg *)data;

	print("imsic test driver: ###### enter %s (hwirq_%d cpu%d)\n", __FUNCTION__, msi->hwirq, sbi_get_cpu_id());
}

static void imsic_test_write_msi_msg(struct device *dev, unsigned long msi_addr,
				     unsigned long msi_data, int hwirq,
				     void *priv)
{
	struct msi_msg *msi;

	list_for_each_entry(msi, &imsic_test.msis, list) {
		if (msi->hwirq == hwirq)
			goto update_msi_msg;
	}

	msi = (struct msi_msg *)mm_alloc(sizeof(struct msi_msg));
	if (!msi) {
		print("%s -- Out of memory\n", __FUNCTION__);
		return;
	}
	memset((char *)msi, 0, sizeof(struct msi_msg));
	list_add_tail(&msi->list, &imsic_test.msis);

update_msi_msg:
	msi->hwirq = hwirq;
	msi->msi_addr = msi_addr;
	msi->msi_data = msi_data;
}

static struct msi_msg *get_msi_msg(int hwirq)
{
	struct msi_msg *msi;

	list_for_each_entry(msi, &imsic_test.msis, list) {
		if (msi->hwirq == hwirq)
			return msi;
	}

	return NULL;
}

static int imsic_test_set_count(struct imsic_test_ioctl_set_count *info)
{
	int irq, i;
	struct msi_msg *msi;
	int count = info->count;
	struct device *dev = imsic_test.dev;
	int hwirq;

	if (!dev)
		return -1;

	irq = msi_get_irq(dev, count, imsic_test_write_msi_msg, NULL, &imsic_test);
	if (irq == -1) {
		print("imsic test driver: imsic test driver: msi_get_hwirq failed\n", __FUNCTION__);
		return -1;
	}
	hwirq = get_device_hwirq(imsic_test.dev, irq);

	for (i = 0; i < count; i++) {
		msi = get_msi_msg(hwirq + i);
		if (!msi) {
			print("warning: imsic_test -- can not get msi msg (%d)\n", irq + i);
			continue;
		}

		register_device_irq(dev, irq + i,
				    imsic_test_irq_handler, msi);
	}

	info->ret_count = count;
	info->hwirq_base = hwirq;

	return 0;
}

static void imsic_test_trigger(struct imsic_test_ioctl_trigger *trigger)
{
	int hwirq = trigger->hwirq;
	struct msi_msg *msi;
	void *addr;

	msi = get_msi_msg(hwirq);
	if (!msi)
		return;

	addr = ioremap((void *)msi->msi_addr, 4096, NULL);
	if (!addr)
		return;

	//print("imsic test driver: imsic test driver: write 0x%lx to 0x%lx\n",
	//      msi->msi_data, msi->msi_addr);
	writel(addr, msi->msi_data);

	iounmap(addr, 4096);
}

static int imsic_test_set_affinity(struct imsic_test_ioctl_set_affinity *affinity)
{
	int hwirq = affinity->hwirq;
	int cpu = affinity->cpu;
	struct msi_msg *msi;

	msi = get_msi_msg(hwirq);
	if (!msi)
		return -1;

	return irq_domain_set_affinity(imsic_test.dev, NULL, hwirq, cpu);
}

static int imsic_test_ioctl(struct device *dev, unsigned int cmd, void *arg)
{
	union imsic_test_ioctl *info = (union imsic_test_ioctl *)arg;
	int ret = 0;

	if (!info)
		return -1;

	switch (cmd) {
	case IMSIC_TEST_SET_COUNT:
		struct imsic_test_ioctl_set_count *set_count = &info->set_count;
		ret = imsic_test_set_count(set_count);
		break;
	case IMSIC_TEST_TRIGGER:
		struct imsic_test_ioctl_trigger *trigger = &info->trigger;
		imsic_test_trigger(trigger);
		break;
	case IMSIC_TEST_SET_AFFINITY:
		struct imsic_test_ioctl_set_affinity *affinity = &info->affinity;
		ret = imsic_test_set_affinity(affinity);
		break;
	default:
		return -1;
	}

	return ret;
}

static const struct driver_ops imsic_test_ops = {
	.ioctl = imsic_test_ioctl,
};

int imsic_test_init(struct device *dev, void *data)
{
	struct driver *drv;

	INIT_LIST_HEAD(&imsic_test.msis);
	imsic_test.dev = dev;

	drv = dev->drv;
	strcpy(dev->name, "IMSIC_TEST");
	strcpy(drv->name, "IMSIC_TEST");
	drv->ops = &imsic_test_ops;

	return 0;
}

DRIVER_REGISTER(imsic_test, imsic_test_init, "imsic,test");
