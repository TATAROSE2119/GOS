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

/*
 * This program is referenced from the nvme driver in linux
 *
 * Copyright (C) 2017 NXP Semiconductors
 * Copyright (C) 2017 Bin Meng <bmeng.cn@gmail.com>
 */

#include "asm/mmio.h"
#include "asm/barrier.h"
#include "asm/type.h"
#include "math.h"
#include "mm.h"
#include "print.h"
#include "clock.h"
#include "string.h"
#include "dma-mapping.h"
#include "spinlocks.h"
#include "align.h"
#include "nvme.h"
#include "nvme_blk.h"

#define NVME_Q_DEPTH		2
#define NVME_AQ_DEPTH		2
#define NVME_SQ_SIZE(depth)	(depth * sizeof(struct nvme_command))
#define NVME_CQ_SIZE(depth)	(depth * sizeof(struct nvme_completion))

#define MAX_PRP_POOL            512

static unsigned short cmdid = 0;

static int nvme_submit_admin_cmd(struct nvme_device *ndev,
				 struct nvme_command *cmd,
				 int *res);

static int nvme_wait_csts(struct nvme_device *ndev,
			  unsigned int mask, unsigned int val)
{
	int timeout;
	unsigned long start;

	timeout = NVME_CAP_TIMEOUT(ndev->cap) * 500;

	start = get_clocksource_counter();
	while (1) {
		if ((readl(ndev->base + NVME_MMIO_CSTS) & mask) == val)
			return 0;

		if (get_clocksource_counter() - start > timeout)
			break;
	}

	return 0;
}

static int nvme_enable_ctrl(struct nvme_device *ndev)
{
	unsigned int min_shift, max_shift, shift = 12;

	min_shift = NVME_CAP_MPSMIN(ndev->cap) + 12;
	max_shift = NVME_CAP_MPSMAX(ndev->cap) + 12;

	if ((shift < min_shift) || (shift > max_shift)) {
		print("nvme: Invalid page shift\n");
		return -1;
	}
	ndev->page_size = 1 << shift;

	if (NVME_CAP_CSS(ndev->cap) & NVME_CAP_CSS_CSI)
		ndev->ctrl_config = NVME_CC_CSS_CSI;
	else
		ndev->ctrl_config = NVME_CC_CSS_NVM;

	if ((ndev->cap & NVME_CAP_CRMS_CRWMS) &&
		(ndev->cap & NVME_CAP_CRMS_CRIMS))
		ndev->ctrl_config |= NVME_CC_CRIME;

	ndev->ctrl_config |= (shift - 12) << NVME_CC_MPS_SHIFT;
	ndev->ctrl_config |= NVME_CC_AMS_RR | NVME_CC_SHN_NONE;
	ndev->ctrl_config |= NVME_CC_IOSQES | NVME_CC_IOCQES;

	writel(ndev->base + NVME_MMIO_CC, ndev->ctrl_config);

	ndev->ctrl_config = readl(ndev->base + NVME_MMIO_CC);

	ndev->ctrl_config |= NVME_CC_ENABLE;
	writel(ndev->base + NVME_MMIO_CC, ndev->ctrl_config);

	return nvme_wait_csts(ndev, NVME_CSTS_RDY, NVME_CSTS_RDY);
}

static int nvme_disable_ctrl(struct nvme_device *ndev)
{
	ndev->ctrl_config &= ~NVME_CC_SHN_MASK;
	ndev->ctrl_config &= ~NVME_CC_ENABLE;
	writel(ndev->base + NVME_MMIO_CC, ndev->ctrl_config);

	return nvme_wait_csts(ndev, NVME_CSTS_RDY, 0);
}

#if 0
static int nvme_shutdown_ctrl(struct nvme_device *ndev)
{
	ndev->ctrl_config &= ~NVME_CC_SHN_MASK;
	ndev->ctrl_config |= NVME_CC_SHN_NORMAL;
	writel(ndev->base + NVME_MMIO_CC, ndev->ctrl_config);

	return nvme_wait_csts(ndev, NVME_CSTS_SHST_MASK, NVME_CSTS_SHST_CMPLT);
}
#endif

