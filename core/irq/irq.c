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
#include "asm/sbi.h"
#include "device.h"
#include "string.h"
#include "trap.h"
#include "print.h"
#include "irq.h"
#include "timer.h"
#include "mm.h"
#include "vmap.h"
#include "irq.h"
#include "iommu.h"
#include "dma-mapping.h"
#include "spinlocks.h"

static LIST_HEAD(irq_domains);

static struct irq_domain intc_domain;

#define MAX_IRQ_COUNT 1024
static struct irq_info irq_info_desc[MAX_IRQ_COUNT] = { 0 };
static DEFINE_SPINLOCK(irq_info_lock);

static int get_free_irq(void)
{
	int i, flags;
	struct irq_info *info;

	spin_lock_irqsave(&irq_info_lock, flags);
	for (i = 0; i < MAX_IRQ_COUNT; i++) {
		info = &irq_info_desc[i];
		if (!info->in_used) {
			info->in_used = 1;
			info->irq = i;
			spin_unlock_irqrestore(&irq_info_lock, flags);
			return i;
		}
	}
	spin_unlock_irqrestore(&irq_info_lock, flags);

	return -1;
}

static int domain_insert_irq_info(struct irq_domain *domain,
				  struct irq_info *info,
				  int hwirq)
{
	int flags;
	struct domain_irq_info *irq_info, *tmp;

	tmp = (struct domain_irq_info *)mm_alloc(sizeof(struct domain_irq_info));
	if (!tmp)
		return -1;

	spin_lock_irqsave(&domain->lock, flags);

	list_for_each_entry(irq_info, &domain->irq_info_head, list) {
		if (irq_info->hwirq == hwirq) {
			spin_unlock_irqrestore(&domain->lock, flags);
			mm_free((void *)tmp, sizeof(struct domain_irq_info));
			return 0;
		}
	}
	irq_info = tmp;
	irq_info->hwirq = hwirq;
	irq_info->info = info;
	list_add_tail(&irq_info->list, &domain->irq_info_head);

	spin_unlock_irqrestore(&domain->lock, flags);

	return 0;
}

static int domain_activate_irq(struct device *dev,
			       struct irq_domain *domain,
			       int hwirq)
{
	if (!domain || !domain->domain_ops)
		return -1;

	if (domain->domain_ops->activate_irq)
		domain->domain_ops->activate_irq(dev, domain, hwirq);

	if (domain->domain_ops->set_type)
		domain->domain_ops->set_type(dev, hwirq,
					     IRQ_TYPE_LEVEL_HIGH,
					     domain->priv);

	if (domain->domain_ops->unmask_irq)
		domain->domain_ops->unmask_irq(dev, hwirq, domain->priv);

	irq_domain_set_affinity(dev, domain, hwirq, 0);

	return 0;
}

static int alloc_irqs(struct device *dev, struct irq_domain *domain,
		      struct irq_info *info, int irq)
{
	int hwirq;

	if (!domain)
		return -1;

	if (domain->domain_ops && domain->domain_ops->alloc_irqs) {
		hwirq = domain->domain_ops->alloc_irqs(dev, 1, domain->priv);
		if (hwirq == -1)
			return 0;
	}
	else {
		hwirq = irq;
	}

	if (domain->link_domain)
		alloc_irqs(dev, domain->link_domain, info, hwirq);


	if (domain_insert_irq_info(domain, info, hwirq))
		return -1;

	if (info->set_msi_desc)
		info->set_msi_desc(dev, hwirq, 1);

	domain_activate_irq(dev, domain, hwirq);

	return hwirq;
}

int get_domain_hwirq(struct irq_domain *domain, struct irq_info *info)
{
	int flags;
	struct domain_irq_info *irq_info;

	spin_lock_irqsave(&domain->lock, flags);
	list_for_each_entry(irq_info, &domain->irq_info_head, list) {
		if (irq_info->info == info) {
			spin_unlock_irqrestore(&domain->lock, flags);
			return irq_info->hwirq;
		}
	}
	spin_unlock_irqrestore(&domain->lock, flags);

	return -1;
}

struct irq_info *get_irq_info(struct irq_domain *domain, int hwirq)
{
	int flags;
	struct domain_irq_info *irq_info;

	spin_lock_irqsave(&domain->lock, flags);
	list_for_each_entry(irq_info, &domain->irq_info_head, list) {
		if (irq_info->hwirq == hwirq) {
			spin_unlock_irqrestore(&domain->lock, flags);
			return irq_info->info;
		}
	}
	spin_unlock_irqrestore(&domain->lock, flags);

