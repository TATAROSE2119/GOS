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

#include "user.h"
#include "mm.h"
#include "print.h"
#include "string.h"
#include "asm/csr.h"
#include "list.h"
#include "spinlocks.h"
#include "irq.h"
#include "asm/type.h"
#include "user_memory.h"
#include "user_exception.h"
#include "asm/ptregs.h"
#include "percpu.h"
#include "asm/sbi.h"
#include "asm/pgtable.h"
#include "asm/tlbflush.h"
#include "task.h"
#include "gos.h"

#define USER_SPACE_SHARE_MEMORY 0x0
#define USER_SPACE_SHARE_MEMORY_SIZE 0x1000

extern void do_exception_vector(void);
extern char user_bin[];
static DEFINE_PER_CPU(struct list_head, user_list);

static DEFINE_PER_CPU(struct user *, current_user);

static unsigned long userid_bitmap;

static void user_userid_init(void) { userid_bitmap = 0; }

static void user_update_run_params(struct user *user)
{
	struct user_run_params *params = user->run_params;
	struct user_run_params *s_params = &user->s_run_params;

	if (!params)
		return;

	if (params->busy == 1)
		return;

	if (s_params->busy == 1) {
		__smp_rmb();
		memcpy((char *)params, (char *)s_params,
		       sizeof(struct user_run_params));
		__smp_wmb();
		params->busy = 1;
		s_params->busy = 0;
	}
}

static int user_set_run_params(struct user *user, struct user_run_params *cmd)
{
	struct user_run_params *params = &user->s_run_params;

	while (params->busy == 1)
		;

	__smp_rmb();

	memcpy((char *)params, (char *)cmd, sizeof(struct user_run_params));

	__smp_wmb();

	params->busy = 1;

	return 0;
}

static struct user *__user_create(void)
{
	struct user *user;
	struct user_cpu_context *u_context;
	struct list_head *users;

	user = (struct user *)mm_alloc(sizeof(struct user));
	if (!user) {
		print("%s -- Out of memory\n", __FUNCTION__);
		return NULL;
	}
	memset((char *)user, 0, sizeof(struct user));

	u_context = &user->cpu_context.u_context;
	u_context->sstatus = read_csr(CSR_SSTATUS);
	u_context->sstatus &= ~SR_SPP;
	u_context->sstatus |= SR_SPIE;

	if (read_csr(sstatus) & SR_FS)
		u_context->sstatus |= SR_FS;

#if CONFIG_ENABLE_VECTOR
	if (read_csr(sstatus) & SR_VS)
		u_context->sstatus |= SR_VS;
#endif

	INIT_LIST_HEAD(&user->memory_region);
	__SPINLOCK_INIT(&user->lock);

	users = &per_cpu(user_list, sbi_get_cpu_id());
	list_add_tail(&user->list, users);

	return user;
}

static int find_free_userid(unsigned long *userid)
{
	unsigned long bitmap = *userid;
	int pos = 0;

	while (bitmap & 0x01) {
		if (pos == 64)
			return -1;
		bitmap = bitmap >> 1;
		pos++;
	}

	*userid |= (1UL) << pos;

	return pos;
}

static int user_alloc_userid(int cpu)
{

	return find_free_userid(&userid_bitmap);
}

static void user_update_userid(struct user *user)
{
	user->user_id = user_alloc_userid(sbi_get_cpu_id());
}

static void __dump_user_info(int cpu)
{
	struct user *user;
	struct list_head *users;

	users = &per_cpu(user_list, cpu);
	if (!users) {
		print("Invalid hart id: %d\n", cpu);
		return;
	}

	print("+++++++++++++ user info on cpu%d +++++++++++++\n", cpu);
	list_for_each_entry(user, users, list)
	{
		print("@@@@@@@@@@@@@@ user%d info: @@@@@@@@@@@@@@\n",
		      user->user_id);
		print("- userid : %d\n", user->user_id);
		print("- pgdp : 0x%lx\n", user->pgdp);
		print("- memory info:\n");
		print("    code_va : 0x%lx\n", user->user_code_va);
		print("    code_user_va : 0x%lx\n", user->user_code_user_va);
		print("    code_pa : 0x%lx\n", user->user_code_pa);
		print("    share_mem_va : 0x%lx\n", user->user_share_memory_va);
		print("    share_mem_user_va : 0x%lx\n",
		      user->user_share_memory_user_va);
		print("    share_mem_pa : 0x%lx\n", user->user_share_memory_pa);
		print("\n");
	}
}