static struct nvme_queue *nvme_alloc_queue(struct nvme_device *ndev,
					   int id, int depth)
{
	struct nvme_queue *q;

	q = mm_alloc(sizeof(struct nvme_queue));
	if (!q)
		goto ret;
	memset((char *)q, 0, sizeof(struct nvme_queue));

	q->submit_q = (struct nvme_command *)dma_alloc(ndev->dev, &q->sq_dma_addr,
					RESIZE(NVME_CQ_SIZE(depth), 4096), NULL);
	if (!q->submit_q)
		goto free1;
	memset((char *)q->submit_q, 0, NVME_CQ_SIZE(depth));

	q->complete_q = (struct nvme_completion *)dma_alloc(ndev->dev, &q->cq_dma_addr,
					RESIZE(NVME_SQ_SIZE(depth), 4096), NULL);
	if (!q->complete_q)
		goto free2;
	memset((char *)q->complete_q, 0, NVME_SQ_SIZE(depth));

	q->id = id;
	q->q_depth = depth;
	q->dev = ndev;

	return q;

free2:
	mm_free(q->submit_q, NVME_SQ_SIZE(depth));
free1:
	mm_free(q, sizeof(struct nvme_queue));
ret:
	return NULL;
}

static int nvme_init_queue(struct nvme_queue *q, int id)
{
	struct nvme_device *ndev = q->dev;

	q->sq_tail = 0;
	q->sq_head = 0;
	q->cq_head = 0;
	q->cq_phase = 1;
	q->q_db = &ndev->dbs[id * 2 * ndev->doorbell_stride];

	return 0;
}

static int nvme_alloc_cq(struct nvme_device *ndev, int id,
			 struct nvme_queue *q)
{
	struct nvme_command c = { };
	int flags = NVME_QUEUE_PHYS_CONFIG | NVME_CQ_IRQ_ENABLED;

	c.create_cq.opcode = nvme_admin_create_cq;
	c.create_cq.prp1 = (unsigned long)q->cq_dma_addr;
	c.create_cq.cqid = (unsigned short)id;
	c.create_cq.qsize = (unsigned short)q->q_depth - 1;
	c.create_cq.cq_flags = (unsigned short)flags;
	c.create_cq.irq_vector = (unsigned short)q->irq_vector;

	return nvme_submit_admin_cmd(ndev, &c, NULL);
}

static int nvme_alloc_sq(struct nvme_device *ndev, int id,
			 struct nvme_queue *q)
{
	struct nvme_command c = { };
	int flags = NVME_QUEUE_PHYS_CONFIG | NVME_SQ_PRIO_MEDIUM;

	c.create_sq.opcode = nvme_admin_create_sq;
	c.create_sq.prp1 = (unsigned long)q->sq_dma_addr;
	c.create_sq.sqid = (unsigned short)id;
	c.create_sq.qsize = (unsigned short)q->q_depth - 1;
	c.create_sq.sq_flags = (unsigned short)flags;
	c.create_sq.cqid = id;

	return nvme_submit_admin_cmd(ndev, &c, NULL);
}


static int nvme_create_queue(struct nvme_queue *q, int id)
{
	q->irq_vector = id - 1;

	if (nvme_alloc_cq(q->dev, id, q)) {
		print("nvme: alloc cq fail\n");
		return -1;
	}

	if (nvme_alloc_sq(q->dev, id, q)) {
		print("nvme: alloc sq fail\n");
		return -1;
	}

	return 0;
}

static int nvme_create_io_queues(struct nvme_device *ndev)
{
	int i;

	for (i = 0; i < ndev->io_queue_count; i++) {
		ndev->io_q[i] = nvme_alloc_queue(ndev, i + 1, ndev->q_depth);
		if (!ndev->io_q[i])
			return -1;
		nvme_init_queue(ndev->io_q[i], i + 1);

		nvme_create_queue(ndev->io_q[i], i + 1);
	}

	return 0;
}

