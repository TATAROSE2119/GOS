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
#include "asm/mmio.h"
#include "print.h"
#include "pci_device_driver.h"
#include "pci.h"
#include "vmap.h"
#include "mm.h"
#include "string.h"
#include "nvme.h"

static int nvme_pci_init(struct pci_device *pdev, void *data)
{
	struct resource res;
	struct nvme_device *ndev;

	ndev = (struct nvme_device *)mm_alloc(sizeof(struct nvme_device));
	if (!ndev) {
		print("nvme_pci: alloc nvme_device fail\n");
		return -1;
	}
	memset((char *)ndev, 0, sizeof(struct nvme_device));

	pci_enable_resource(pdev, 1 << 0);
	pci_set_master(pdev, 1);
	pci_get_resource(pdev, 0, &res);

	ndev->base = (unsigned long)ioremap((void *)res.base,
					    res.end - res.base + 1,
					    NULL);
	ndev->dev = &pdev->dev;

	return nvme_init(ndev);
}

PCI_DRIVER_REGISTER(nvme_pci, nvme_pci_init, 0x1b36, 0x10);
PCI_DRIVER_REGISTER(nvme_pci_qemu, nvme_pci_init, 0x1e49, 0x1);