struct user *get_user(int userid, int cpu)
{
	struct user *user;
	struct list_head *users;

	users = &per_cpu(user_list, cpu);

	list_for_each_entry(user, users, list)
	{
		if (user->user_id == userid)
			return user;
	}

	return NULL;
}

void set_current_user(struct user *user)
{
	per_cpu(current_user, sbi_get_cpu_id()) = user;
}

struct user *get_current_user(void)
{
	return per_cpu(current_user, sbi_get_cpu_id());
}

void dump_user_info_on_all_cpu(void)
{
	int cpu;

	for_each_online_cpu(cpu) __dump_user_info(cpu);
}

void dump_user_info_on_cpu(int cpu) { __dump_user_info(cpu); }

struct user *user_create_force(void) { return __user_create(); }

struct user *user_create(void)
{
	struct user *user;
	struct list_head *users;

	users = &per_cpu(user_list, sbi_get_cpu_id());

	if (list_empty(users))
		goto create_user;

	user = list_entry(list_first(users), struct user, list);

	return user;

create_user:
	return __user_create();
}

int user_mode_run(struct user *user, struct user_run_params *params)
{
	extern unsigned long __user_payload_start;
	extern unsigned long __user_payload_end;
	int user_bin_size =
	    (char *)&__user_payload_end - (char *)&__user_payload_start;
	char *user_bin_ptr = user_bin;
	struct user_cpu_context *u_context = &user->cpu_context.u_context;
	struct pt_regs *regs;

	if (user->mapping == 1) {
		return user_set_run_params(user, params);
	}

	user_update_userid(user);
	print("userid: %d\n", user->user_id);

	/* map user code */
	user->user_code_user_va = USER_SPACE_CODE_START;
	if (user->user_code_user_va + user_bin_size >
	    USER_SPACE_FIXED_MMAP + USER_SPACE_FIXED_MMAP_SIZE) {
		print("%s -- code is too large! 0x%lx bytes\n", __FUNCTION__,
		      user_bin_size);
		return -1;
	}
	user->user_code_va = (unsigned long)mm_alloc(user_bin_size);
	if (!user->user_code_va) {
		print("%s -- Out of memory\n", __FUNCTION__);
		return -1;
	}
	user->user_code_pa = virt_to_phy(user->user_code_va);

	print("user space user mode page mapping -- va: 0x%lx --> pa: 0x%lx, "
	      "size:0x%x\n",
	      user->user_code_user_va, user->user_code_pa, user_bin_size);
	user_page_mapping(user->user_code_pa, user->user_code_user_va,
			  user_bin_size);

	if (-1 == add_user_space_memory(user, user->user_code_user_va,
					user_bin_size)) {
		print("user space memory overlay!! start:0x%lx, len: 0x%x\n",
		      user->user_code_user_va, user_bin_size);
		return -1;
	}

	/* map share memory */
	user->user_share_memory_va =
	    (unsigned long)mm_alloc(USER_SPACE_SHARE_MEMORY_SIZE);
	if (!user->user_share_memory_va) {
		print("%s -- Out of memory\n", __FUNCTION__);
		return -1;
	}
	user->user_share_memory_pa = virt_to_phy(user->user_share_memory_va);
	user->user_share_memory_user_va = USER_SPACE_SHARE_MEMORY;

	user_page_mapping(user->user_share_memory_pa,
			  user->user_share_memory_user_va,
			  USER_SPACE_SHARE_MEMORY_SIZE);

	local_flush_tlb_all();

	if (-1 == add_user_space_memory(user, user->user_share_memory_user_va,
					USER_SPACE_SHARE_MEMORY_SIZE)) {
		print("user space memory overlay!! start:0x%lx, len: 0x%x\n",
		      user->user_share_memory_user_va,
		      USER_SPACE_SHARE_MEMORY_SIZE);
		return -1;
	}

	memcpy((char *)user->user_code_va, user_bin_ptr, user_bin_size);
	if (params) {
		params->userid = user->user_id;
		params->cpu = sbi_get_cpu_id();
		memcpy((char *)user->user_share_memory_va, (void *)params,
		       sizeof(struct user_run_params));
		user->run_params =
		    (struct user_run_params *)user->user_share_memory_va;
		user->run_params->busy = 1;
	}

	/* Update user mode entry and param */
	u_context->sepc = user->user_code_user_va;
	u_context->a0 = user->user_share_memory_user_va;

	user->mapping = 1;

	regs = mm_alloc(sizeof(struct pt_regs));
	if (!regs) {
		print("user space alloc pt_regs failed !\n");
		return -1;
	}
	memset((char *)regs, 0, sizeof(struct pt_regs));

#ifndef CONFIG_ENABLE_MULTI_TASK
	user->pgdp = (void *)phy_to_virt(get_default_pgd());
#else
	user->pgdp = get_current_task()->pgdp;
#endif

	set_current_user(user);

	while (1) {
		user_update_run_params(user);
		disable_local_irq();
		user_mode_switch_to(&user->cpu_context);	//切换到用户态
		if (do_user_exception(user, regs) == -1) {	//用户态异常处理
			enable_local_irq();	//异常处理失败，返回内核态
			break;
		}
		enable_local_irq();
	}

	mm_free((void *)regs, sizeof(struct pt_regs));

	return 0;
}