static int nvme_configure_admin_queue(struct nvme_device *ndev)
{
	unsigned int aqa;

	ndev->subsystem = readl(ndev->base + NVME_MMIO_VS) >= NVME_VS(1, 1, 0) ?
				NVME_CAP_NSSRC(ndev->cap) : 0;

	if (ndev->subsystem &&
	    (readl(ndev->base + NVME_MMIO_CSTS) & NVME_CSTS_NSSRO))
		writel(ndev->base + NVME_MMIO_CSTS, NVME_CSTS_NSSRO);

	if (-1 == nvme_disable_ctrl(ndev))
		return -1;

	ndev->admin_q = nvme_alloc_queue(ndev, 0, NVME_AQ_DEPTH);
	if (!ndev->admin_q)
		return -1;

	aqa = ndev->admin_q->q_depth - 1;
	aqa |= aqa << 16;
	writel(ndev->base + NVME_MMIO_AQA, aqa);

	writeq(ndev->base + NVME_MMIO_ASQ, ndev->admin_q->sq_dma_addr);
	writeq(ndev->base + NVME_MMIO_ACQ, ndev->admin_q->cq_dma_addr);

	if (-1 == nvme_enable_ctrl(ndev))
		return -1;

	nvme_init_queue(ndev->admin_q, 0);

	return 0;
}

static void nvme_submit_cmd(struct nvme_queue *q,
			    struct nvme_command *cmd)
{
	int tail = q->sq_tail;

	memcpy((char *)&q->submit_q[tail], (char *)cmd, sizeof(struct nvme_command));

	mb();

	if (++tail == q->q_depth)
		tail = 0;

	writel(q->q_db, tail);
	q->sq_tail = tail;
}

static int nvme_submit_sync_cmd(struct nvme_queue *q,
				struct nvme_command *cmd,
				int *res, int timeout)
{
	int head = q->cq_head;
	int phase = q->cq_phase;
	unsigned long start;
	unsigned short status;

	cmd->common.command_id = cmdid++;
	nvme_submit_cmd(q, cmd);

	start = get_clocksource_counter();
	while (1) {
		if (get_clocksource_counter() - start >= timeout)
			return -1;

		status = READ_ONCE(q->complete_q[head].status);
		if ((status & 0x01) == phase)
			break;
	}

	status >>= 1;
	if (status)
		print("ERROR: status = %x, phase = %d, head = %d\n",
		       status, phase, head);

	if (++head == q->q_depth) {
		head = 0;
		phase = !phase;
	}
	writel(q->q_db + q->dev->doorbell_stride, head);
	q->cq_head = head;
	q->cq_phase = phase;

	return status;
}

static int nvme_submit_admin_cmd(struct nvme_device *ndev,
				 struct nvme_command *cmd,
				 int *res)
{
	return nvme_submit_sync_cmd(ndev->admin_q, cmd, res, 6000);
}

#if 0
static int nvme_get_features(struct nvme_device *ndev,
			     unsigned int fid, unsigned int nsid,
			     unsigned long dma_addr, int *res)
{
	return 0;
}
#endif

static int nvme_set_features(struct nvme_device *ndev,
			     unsigned int fid, unsigned int dword11,
			     unsigned long dma_addr, int *res)
{
	struct nvme_command c = { };

	c.features.opcode = nvme_admin_set_features;
	c.features.prp1 = dma_addr;
	c.features.fid = fid;
	c.features.dword11 = dword11;

	return nvme_submit_admin_cmd(ndev, &c, res);
}

static int nvme_identify(struct nvme_device *ndev, int nsid,
			 int cns, void *addr)
{
	struct nvme_command c = { };
	int page_size = ndev->page_size;
	int offset = ((unsigned long)addr) & (page_size - 1);
	int len = sizeof(struct nvme_id_ctrl);

	c.identify.opcode = nvme_admin_identify;
	c.identify.nsid = nsid;
	c.identify.cns = cns;
	c.identify.prp1 = (unsigned long)addr;

	len -= (page_size - offset);
	if (len <= 0) {
		c.identify.prp2 = 0;
	} else {
		addr += (page_size - offset);
		c.identify.prp2 = (unsigned long)addr;
	}

	return nvme_submit_admin_cmd(ndev, &c, NULL);
}