	return NULL;
}

int get_irq(struct device *dev, int *ret_irq)
{
	int irq, num, i;
	int *irqs = dev->irqs;
	struct irq_info *info;
	struct irq_domain *domain = dev->irq_domain;

	if (!domain)
		return -1;

	if (dev->irq_num > MAX_IRQ_NUM)
		num = MAX_IRQ_NUM;
	else
		num = dev->irq_num;

	if (num == 0)
		return 0;

	if (!irqs)
		return -1;

	for (i = 0; i < num; i++) {
		irq = get_free_irq();
		if (-1 == irq)
			return -1;

		info = &irq_info_desc[irq];
		alloc_irqs(dev, domain, info, irqs[i]);
		ret_irq[i] = irq;
	}

	return num;
}

int msi_get_irq(struct device *dev, int nr,
		write_msi_msg_t write_msi_msg,
		void (*set_msi_desc)(struct device *dev, int hwirq, int nr),
		void *priv)
{
	int i, hwirq, irq;
	struct irq_info *info;
	struct irq_domain *domain = dev->irq_domain;

	if (!domain)
		return 0;

	for (i = 0; i < nr; i++) {
		unsigned long msi_addr = 0, msi_data = 0;

		irq = get_free_irq();
		if (-1 == irq)
			return -1;

		info = &irq_info_desc[irq];
		info->write_msi_msg = write_msi_msg;
		info->set_msi_desc = set_msi_desc;
		info->msi_priv = priv;
		hwirq = alloc_irqs(dev, domain, info, -1);
		if (hwirq == -1)
			return -1;

		if (domain->domain_ops && domain->domain_ops->get_msi_msg) {
			domain->domain_ops->get_msi_msg(dev, domain, hwirq,
							&msi_addr, &msi_data,
							domain->priv);
			if (write_msi_msg)
				write_msi_msg(dev, msi_addr, msi_data, hwirq, priv);
		}
	}

	return irq - nr + 1;
}

int get_link_domain_hwirq(struct irq_domain *domain, int hwirq)
{
	struct irq_domain *link_domain;
	struct irq_info *info;

	link_domain = domain->link_domain;
	if (!link_domain)
		return -1;

	info = get_irq_info(domain, hwirq);
	if (!info)
		return -1;

	return get_domain_hwirq(link_domain, info);
}

int get_device_hwirq(struct device *dev, int irq)
{
	struct irq_domain *domain = dev->irq_domain;
	struct irq_info *info;

	if (irq > MAX_IRQ_COUNT)
		return -1;

	if (!domain)
		return -1;

	info = &irq_info_desc[irq];
	if (!info)
		return -1;

	return get_domain_hwirq(domain, info);
}

int irq_domain_set_affinity(struct device *dev,
			    struct irq_domain *domain,
			    int hwirq, int cpu)
{
	int link_hwirq;
	struct irq_domain *irq_domain;

	if (!domain) {
		if (!dev)
			return -1;
		irq_domain = dev->irq_domain;
	}
	else
		irq_domain = domain;

	if (irq_domain->link_domain) {
		link_hwirq = get_link_domain_hwirq(irq_domain, hwirq);
		irq_domain_set_affinity(dev, irq_domain->link_domain, link_hwirq, cpu);
	}

	if (!irq_domain->domain_ops)
		return -1;

	if (irq_domain->domain_ops->set_affinity)
		irq_domain->domain_ops->set_affinity(dev, irq_domain, hwirq, cpu);

	if (irq_domain->domain_ops->get_msi_msg) {
		unsigned long msi_addr = 0, msi_data = 0;
		struct irq_info *irq_info;

		irq_domain->domain_ops->get_msi_msg(dev, irq_domain, hwirq,
						    &msi_addr, &msi_data,
						    irq_domain->priv);
		if (dev && dev->iommu) {
			if (dma_mapping(dev, msi_addr, &msi_addr, PAGE_SIZE, NULL)) {
				print("warning -- Map msi addr failed, irq:%d msi_addr:0x%lx dev:%s\n",
				      hwirq, msi_addr, dev->compatible);
				return -1;
			}
		}

		irq_info = get_irq_info(irq_domain, hwirq);
		if (irq_info && irq_info->write_msi_msg)
			irq_info->write_msi_msg(dev, msi_addr, msi_data, hwirq, irq_info->msi_priv);
	}

	return 0;
}