#if CONFIG_FORK
// 子任务入口
static int user_child_start(void *data)
{
	struct user *child = (struct user *)data;
	struct pt_regs *regs;
	regs = mm_alloc(sizeof(struct pt_regs));
	if (!regs) {
		return -1;
	}
	memset((char *)regs, 0, sizeof(struct pt_regs));

	child->pgdp = get_current_task()->pgdp;

	set_current_user(child);

	while (1) {
		disable_local_irq();

		user_mode_switch_to(&child->cpu_context);
		if (do_user_exception(child, regs) == -1) {
			enable_local_irq();
			break;
		}

		enable_local_irq();
	}
	mm_free((void *)regs, sizeof(struct pt_regs));
	return 0;
}
int do_fork(struct user *parent)
{
	struct user *child;
	struct user_memory_region *region;	// 用户内存区域
	void *child_pgdp;	// 子任务页表基地址
	void *default_pgdp = (void *)phy_to_virt(get_default_pgd());// 内核默认页表基地址

	child = user_create_force();	// 创建子任务
	if (!child) {
		return -1;
	}
	user_update_userid(child);	// 更新子任务的用户 ID

	child_pgdp = mm_alloc(PAGE_SIZE);	// 分配子任务页表基地址
	if (!child_pgdp) {
		return -1;
	}
	memcpy((char *)child_pgdp, (char *)default_pgdp, PAGE_SIZE);

	/*
	 * memcpy 继承了内核 PGD 的低地址映射(含内核代码 0x80200000 与用户区,
	 * 二者同在 PGD[0])。copy_page_range 会在遍历用户区时把与父共享的下级
	 * 页表逐级克隆成子私有表(保留内核等兄弟项)，从而只隔离用户区、不破坏
	 * 内核映射。因此这里不能整表清 PGD[0]。
	 */
	list_for_each_entry(region, &parent->memory_region, list)
	{
		if (copy_page_range(region->start, region->end,
				    (unsigned long *)child_pgdp,
				    (unsigned long *)phy_to_virt(
					(unsigned long)parent->pgdp))) {// 复制父任务的用户区页表到子任务
			return -1;
		}
		add_user_space_memory(child, region->start,
				      region->end - region->start);
	}
	// clone U态上下文加元数据
	child->cpu_context = parent->cpu_context; // 整包寄存器/CSR
	child->cpu_context.u_context.a0 = 0;	  // child fork() return 0;
	child->cpu_context.u_context.sepc = parent->cpu_context.u_context.sepc +
					    4; // 跳过ecall，从下一条指令继续

	child->user_code_user_va = parent->user_code_user_va;
	child->user_code_pa = parent->user_code_pa;
	child->user_code_va = parent->user_code_va;
	child->user_share_memory_user_va = parent->user_share_memory_user_va;
	child->user_share_memory_pa = parent->user_share_memory_pa;
	child->user_share_memory_va = parent->user_share_memory_va;
	child->mapping = 1; // 标记为已经映射

	if (create_task("user_child", user_child_start, (void *)child,
			sbi_get_cpu_id(), 0, 0,
			(void *)virt_to_phy((unsigned long)child_pgdp)) < 0) {
		return -1;
	}
	return child->user_id;
}

#endif

void user_init(void)
{
	int cpu;
	struct list_head *users;

	for_each_online_cpu(cpu)
	{
		users = &per_cpu(user_list, cpu);
		INIT_LIST_HEAD(users);
	}

	user_userid_init();
}