static int nvme_ns_info(struct nvme_device *ndev, int i)
{
	struct blk_device *bdev;
	struct nvme_ns *ns;
	struct nvme_id_ns *id;
	unsigned long dma_addr;
	int ret;
	char name[64];

	id = mm_alloc_align(sizeof(struct nvme_id_ns), ndev->page_size);
	if (!id)
		return -1;
	memset((char *)id, 0, sizeof(struct nvme_id_ns));
	dma_mapping(ndev->dev, (unsigned long)id, &dma_addr,
		    sizeof(struct nvme_id_ns), NULL);

	ret = nvme_identify(ndev, i, 0, (void *)dma_addr);
	if (ret) {
		print("nvme_identify fail!!\n");
		return ret;
	}

	if (!id->nsze)
		return 0;

	ns = (struct nvme_ns *)mm_alloc(sizeof(struct nvme_ns));
	if (!ns)
		return -1;

	ns->flbas = id->flbas & NVME_NS_FLBAS_LBA_MASK;
	ns->lba_shift = id->lbaf[ns->flbas].ds;
	ns->ns_id = i;
	ns->ndev = ndev;
	list_add_tail(&ns->list, &ndev->nss);

	print("nvme: nvme0n%d lba_shift:%d\n", i, ns->lba_shift);
	sprintf(name, "nvme0n%d", i);

	bdev = nvme_blk_create_device(name, ns);
	if (!bdev) {
		print("nvme: nvme create blk device fail\n");
		return -1;
	}

	nvme_parse_partition(bdev, 1 << ns->lba_shift);

	return 0;
}

static int nvme_scan_namespace(struct nvme_device *ndev)
{
	int i;
	struct nvme_id_ctrl *ctrl;
	unsigned long dma_addr;

	ctrl = mm_alloc_align(sizeof(struct nvme_id_ctrl), ndev->page_size);
	if (!ctrl)
		return -1;
	memset((char *)ctrl, 0, sizeof(struct nvme_id_ctrl));
	dma_mapping(ndev->dev, (unsigned long)ctrl, &dma_addr,
		    sizeof(struct nvme_id_ctrl), NULL);

	nvme_identify(ndev, 0, 1, (void *)dma_addr);
	ndev->max_transfer_shift = ctrl->mdts;
	print("vid: %d ssvid:%d nn:%d sn:%s mdts:%d\n", ctrl->vid, ctrl->ssvid, ctrl->nn, ctrl->sn, ctrl->mdts);

	for (i = 1; i <= ctrl->nn; i++)
		nvme_ns_info(ndev, i);

	return 0;
}

static int nvme_set_queue_count(struct nvme_device *ndev, int count)
{
	int status;
	int res;
	unsigned int q_count = (count - 1) | ((count - 1) << 16);
	struct nvme_queue **q;

	q = (struct nvme_queue **)mm_alloc(sizeof(unsigned long) * count);
	if (!q)
		return -1;
	ndev->io_q = q;
	ndev->io_queue_count = count;

	status = nvme_set_features(ndev, NVME_FEAT_NUM_QUEUES, q_count, 0, &res);
	if (status < 0)
		return status;
	if (status > 1)
		return 0;

	return min(res & 0xffff, res >> 16) + 1;
}

static int nvme_setup_io_queues(struct nvme_device *ndev)
{
	int nr_io_queues;

	nr_io_queues = 1;
	if (nvme_set_queue_count(ndev, nr_io_queues) <= 0)
		return -1;

	if (nvme_create_io_queues(ndev))
		return -1;

	return 0;
}

static int nvme_setup_prp_pool(struct nvme_device *ndev)
{
	ndev->prp_pool = (unsigned long *)dma_alloc(ndev->dev,
						    &ndev->prp_pool_dma_addr,
						    sizeof(unsigned long) * MAX_PRP_POOL,
						    NULL);
	if (!ndev->prp_pool)
		return -1;

	ndev->prp_entry_count = MAX_PRP_POOL;

	return 0;
}

