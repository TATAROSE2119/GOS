#ifndef __VCPU_REMOTE_STUB_H__
#define __VCPU_REMOTE_STUB_H__

#include "virt.h"
#include "list.h"

struct vcpu_remote_stub_info {
	char name[64];
	unsigned long handler_gpa;
	unsigned long pt_regs_gpa;
	unsigned long virt_pt_regs_gpa;
	unsigned long msi_addr;
	unsigned long msi_data;
};

struct vcpu_remote_stub {
	struct list_head list;
	struct vcpu *vcpu;
	struct vcpu_remote_stub_info *info;
};

void vcpu_del_remote_stub(struct vcpu *vcpu);
int vcpu_exist_remote_stub(struct vcpu *vcpu);
int vcpu_register_remote_stub(struct vcpu *vcpu, unsigned long gpa);
void vcpu_remote_stub_init(struct vcpu *vcpu);

#endif
