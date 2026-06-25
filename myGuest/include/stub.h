#ifndef __STUB_H__
#define __STUB_H__

#include "asm/ptregs.h"
#include "list.h"

struct remote_stub_info {
	char name[64];
	unsigned long handler;
	unsigned long pt_regs_pa;
	unsigned long virt_pt_regs_pa;
	unsigned long msi_addr;
	unsigned long msi_data;
};

struct remote_stub {
	struct list_head list;
	struct remote_stub_info *info;
};

int register_remote_stub(const char *name, void (*handler)(struct pt_regs *regs, struct virt_pt_regs *vregs));
int remote_stub_init(void);

#endif