static int nvme_setup_prp2(struct nvme_device *ndev, unsigned long *prp2,
			   int count, unsigned long addr)
{
	unsigned long dma_addr;
	unsigned long *prp_pool = ndev->prp_pool;
	int offset = addr & (ndev->page_size - 1);
	int len = count, nr_pages;

	len -= ndev->page_size - offset;

	if (len <= 0) {
		*prp2 = 0;
		return 0;
	}

	dma_addr = addr + (ndev->page_size - offset);

	if (len <= ndev->page_size) {
		*prp2 = dma_addr;
		return 0;
	}

	nr_pages = (len % ndev->page_size) == 0 ?
			(len / ndev->page_size) : (len / ndev->page_size + 1);

	if (nr_pages > ndev->prp_entry_count) {
		int len;
		mm_free((void *)ndev->prp_pool, sizeof(unsigned long) * MAX_PRP_POOL);
		len = nr_pages * ndev->page_size;
		ndev->prp_pool = (unsigned long *)dma_alloc(ndev->dev,
							    &ndev->prp_pool_dma_addr,
							    len,
							    NULL);
		if (!ndev->prp_pool)
			return -1;
	}

	while (nr_pages) {
		*prp_pool++ = dma_addr;
		dma_addr += ndev->page_size;
		nr_pages--;
	}
	*prp2 = (unsigned long)ndev->prp_pool_dma_addr;

	return 0;
}

int nvme_blk_rw(struct nvme_device *ndev, struct nvme_ns *ns,
		unsigned long blknr, unsigned int count,
		void *buffer, int rw)
{
	struct nvme_command c = { };
	unsigned long start_lba = blknr, prp2, dma_addr;
	unsigned int total_lbas = count;

	if (rw == BLK_READ)
		c.rw.opcode = nvme_cmd_read;
	else if (rw == BLK_WRITE)
		c.rw.opcode = nvme_cmd_write;
	else
		return -1;

	c.rw.flags = 0;
	c.rw.nsid = ns->ns_id;
	c.rw.control = 0;
	c.rw.dsmgmt = 0;
	c.rw.reftag = 0;
	c.rw.apptag = 0;
	c.rw.appmask = 0;
	c.rw.metadata = 0;

	if (dma_mapping(ndev->dev, (unsigned long)buffer, &dma_addr,
			(1 << ns->lba_shift) * count, NULL)) {
		print("nvme: blk rw dma_mapping fail\n");
		return -1;
	}

	spin_lock(&ndev->lock);
	while (total_lbas) {
		int lbas;
		if (total_lbas < ndev->max_transfer_shift)
			lbas = total_lbas;
		else
			lbas = ndev->max_transfer_shift;

		if (nvme_setup_prp2(ndev, &prp2, lbas << ns->lba_shift, dma_addr)) {
			print("nvme: setup prp2 fail\n");
			spin_unlock(&ndev->lock);
			return -1;
		}

		c.rw.slba = start_lba;
		c.rw.length = lbas - 1;
		c.rw.prp1 = dma_addr;
		c.rw.prp2 = prp2;
		if (nvme_submit_sync_cmd(ndev->io_q[0], &c, NULL, 6000))
			break;

		start_lba += lbas;
		dma_addr += lbas << ns->lba_shift;
		total_lbas -= lbas;
	}
	spin_unlock(&ndev->lock);

	return total_lbas;
}

int nvme_init(struct nvme_device *ndev)
{
	INIT_LIST_HEAD(&ndev->nss);
	__SPINLOCK_INIT(&ndev->lock);
	ndev->cap = readq(ndev->base + NVME_MMIO_CAP);
	ndev->q_depth = NVME_Q_DEPTH;//NVME_CAP_MQES(ndev->cap) + 1;
	ndev->doorbell_stride = 1 << NVME_CAP_STRIDE(ndev->cap);
	ndev->dbs = (unsigned int *)(ndev->base + NVME_MMIO_DB);

	print("nvme: cap:0x%lx q_depth:%d stride:%d\n", ndev->cap, ndev->q_depth, ndev->doorbell_stride);

	if (NVME_CAP_CMBS(ndev->cap))
		writel(ndev->base + NVME_MMIO_CMBMSC, NVME_CMBMSC_CRE);

	if (nvme_configure_admin_queue(ndev)) {
		print("nvme: nvme_setup_configure fail\n");
		return -1;
	}

	if (nvme_setup_prp_pool(ndev)) {
		print("nvme: setup prp pool fail\n");
		return -1;
	}

	if (nvme_setup_io_queues(ndev)) {
		print("nvme: setup io queue fail\n");
		return -1;
	}

	if (nvme_scan_namespace(ndev)) {
		print("nvme: scan namespace fail\n");
		return -1;
	}

	return 0;
}