int domain_register_irq(struct device *dev, struct irq_domain *domain,
			int hwirq, void (*handler)(void *data),
			void *priv)
{
	int irq;
	struct irq_info *irq_info;

	irq = get_free_irq();
	if (-1 == irq)
		return -1;
	irq_info = &irq_info_desc[irq];

	if (domain_insert_irq_info(domain, irq_info, hwirq))
		return -1;

	return register_device_irq(dev, irq, handler, priv);
}

int register_device_irq(struct device *dev, unsigned int irq,
			void (*handler)(void *data), void *priv)
{
	struct irq_info *irq_info;

	if (irq > MAX_IRQ_COUNT)
		return -1;

	irq_info = &irq_info_desc[irq];
	irq_info->handler = handler;
	irq_info->priv = priv;

	return 0;
}

void handle_irq(unsigned long cause)
{
	struct irq_domain *d = find_irq_domain("INTC");
	struct irq_info *irq_info;

	if (!d) {
		print("unsupported cause: %d\n", cause & (~SCAUSE_IRQ));
		while (1) ;
		return;
	}

	irq_info = get_irq_info(d, cause & (~SCAUSE_IRQ));
	if (!irq_info || !irq_info->handler)
		return;

	irq_info->handler(irq_info->priv);
}

int domain_handle_irq(struct irq_domain *domain, unsigned int hwirq, void *data)
{
	struct irq_info *irq_info;

	irq_info = get_irq_info(domain, hwirq);
	if (!irq_info)
		return -1;

	irq_info->handler(irq_info->priv);

	return 0;
}

int irqchip_setup(struct device_init_entry *hw)
{
	extern unsigned long IRQCHIP_INIT_TABLE, IRQCHIP_INIT_TABLE_END;
	int driver_nr =
	    (struct irqchip_init_entry *)&IRQCHIP_INIT_TABLE_END -
	    (struct irqchip_init_entry *)&IRQCHIP_INIT_TABLE;
	int driver_nr_tmp = 0;
	struct irqchip_init_entry *driver_entry;
	struct device_init_entry *device_entry = hw;
	struct irqchip_init_entry *driver_tmp =
	    (struct irqchip_init_entry *)&IRQCHIP_INIT_TABLE;
	struct irq_domain *d;

	while (strncmp(device_entry->compatible, "THE END", sizeof("THE_END"))) {
		driver_nr_tmp = driver_nr;
		for (driver_entry = driver_tmp; driver_nr_tmp;
		     driver_entry++, driver_nr_tmp--) {
			d = find_irq_domain(device_entry->irq_parent);
			if (!strncmp
			    (driver_entry->compatible, device_entry->compatible,
			     128)) {
				driver_entry->init(device_entry->compatible,
						   device_entry->start,
						   device_entry->len, d,
						   device_entry->data);
			}
		}
		device_entry++;
	}

	return 0;
}

struct irq_domain *find_irq_domain(char *name)
{
	struct irq_domain *domain;

	list_for_each_entry(domain, &irq_domains, list)
	    if (!strncmp(domain->name, name, 128))
		return domain;

	return NULL;
}

int irq_domain_init(struct irq_domain *domain,
		    char *name, struct irq_domain_ops *ops,
		    struct irq_domain *parent, void *priv)
{
	domain->parent_domain = parent;
	strcpy(domain->name, name);
	INIT_LIST_HEAD(&domain->irq_info_head);
	list_add(&domain->list, &irq_domains);
	domain->priv = priv;
	domain->domain_ops = ops;
	__SPINLOCK_INIT(&domain->lock);

	return 0;
}

int irq_domain_init_hierarchy(struct irq_domain *domain, char *name,
			      struct irq_domain_ops *ops,
			      struct irq_domain *base_domain,
			      void *priv)
{
	memset((char *)domain, 0, sizeof(struct irq_domain));
	domain->link_domain = base_domain;

	return irq_domain_init(domain, name, ops, base_domain, priv);
}

int irq_domain_init_cascade(struct irq_domain *domain, char *name,
			    struct irq_domain_ops *ops,
			    struct irq_domain *parent, unsigned int hwirq,
			    void (*handler)(void *data), void *priv)
{
	memset((char *)domain, 0, sizeof(struct irq_domain));
	domain_register_irq(NULL, parent, hwirq, handler, priv);

	return irq_domain_init(domain, name, ops, parent, priv);
}

struct irq_domain *get_intc_domain(void)
{
	return &intc_domain;
}

int irq_init(void)
{
	memset((char *)&intc_domain, 0, sizeof(struct irq_domain));

	irq_domain_init(&intc_domain, "INTC", NULL, NULL, NULL);

	return 0;
}
