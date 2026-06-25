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

#include "asm/ptregs.h"
#include "asm/mmio.h"
#include "mm.h"
#include "print.h"
#include "list.h"
#include "virt.h"
#include "string.h"
#include "stub.h"
#include "gos.h"
#include "vcpu_remote_stub.h"

static void vcpu_remote_stub_doorbell(struct vcpu *vcpu,
				      struct vcpu_remote_stub_info *info)
{
	if (info->msi_addr != vcpu->vs_interrupt_file_gpa)
		return;

	writel(vcpu->vs_interrupt_file_va, info->msi_data);
}

void vcpu_del_remote_stub(struct vcpu *vcpu)
{
	struct vcpu_remote_stub *stub, *tmp;

	list_for_each_entry_safe(stub, tmp, &vcpu->vcpu_remote_stub_list, list) {
		list_del(&stub->list);
		mm_free(stub, sizeof(struct vcpu_remote_stub));
	}
}

static void vcpu_remote_stub_handler(struct pt_regs *regs, void *priv)
{
	struct vcpu_remote_stub *stub = (struct vcpu_remote_stub *)priv;
	struct vcpu_remote_stub_info *stub_info;
	struct vcpu *vcpu;
	unsigned long pt_regs_hva, virt_pt_regs_hva;
	struct virt_pt_regs *vr;

	if (!stub)
		return;

	stub_info = stub->info;
	vcpu = stub->vcpu;
	if ((!stub_info) || (!vcpu))
		return;

	pt_regs_hva = vcpu->host_memory_va +
		(stub_info->pt_regs_gpa - vcpu->guest_memory_pa);

	if ((pt_regs_hva < vcpu->host_memory_va) ||
	    (pt_regs_hva > vcpu->host_memory_va + vcpu->memory_size - sizeof(struct pt_regs)))
		return;

	virt_pt_regs_hva = vcpu->host_memory_va +
		(stub_info->virt_pt_regs_gpa - vcpu->guest_memory_pa);

	if ((virt_pt_regs_hva < vcpu->host_memory_va) ||
	    (virt_pt_regs_hva > vcpu->host_memory_va + vcpu->memory_size - sizeof(struct virt_pt_regs)))
		return;

	memcpy((char *)pt_regs_hva, (char *)regs, sizeof(struct pt_regs));

	vr = (struct virt_pt_regs *)virt_pt_regs_hva;
	vr->htval = read_csr(CSR_HTVAL);
	vr->vsepc = read_csr(CSR_VSEPC);

	vcpu_remote_stub_doorbell(vcpu, stub_info);
}

int vcpu_exist_remote_stub(struct vcpu *vcpu)
{
	struct vcpu_remote_stub *stub;

	list_for_each_entry(stub, &vcpu->vcpu_remote_stub_list, list) {
		if (stub->vcpu == vcpu)
			return 1;
	}

	return 0;
}

int vcpu_register_remote_stub(struct vcpu *vcpu, unsigned long gpa)
{
	struct vcpu_remote_stub_info *rs;
	struct vcpu_remote_stub *new, *stub;

	rs = (struct vcpu_remote_stub_info *)
	     (vcpu->host_memory_va + (gpa - vcpu->guest_memory_pa));

	if ((((unsigned long)rs) < vcpu->host_memory_va) ||
	    (((unsigned long)rs) > (vcpu->host_memory_va +
		vcpu->memory_size - sizeof(struct vcpu_remote_stub_info))))
		return -1;

	list_for_each_entry(stub, &vcpu->vcpu_remote_stub_list, list) {
		if (!strcmp(stub->info->name, rs->name)) {
			print("%s -- stub is already exit...\n", __FUNCTION__);
			return -1;
		}
	}

	new = (struct vcpu_remote_stub *)mm_alloc(sizeof(struct vcpu_remote_stub));
	new->vcpu = vcpu;
	new->info = rs;
	list_add_tail(&new->list, &vcpu->vcpu_remote_stub_list);

	if (register_stub(rs->name, vcpu_remote_stub_handler, (void *)new)) {
		list_del(&new->list);
		mm_free(new, sizeof(struct vcpu_remote_stub));
		return -1;
	}

	return 0;
}

void vcpu_remote_stub_init(struct vcpu *vcpu)
{
	INIT_LIST_HEAD(&vcpu->vcpu_remote_stub_list);
}
