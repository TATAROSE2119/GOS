#!/bin/bash
# Copyright (c) 2024 Beijing Institute of Open Source Chip (BOSC)
#
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2 or later, as published by the Free Software Foundation.
#
# This program is distributed in the hope it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along with
# this program.  If not, see <http://www.gnu.org/licenses/>.


if [ "$#" -eq 1 ];then
	if [ "$1" = "run" ];then
		bear -- make $1
		exit 0
	
	elif [ "$1" = "run-debug" ];then
		bear -- make $1
		exit 0

	elif [ "$1" = "clean" ];then
		bear -- make $1
		exit 0
	
	elif [ "$1" = "dtb" ];then
		bear -- make $1
		exit 0

	elif [ "$1" = "vcs-h" ];then
		bear -- make gos-vcs-h.dtb
		bear -- make vcs_h_defconfig

	elif [ "$1" = "vcs-minimum" ];then
		bear -- make gos-minimum.dtb
		bear -- make minimum_defconfig

	elif [ "$1" = "pldm" ];then
		bear -- make gos-vcs-h.dtb
		bear -- make pldm_defconfig

	elif [ "$1" = "default" ];then
		bear -- make gos-qualcore.dtb
		bear -- make defconfig

	elif [ "$1" = "fpga" ];then
		bear -- make gos-dualcore.dtb
		bear -- make fpga_defconfig

	elif [ "$1" = "default-Sv48" ];then
		bear -- make gos-dualcore-Sv48.dtb
		bear -- make Sv48_defconfig

	elif [ "$1" = "default-Sv57" ];then
		bear -- make gos-dualcore-Sv57.dtb
		bear -- make Sv57_defconfig
	elif [ "$1" = "fpga-h" ];then
		bear -- make gos-singlecore.dtb
		bear -- make fpga_h_defconfig
	elif [ "$1" = "fpga-h-xdma" ];then
		bear -- make gos-singlecore.dtb
		bear -- make fpga_h_xdma_defconfig
	elif [ "$1" = "vcs-minimum-sv48" ];then
		bear -- make gos-minimum-sv48.dtb
		bear -- make minimum_sv48_defconfig
	elif [ "$1" = "vcs-vs-multi-test" ];then
		bear -- make gos-vcs-h.dtb
		bear -- make multi_imsic_defconfig
	elif [ "$1" = "cmn-fpga-imsic-multi-test" ];then
		bear -- make gos-singlecore.dtb
		bear -- make multi_imsic_cmn_fpga_defconfig
	elif [ "$1" = "mellite-fpga" ];then
		bear -- make gos-mellite.dtb
		bear -- make mellite_defconfig
	elif [ "$1" = "mellite-sram" ];then
		bear -- make gos-mellite-sram.dtb
		bear -- make mellite_zebu_sram_defconfig
	elif [ "$1" = "vcs-aia-minimum" ];then
		bear -- make gos-minimum.dtb
		bear -- make vcs_aia_minimum_defconfig
	elif [ "$1" = "vcs-qual-minimum" ];then
		bear -- make gos-qualcore-minimum.dtb
		bear -- make vcs_aia_minimum_defconfig
	elif [ "$1" = "nanhu-board" ];then
		bear -- make gos-nanhu-board.dtb
		bear -- make nanhu_board_defconfig
	elif [ "$1" = "nanhu-v5" ];then
		bear -- make gos-singlecore.dtb
		bear -- make nanhu-v5_defconfig
	else
		bear -- make $1
	fi
fi

bear -- make clean
bear -- make autoconf
bear -- make 

if [ "$1" = "fpga" ];then
	bear -- make fpga
fi
if [ "$1" = "fpga-h" ];then
        bear -- make fpga
fi

