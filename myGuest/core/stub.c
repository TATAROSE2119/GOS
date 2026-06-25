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
#include "asm/pgtable.h"
#include "asm/ptregs.h"
#include "list.h"
#include "mm.h"
#include "string.h"
#include "irq.h"
#include "stub.h"

static LIST_HEAD(remote_stub_list);

typedef void (*stub_handler_t)(struct pt_regs *regs, struct virt_pt_regs *vregs);

static int remote_stub_irq_handler(void *data)
{
	stub_handler_t stub_handler;
	struct remote_stub_info *info = (struct remote_stub_info *)data;
	struct pt_regs *arg;
	struct virt_pt_regs *arg2;

	if (!info)
		return -1;

	stub_handler = (stub_handler_t)info->handler;
	if (!stub_handler)
		return -1;

	arg = (struct pt_regs *)phy_to_virt(info->pt_regs_pa);
	if (!arg)
		return -1;

	arg2 = (struct virt_pt_regs *)phy_to_virt(info->virt_pt_regs_pa);
	if (!arg2)
		return -1;

	stub_handler(arg, arg2);

	return 0;
}

int register_remote_stub(const char *name, void (*handler)(struct pt_regs *regs, struct virt_pt_regs *vregs))
{
	int hwirq;
	struct remote_stub_info *info;
	struct remote_stub *stub;
	struct pt_regs *r;
	struct virt_pt_regs *virt_r;
	unsigned long msi_addr = -1, msi_data = -1;

	list_for_each_entry(stub, &remote_stub_list, list) {
		if (!strncmp(stub->info->name, name, 64)) {
			print("%s -- %s is already registered\n", __FUNCTION__, name);
			return -1;
		}
	}

	info = mm_alloc(sizeof(struct remote_stub_info));
	if (!info)
		return -1;

	r = (struct pt_regs *)mm_alloc(sizeof(struct pt_regs));
	if (!r)
		return -1;

	virt_r = (struct virt_pt_regs *)mm_alloc(sizeof(struct virt_pt_regs));
	if (!virt_r)
		return -1;

	strcpy(info->name, (char *)name);
	info->handler = (unsigned long)handler;
	info->pt_regs_pa = virt_to_phy(r);
	info->virt_pt_regs_pa = virt_to_phy(virt_r);

	hwirq = alloc_msi_irqs(1);
	if (hwirq == -1)
		goto free;

	if (compose_msi_msg(hwirq, &msi_addr, &msi_data))
		goto free;

	if (register_irq_handler(hwirq, remote_stub_irq_handler, (void *)info))
		goto free;

	info->msi_addr = msi_addr;
	info->msi_data = msi_data;

	stub = (struct remote_stub *)mm_alloc(sizeof(struct remote_stub));
	if (!stub)
		goto free;
	stub->info = info;
	list_add_tail(&stub->list, &remote_stub_list);

	sbi_set_remote_stub(virt_to_phy(info));

	return 0;
free:
	mm_free(info, sizeof(struct remote_stub_info));

	return -1;
}
