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

#include "command.h"
#include <string.h>
#include <asm/type.h>
#include "asm/csr.h"
#include "stub.h"

#if 0
static unsigned long cause = 0x0;
static unsigned long opcode = 0x0;
static unsigned long funct3 = 0x0;
static unsigned long csr = 0x0;
#endif

static void en_h_all()
{
	print("read all enabled CSR_* \n");
	print("read_csr: CSR_SENVCFG 0x%lx \n", read_csr(CSR_SENVCFG));
	print("read_csr: CSR_SSTATEEN0 0x%lx \n", read_csr(CSR_SSTATEEN0));
	print("read_csr: CSR_STOPI 0x%lx \n", read_csr(CSR_STOPI));
	print("read_csr: CSR_STOPEI 0x%lx \n", read_csr(CSR_STOPEI));


	print("TEST PASS\n");
}

static void senvcfg_stub_handler(struct pt_regs *regs, struct virt_pt_regs *vregs)
{
	print("trigger handler\n");
	// currently vsstval return 0x0, based on the spec, 0x0 or meaningful value both are valid
	// FIXEME, for future if vsstval return meaningful value, we need to add more check point for vsstval
	if (EXC_VIRTUAL_INST_FAULT == regs->a1) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}
}

static void dis_h_senvcfg()
{
	register_remote_stub("vcpu_do_trap_error",senvcfg_stub_handler);

	//read the unauthorized csr, expect trap exception triggered
	print("read_csr: CSR_SENVCFG 0x%lx \n", read_csr(CSR_SENVCFG));

	print("TEST FAIL\n");
}

static void sstateen_stub_handler(struct pt_regs *regs, struct virt_pt_regs *vregs)
{
	if (EXC_VIRTUAL_INST_FAULT == regs->a1) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}
}

static void dis_h_sstateen()
{
	register_remote_stub("vcpu_do_trap_error",sstateen_stub_handler);

	//read the unauthorized csr, expect trap exception triggered
	print("read_csr: CSR_SSTATEEN0 0x%lx \n", read_csr(CSR_SSTATEEN0));

	print("TEST FAIL\n");
}

static void aia_stub_handler(struct pt_regs *regs, struct virt_pt_regs *vregs)
{
	if (EXC_VIRTUAL_INST_FAULT == regs->a1) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}
}

static void dis_h_aia()
{
	register_remote_stub("vcpu_do_trap_error",aia_stub_handler);

	//read the unauthorized csr, expect trap exception triggered
	print("read_csr: CSR_STOPI 0x%lx \n", read_csr(CSR_STOPI));

	print("TEST FAIL\n");
}

#if 0
static void imsic_stub_handler(struct pt_regs *regs, struct virt_pt_regs *vregs)
{
	if (EXC_VIRTUAL_INST_FAULT == regs->a1) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}
}

static void dis_h_imsic()
{
	register_remote_stub("vcpu_do_trap_error",imsic_stub_handler);

	//read the unauthorized csr, expect trap exception triggered
	print("read_csr: CSR_STOPEI 0x%lx \n", read_csr(CSR_STOPEI));

	print("TEST FAIL\n");
}
#endif

static void Usage(void)
{
	print("Usage:vcpu_run hstateen_test [cmd] \n");
	print("cmd option:\n");
	print("    -- en_h_all \n");
	print("    -- dis_h_senvcfg \n");
	print("    -- dis_h_sstateen \n");
	print("    -- dis_h_aia \n");

	return;
}

static int cmd_hstateen_handler(int argc, char *argv[], void *priv)
{
	if (argc < 1) {
		print("Invalid input params\n");
		Usage();
		return -1;
	}
	if (!strncmp(argv[0], "en_m_all", sizeof("en_m_all"))) {
		print("en_m_all\n");
	} else if (!strncmp(argv[0], "en_h_all", sizeof("en_h_all"))) {
		en_h_all();
	} else if (!strncmp(argv[0], "dis_h_senvcfg", sizeof("dis_h_senvcfg"))) {
		dis_h_senvcfg();
	} else if (!strncmp(argv[0], "dis_h_sstateen", sizeof("dis_h_sstateen"))) {
		dis_h_sstateen();
	} else if (!strncmp(argv[0], "dis_h_aia", sizeof("dis_h_aia"))) {
		dis_h_aia();
	} else if (!strncmp(argv[0], "dis_h_imsic", sizeof("dis_h_imsic"))) {
		// FIXME
		// vcpu_remote_stub depends on IMSIC, disable IMSIC will cause stub handler not work
		// skip this test point currently
	} else {
		print("unsupported command\n");
		Usage();
		return -1;
	}

	return 0;
}

static const struct command cmd_hstateen_test = {
	.cmd = "hstateen_test",
	.handler = cmd_hstateen_handler,
	.priv = NULL,
};

int cmd_hstateen_init()
{
	register_command(&cmd_hstateen_test);

	return 0;
}

APP_COMMAND_REGISTER(hstateen, cmd_hstateen_init);
