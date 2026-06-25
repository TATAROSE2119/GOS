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
#include "../command.h"
#include "asm/csr.h"
#include "asm/sbi.h"
#include <string.h>
#include "stub.h"

static unsigned long cause = 0x0;
static unsigned long opcode = 0x0;
static unsigned long funct3 = 0x0;
static unsigned long csr = 0x0;

static void parse_trap_info(struct pt_regs *regs)
{
	print("scause:0x%lx stval:0x%lx\n", regs->scause, regs->sbadaddr);

	unsigned long scause = regs->scause;
	unsigned long inst = regs->sbadaddr; // 读取异常指令

	cause = scause & 0x7F;
	opcode = inst & 0x7F;
	funct3 = (inst >> 12) & 0x7;
	csr = (inst >> 20) & 0xFFF;

	print("opcode = 0x%lx, funct3 = 0x%lx, csr = 0x%lx cause = 0x%lx\n", opcode, funct3, csr, cause);
}

static void en_m_all()
{

	unsigned long smstateen = sbi_get_smstateen();
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", smstateen);
	print("enable HSENVCFG in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN0_HSENVCFG_SHIFT);
	print("enable STATEN in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN_STATEN_SHIFT);
	print("enable AIA in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN0_AIA_SHIFT);
	print("enable IMSIC in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN0_IMSIC_SHIFT);
	sbi_set_smstateen(smstateen);
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", sbi_get_smstateen());


	print("read_csr: CSR_SENVCFG 0x%lx \n", read_csr(CSR_SENVCFG));
	print("read_csr: CSR_HENVCFG 0x%lx \n", read_csr(CSR_HENVCFG));
	print("read_csr: CSR_SSTATEEN0 0x%lx \n", read_csr(CSR_SSTATEEN0));
	print("read_csr: CSR_HSTATEEN0 0x%lx \n", read_csr(CSR_HSTATEEN0));
	print("read_csr: CSR_STOPI 0x%lx \n", read_csr(CSR_STOPI));
	print("read_csr: CSR_STOPEI 0x%lx \n", read_csr(CSR_STOPEI));


	print("TEST PASS\n");
}

static void en_h_all()
{
	//enable all bit of SMSTATEEN0
	unsigned long smstateen = sbi_get_smstateen();
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", smstateen);
	print("enable HSENVCFG in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN0_HSENVCFG_SHIFT);
	print("enable STATEN in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN_STATEN_SHIFT);
	print("enable AIA in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN0_AIA_SHIFT);
	print("enable IMSIC in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN0_IMSIC_SHIFT);
	sbi_set_smstateen(smstateen);
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", sbi_get_smstateen());

	//enable all bit of HSTATEEN
	unsigned long hstateen = read_csr(CSR_HSTATEEN0);
	print("read_csr: CSR_HSTATEEN0 0x%lx \n", hstateen);

	print("enable HSENVCFG in HSTATEEN0\n");
	hstateen |= (1UL << SMSTATEEN0_HSENVCFG_SHIFT);
	print("enable STATEN in HSTATEEN0\n");
	hstateen |= (1UL << SMSTATEEN_STATEN_SHIFT);
	print("enable AIA in HSTATEEN0\n");
	hstateen |= (1UL << SMSTATEEN0_AIA_SHIFT);
	print("enable IMSIC in HSTATEEN0\n");
	hstateen |= (1UL << SMSTATEEN0_IMSIC_SHIFT);

	write_csr(CSR_HSTATEEN0,hstateen);
	print("CSR: CSR_HSTATEEN0: 0x%lx\n", read_csr(CSR_HSTATEEN0));
}

static void senvcfg_stub_handler(struct pt_regs *regs, void *priv)
{
	parse_trap_info(regs);
	if (cause == 0x2 && opcode == 0x73 && funct3 == 0x2 && csr == 0x10A) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}
}

static void dis_m_senvcfg()
{
	register_handle_exception_stub_handler(senvcfg_stub_handler);

	print("disable HSENVCFG in SMSTATEEN0\n");
	unsigned long smstateen = sbi_get_smstateen();
	smstateen &= ~(1UL << SMSTATEEN0_HSENVCFG_SHIFT);
	sbi_set_smstateen(smstateen);
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", sbi_get_smstateen());

	//read the unauthorized csr, expect trap exception triggered
	print("read_csr: CSR_SENVCFG 0x%lx \n", read_csr(CSR_SENVCFG));

	print("TEST FAIL\n");
}

static void dis_h_all()
{
	//enable all bit of SMSTATEEN0
	unsigned long smstateen = sbi_get_smstateen();
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", smstateen);
	print("enable HSENVCFG in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN0_HSENVCFG_SHIFT);
	print("enable STATEN in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN_STATEN_SHIFT);
	print("enable AIA in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN0_AIA_SHIFT);
	print("enable IMSIC in SMSTATEEN0\n");
	smstateen  |=  (1UL << SMSTATEEN0_IMSIC_SHIFT);
	sbi_set_smstateen(smstateen);
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", sbi_get_smstateen());

	//dis all bit of HSTATEEN
	unsigned long hstateen = read_csr(CSR_HSTATEEN0);
	print("read_csr: CSR_HSTATEEN0 0x%lx \n", hstateen);

	print("disable HSENVCFG in HSTATEEN0\n");
	hstateen &= ~(1UL << SMSTATEEN0_HSENVCFG_SHIFT);
	print("disable STATEN in HSTATEEN0\n");
	hstateen &= ~(1UL << SMSTATEEN_STATEN_SHIFT);
	print("disable AIA in HSTATEEN0\n");
	hstateen &= ~(1UL << SMSTATEEN0_AIA_SHIFT);
	/* vcpu_remote_stub depends on IMSIC, disable IMSIC will cause stub handler not work
	 * skip this test point currently
	 * print("disable IMSIC in HSTATEEN0\n");
	hstateen &= ~(1UL << SMSTATEEN0_IMSIC_SHIFT);*/
	write_csr(CSR_HSTATEEN0,hstateen);
	print("CSR: CSR_HSTATEEN0: 0x%lx\n", read_csr(CSR_HSTATEEN0));
}

static void henvcfg_stub_handler(struct pt_regs *regs, void *priv)
{
	parse_trap_info(regs);
	if (cause == 0x2 && opcode == 0x73 && funct3 == 0x2 && csr == 0x60A) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}
}

static void dis_m_henvcfg()
{
	register_handle_exception_stub_handler(henvcfg_stub_handler);

	print("disable HSENVCFG in SMSTATEEN0\n");
	unsigned long smstateen = sbi_get_smstateen();
	smstateen &= ~(1UL << SMSTATEEN0_HSENVCFG_SHIFT);
	sbi_set_smstateen(smstateen);
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", sbi_get_smstateen());

	//read the unauthorized csr, expect trap exception triggered
	print("read_csr: CSR_HENVCFG 0x%lx \n", read_csr(CSR_HENVCFG));

	print("TEST FAIL\n");
}

static void sstateen_stub_handler(struct pt_regs *regs, void *priv)
{
	parse_trap_info(regs);
	if (cause == 0x2 && opcode == 0x73 && funct3 == 0x2 && csr == 0x10C) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}
}

static void dis_m_sstateen()
{
	register_handle_exception_stub_handler(sstateen_stub_handler);

	print("disable STATEN in SMSTATEEN0\n");
	unsigned long smstateen = sbi_get_smstateen();
	smstateen &= ~(1UL << SMSTATEEN_STATEN_SHIFT);
	sbi_set_smstateen(smstateen);
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", sbi_get_smstateen());

	//read the unauthorized csr, expect trap exception triggered
	print("read_csr: CSR_SSTATEEN0 0x%lx \n", read_csr(CSR_SSTATEEN0));

	print("TEST FAIL\n");
}

static void hstateen_stub_handler(struct pt_regs *regs, void *priv)
{
	parse_trap_info(regs);
	if (cause == 0x2 && opcode == 0x73 && funct3 == 0x2 && csr == 0x60C) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}
}

static void dis_m_hstateen()
{
	register_handle_exception_stub_handler(hstateen_stub_handler);

	print("disable STATEN in SMSTATEEN0\n");
	unsigned long smstateen = sbi_get_smstateen();
	smstateen &= ~(1UL << SMSTATEEN_STATEN_SHIFT);
	sbi_set_smstateen(smstateen);
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", sbi_get_smstateen());

	//read the unauthorized csr, expect trap exception triggered
	print("read_csr: CSR_HSTATEEN0 0x%lx \n", read_csr(CSR_HSTATEEN0));

	print("TEST FAIL\n");
}


static void aia_stub_handler(struct pt_regs *regs, void *priv)
{
	parse_trap_info(regs);
	if (cause == 0x2 && opcode == 0x73 && funct3 == 0x2 && csr == 0xdb0) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}
}

static void dis_m_aia()
{
	register_handle_exception_stub_handler(aia_stub_handler);

	print("disable AIA in SMSTATEEN0\n");
	unsigned long smstateen = sbi_get_smstateen();
	smstateen &= ~(1UL << SMSTATEEN0_AIA_SHIFT);
	sbi_set_smstateen(smstateen);
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", sbi_get_smstateen());

	//read the unauthorized csr, all other state added by the AIA and
	//not controlled by bits 60 and 58, expect trap exception triggered
	print("read_csr: CSR_STOPI 0x%lx \n", read_csr(CSR_STOPI));

	print("TEST FAIL\n");
}

static void imsic_stub_handler(struct pt_regs *regs, void *priv)
{
	parse_trap_info(regs);
	if (cause == 0x2 && opcode == 0x73 && funct3 == 0x2 && csr == 0x15c) {
		print("TEST PASS\n");
	} else {
		print("TEST FAIL\n");
	}
}

static void dis_m_imsic()
{
	register_handle_exception_stub_handler(imsic_stub_handler);

	print("disable IMSIC in SMSTATEEN0\n");
	unsigned long smstateen = sbi_get_smstateen();
	smstateen &= ~(1UL << SMSTATEEN0_IMSIC_SHIFT);
	sbi_set_smstateen(smstateen);
	print("CSR: CSR_MSTATEEN0: 0x%lx\n", sbi_get_smstateen());

	//read the unauthorized csr, m:[vs|s]topei, expect trap exception triggered
	print("read_csr: CSR_STOPEI 0x%lx \n", read_csr(CSR_STOPEI));

	print("TEST FAIL\n");
}


static void Usage()
{
	print("Usage: smstateen_test [cmd]\n");
	print("cmd option:\n");
	print("    en_m_all \n");
	print("    en_h_all \n");
	print("    dis_h_all \n");
	print("    dis_m_senvcfg \n");
	print("    dis_m_henvcfg \n");
	print("    dis_m_sstateen \n");
	print("    dis_m_hstateen \n");
	print("    dis_m_aia \n");
	print("    dis_m_imsic \n");
}

static int cmd_smstateen_handler(int argc, char *argv[], void *priv)
{
	if (argc < 1) {
		print("Invalid input params\n");
		Usage();
		return -1;
	}
	if (!strncmp(argv[0], "en_m_all", sizeof("en_m_all"))) {
		en_m_all();
	} else if (!strncmp(argv[0], "en_h_all", sizeof("en_h_all"))) {
		en_h_all();
	}else if (!strncmp(argv[0], "dis_h_all", sizeof("dis_h_all"))) {
		dis_h_all();
	} else if (!strncmp(argv[0], "dis_m_senvcfg", sizeof("dis_m_senvcfg"))) {
		dis_m_senvcfg();
	} else if (!strncmp(argv[0], "dis_m_henvcfg", sizeof("dis_m_henvcfg"))) {
		dis_m_henvcfg();
	} else if (!strncmp(argv[0], "dis_m_sstateen", sizeof("dis_m_sstateen"))) {
		dis_m_sstateen();
	} else if (!strncmp(argv[0], "dis_m_hstateen", sizeof("dis_m_hstateen"))) {
		dis_m_hstateen();
	} else if (!strncmp(argv[0], "dis_m_aia", sizeof("dis_m_aia"))) {
		dis_m_aia();
	} else if (!strncmp(argv[0], "dis_m_imsic", sizeof("dis_m_imsic"))) {
		dis_m_imsic();
	} else {
		print("unsupported command\n");
		Usage();
		return -1;
	}

	return 0;
}

static const struct command cmd_smstateen = {
	.cmd = "smstateen_test",
	.handler = cmd_smstateen_handler,
	.priv = NULL,
};

int cmd_smstateen_init()
{
	register_command(&cmd_smstateen);

	return 0;
}

APP_COMMAND_REGISTER(smstateen, cmd_smstateen_init);