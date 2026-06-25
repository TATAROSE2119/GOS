#ifndef __NVME_H__
#define __NVME_H__

#include "list.h"
#include "spinlocks.h"

#define NVME_MMIO_CAP     0x0000
#define NVME_MMIO_VS      0x0008
#define NVME_MMIO_INTMS   0x000C
#define NVME_MMIO_INTMC   0x0010
#define NVME_MMIO_CC      0x0014
#define NVME_MMIO_CSTS    0x001C
#define NVME_MMIO_AQA     0x0024
#define NVME_MMIO_ASQ     0x0028
#define NVME_MMIO_ACQ     0x0030
#define NVME_MMIO_CMBLOC  0x0038
#define NVME_MMIO_CMBSZ   0x003C
#define NVME_MMIO_BPINFO  0x0040
#define NVME_MMIO_BPRSEL  0x0044
#define NVME_MMIO_BPMBK   0x0048
#define NVME_MMIO_CMBMSC  0x0050
#define NVME_MMIO_CRTO    0x0068
#define NVME_MMIO_PMRCAP  0x0E00
#define NVME_MMIO_PMRCTL  0x0E04
#define NVME_MMIO_PMRSTS  0x0E08
#define NVME_MMIO_PMREBS  0x0E0C
#define NVME_MMIO_PMRSWTP 0x0E10
#define NVME_MMIO_DB      0x1000

#define NVME_CAP_MQES(cap)	((cap) & 0xffffUL)
#define NVME_CAP_TIMEOUT(cap)	(((cap) >> 24) & 0xff)
#define NVME_CAP_STRIDE(cap)	(((cap) >> 32) & 0xf)
#define NVME_CAP_NSSRC(cap)	(((cap) >> 36) & 0x1)
#define NVME_CAP_CSS(cap)	(((cap) >> 37) & 0xff)
#define NVME_CAP_MPSMIN(cap)	(((cap) >> 48) & 0xf)
#define NVME_CAP_MPSMAX(cap)	(((cap) >> 52) & 0xf)
#define NVME_CAP_CMBS(cap)	(((cap) >> 57) & 0x1)

#define NVME_VS(major, minor, tertiary) \
	(((major) << 16) | ((minor) << 8) | (tertiary))

#define NVME_ADM_SQES           6
#define NVME_NVM_IOSQES		6
#define NVME_NVM_IOCQES		4

enum {
	NVME_CC_ENABLE		= 1 << 0,
	NVME_CC_EN_SHIFT	= 0,
	NVME_CC_CSS_SHIFT	= 4,
	NVME_CC_MPS_SHIFT	= 7,
	NVME_CC_AMS_SHIFT	= 11,
	NVME_CC_SHN_SHIFT	= 14,
	NVME_CC_IOSQES_SHIFT	= 16,
	NVME_CC_IOCQES_SHIFT	= 20,
	NVME_CC_CSS_NVM		= 0 << NVME_CC_CSS_SHIFT,
	NVME_CC_CSS_CSI		= 6 << NVME_CC_CSS_SHIFT,
	NVME_CC_CSS_MASK	= 7 << NVME_CC_CSS_SHIFT,
	NVME_CC_AMS_RR		= 0 << NVME_CC_AMS_SHIFT,
	NVME_CC_AMS_WRRU	= 1 << NVME_CC_AMS_SHIFT,
	NVME_CC_AMS_VS		= 7 << NVME_CC_AMS_SHIFT,
	NVME_CC_SHN_NONE	= 0 << NVME_CC_SHN_SHIFT,
	NVME_CC_SHN_NORMAL	= 1 << NVME_CC_SHN_SHIFT,
	NVME_CC_SHN_ABRUPT	= 2 << NVME_CC_SHN_SHIFT,
	NVME_CC_SHN_MASK	= 3 << NVME_CC_SHN_SHIFT,
	NVME_CC_IOSQES		= NVME_NVM_IOSQES << NVME_CC_IOSQES_SHIFT,
	NVME_CC_IOCQES		= NVME_NVM_IOCQES << NVME_CC_IOCQES_SHIFT,
	NVME_CC_CRIME		= 1 << 24,
};

enum {
	NVME_CSTS_RDY		= 1 << 0,
	NVME_CSTS_CFS		= 1 << 1,
	NVME_CSTS_NSSRO		= 1 << 4,
	NVME_CSTS_PP		= 1 << 5,
	NVME_CSTS_SHST_NORMAL	= 0 << 2,
	NVME_CSTS_SHST_OCCUR	= 1 << 2,
	NVME_CSTS_SHST_CMPLT	= 2 << 2,
	NVME_CSTS_SHST_MASK	= 3 << 2,
};

enum {
	NVME_CMBMSC_CRE		= 1 << 0,
	NVME_CMBMSC_CMSE	= 1 << 1,
};

enum {
	NVME_CAP_CSS_NVM	= 1 << 0,
	NVME_CAP_CSS_CSI	= 1 << 6,
};

enum {
	NVME_CAP_CRMS_CRWMS	= 1ULL << 59,
	NVME_CAP_CRMS_CRIMS	= 1ULL << 60,
};

enum nvme_opcode {
	nvme_cmd_flush		= 0x00,
	nvme_cmd_write		= 0x01,
	nvme_cmd_read		= 0x02,
	nvme_cmd_write_uncor	= 0x04,
	nvme_cmd_compare	= 0x05,
	nvme_cmd_write_zeroes	= 0x08,
	nvme_cmd_dsm		= 0x09,
	nvme_cmd_resv_register	= 0x0d,
	nvme_cmd_resv_report	= 0x0e,
	nvme_cmd_resv_acquire	= 0x11,
	nvme_cmd_resv_release	= 0x15,
};

enum nvme_admin_opcode {
	nvme_admin_delete_sq		= 0x00,
	nvme_admin_create_sq		= 0x01,
	nvme_admin_get_log_page		= 0x02,
	nvme_admin_delete_cq		= 0x04,
	nvme_admin_create_cq		= 0x05,
	nvme_admin_identify		= 0x06,
	nvme_admin_abort_cmd		= 0x08,
	nvme_admin_set_features		= 0x09,
	nvme_admin_get_features		= 0x0a,
	nvme_admin_async_event		= 0x0c,
	nvme_admin_activate_fw		= 0x10,
	nvme_admin_download_fw		= 0x11,
	nvme_admin_format_nvm		= 0x80,
	nvme_admin_security_send	= 0x81,
	nvme_admin_security_recv	= 0x82,
};

enum {
	NVME_QUEUE_PHYS_CONFIG	= (1 << 0),
	NVME_CQ_IRQ_ENABLED	= (1 << 1),
	NVME_SQ_PRIO_URGENT	= (0 << 1),
	NVME_SQ_PRIO_HIGH	= (1 << 1),
	NVME_SQ_PRIO_MEDIUM	= (2 << 1),
	NVME_SQ_PRIO_LOW	= (3 << 1),
	NVME_FEAT_ARBITRATION	= 0x01,
	NVME_FEAT_POWER_MGMT	= 0x02,
	NVME_FEAT_LBA_RANGE	= 0x03,
	NVME_FEAT_TEMP_THRESH	= 0x04,
	NVME_FEAT_ERR_RECOVERY	= 0x05,
	NVME_FEAT_VOLATILE_WC	= 0x06,
	NVME_FEAT_NUM_QUEUES	= 0x07,
	NVME_FEAT_IRQ_COALESCE	= 0x08,
	NVME_FEAT_IRQ_CONFIG	= 0x09,
	NVME_FEAT_WRITE_ATOMIC	= 0x0a,
	NVME_FEAT_ASYNC_EVENT	= 0x0b,
	NVME_FEAT_AUTO_PST	= 0x0c,
	NVME_FEAT_SW_PROGRESS	= 0x80,
	NVME_FEAT_HOST_ID	= 0x81,
	NVME_FEAT_RESV_MASK	= 0x82,
	NVME_FEAT_RESV_PERSIST	= 0x83,
	NVME_LOG_ERROR		= 0x01,
	NVME_LOG_SMART		= 0x02,
	NVME_LOG_FW_SLOT	= 0x03,
	NVME_LOG_RESERVATION	= 0x80,
	NVME_FWACT_REPL		= (0 << 3),
	NVME_FWACT_REPL_ACTV	= (1 << 3),
	NVME_FWACT_ACTV		= (2 << 3),
};

enum {
	NVME_NS_FEAT_THIN	= 1 << 0,
	NVME_NS_FLBAS_LBA_MASK	= 0xf,
	NVME_NS_FLBAS_META_EXT	= 0x10,
	NVME_LBAF_RP_BEST	= 0,
	NVME_LBAF_RP_BETTER	= 1,
	NVME_LBAF_RP_GOOD	= 2,
	NVME_LBAF_RP_DEGRADED	= 3,
	NVME_NS_DPC_PI_LAST	= 1 << 4,
	NVME_NS_DPC_PI_FIRST	= 1 << 3,
	NVME_NS_DPC_PI_TYPE3	= 1 << 2,
	NVME_NS_DPC_PI_TYPE2	= 1 << 1,
	NVME_NS_DPC_PI_TYPE1	= 1 << 0,
	NVME_NS_DPS_PI_FIRST	= 1 << 3,
	NVME_NS_DPS_PI_MASK	= 0x7,
	NVME_NS_DPS_PI_TYPE1	= 1,
	NVME_NS_DPS_PI_TYPE2	= 2,
	NVME_NS_DPS_PI_TYPE3	= 3,
};

struct nvme_id_power_state {
	unsigned short			max_power;	/* centiwatts */
	unsigned char			rsvd2;
	unsigned char			flags;
	unsigned int			entry_lat;	/* microseconds */
	unsigned int			exit_lat;	/* microseconds */
	unsigned char			read_tput;
	unsigned char			read_lat;
	unsigned char			write_tput;
	unsigned char			write_lat;
	unsigned short			idle_power;
	unsigned char			idle_scale;
	unsigned char			rsvd19;
	unsigned short			active_power;
	unsigned char			active_work_scale;
	unsigned char			rsvd23[9];
};

struct nvme_id_ctrl {
	unsigned short			vid;
	unsigned short			ssvid;
	char				sn[20];
	char				mn[40];
	char				fr[8];
	unsigned char			rab;
	unsigned char			ieee[3];
	unsigned char			mic;
	unsigned char			mdts;
	unsigned short			cntlid;
	unsigned int			ver;
	unsigned char			rsvd84[172];
	unsigned short			oacs;
	unsigned char			acl;
	unsigned char			aerl;
	unsigned char			frmw;
	unsigned char			lpa;
	unsigned char			elpe;
	unsigned char			npss;
	unsigned char			avscc;
	unsigned char			apsta;
	unsigned short			wctemp;
	unsigned short			cctemp;
	unsigned char			rsvd270[242];
	unsigned char			sqes;
	unsigned char			cqes;
	unsigned char			rsvd514[2];
	unsigned int			nn;
	unsigned short			oncs;
	unsigned short			fuses;
	unsigned char			fna;
	unsigned char			vwc;
	unsigned short			awun;
	unsigned short			awupf;
	unsigned char			nvscc;
	unsigned char			rsvd531;
	unsigned short			acwu;
	unsigned char			rsvd534[2];
	unsigned int			sgls;
	unsigned char			rsvd540[1508];
	struct nvme_id_power_state	psd[32];
	unsigned char			vs[1024];
};

struct nvme_lbaf {
	unsigned short			ms;
	unsigned char			ds;
	unsigned char			rp;
};

struct nvme_id_ns {
	unsigned long			nsze;
	unsigned long			ncap;
	unsigned long			nuse;
	unsigned char			nsfeat;
	unsigned char			nlbaf;
	unsigned char			flbas;
	unsigned char			mc;
	unsigned char			dpc;
	unsigned char			dps;
	unsigned char			nmic;
	unsigned char			rescap;
	unsigned char			fpi;
	unsigned char			rsvd33;
	unsigned short			nawun;
	unsigned short			nawupf;
	unsigned short			nacwu;
	unsigned short			nabsn;
	unsigned short			nabo;
	unsigned short			nabspf;
	unsigned short			rsvd46;
	unsigned long			nvmcap[2];
	unsigned char			rsvd64[40];
	unsigned char			nguid[16];
	unsigned char			eui64[8];
	struct nvme_lbaf	lbaf[16];
	unsigned char			rsvd192[192];
	unsigned char			vs[3712];
};

struct nvme_common_command {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			nsid;
	unsigned int			cdw2[2];
	unsigned long			metadata;
	unsigned long			prp1;
	unsigned long			prp2;
	unsigned int			cdw10[6];
};

struct nvme_rw_command {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			nsid;
	unsigned long			rsvd2;
	unsigned long			metadata;
	unsigned long			prp1;
	unsigned long			prp2;
	unsigned long			slba;
	unsigned short			length;
	unsigned short			control;
	unsigned int			dsmgmt;
	unsigned int			reftag;
	unsigned short			apptag;
	unsigned short			appmask;
};

struct nvme_identify {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			nsid;
	unsigned long			rsvd2[2];
	unsigned long			prp1;
	unsigned long			prp2;
	unsigned int			cns;
	unsigned int			rsvd11[5];
};

struct nvme_features {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			nsid;
	unsigned long			rsvd2[2];
	unsigned long			prp1;
	unsigned long			prp2;
	unsigned int			fid;
	unsigned int			dword11;
	unsigned int			rsvd12[4];
};

struct nvme_create_cq {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			rsvd1[5];
	unsigned long			prp1;
	unsigned long			rsvd8;
	unsigned short			cqid;
	unsigned short			qsize;
	unsigned short			cq_flags;
	unsigned short			irq_vector;
	unsigned int			rsvd12[4];
};

struct nvme_create_sq {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			rsvd1[5];
	unsigned long			prp1;
	unsigned long			rsvd8;
	unsigned short			sqid;
	unsigned short			qsize;
	unsigned short			sq_flags;
	unsigned short			cqid;
	unsigned int			rsvd12[4];
};

struct nvme_delete_queue {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			rsvd1[9];
	unsigned short			qid;
	unsigned short			rsvd10;
	unsigned int			rsvd11[5];
};

struct nvme_abort_cmd {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			rsvd1[9];
	unsigned short			sqid;
	unsigned short			cid;
	unsigned int			rsvd11[5];
};

struct nvme_download_firmware {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			rsvd1[5];
	unsigned long			prp1;
	unsigned long			prp2;
	unsigned int			numd;
	unsigned int			offset;
	unsigned int			rsvd12[4];
};

struct nvme_dsm_cmd {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			nsid;
	unsigned long			rsvd2[2];
	unsigned long			prp1;
	unsigned long			prp2;
	unsigned int			nr;
	unsigned int			attributes;
	unsigned int			rsvd12[4];
};

struct nvme_format_cmd {
	unsigned char			opcode;
	unsigned char			flags;
	unsigned short			command_id;
	unsigned int			nsid;
	unsigned long			rsvd2[4];
	unsigned int			cdw10;
	unsigned int			rsvd11[5];
};

struct nvme_command {
	union {
		struct nvme_common_command common;
		struct nvme_rw_command rw;
		struct nvme_identify identify;
		struct nvme_features features;
		struct nvme_create_cq create_cq;
		struct nvme_create_sq create_sq;
		struct nvme_delete_queue delete_queue;
		struct nvme_download_firmware dlfw;
		struct nvme_format_cmd format;
		struct nvme_dsm_cmd dsm;
		struct nvme_abort_cmd abort;
	};
};

struct nvme_completion {
	unsigned int	result;
	unsigned int	rsvd;
	unsigned short	sq_head;
	unsigned short	sq_id;
	unsigned short	command_id;
	unsigned short	status;
};

struct nvme_queue {
	int id;
	struct nvme_device     *dev;
	struct nvme_command    *submit_q;
	unsigned long sq_dma_addr;
	struct nvme_completion *complete_q;
	unsigned long cq_dma_addr;
	int q_depth;
	int sq_head;
	int sq_tail;
	int cq_head;
	int cq_phase;
	int irq_vector;
	unsigned int *q_db;
};

struct nvme_ns {
	struct list_head list;
	int ns_id;
	int lba_shift;
	int flbas;
	struct nvme_device *ndev;
};

struct nvme_device {
	struct device *dev;
	unsigned long base;
	unsigned long cap;
	int q_depth;
	int page_size;
	int subsystem;
	unsigned int ctrl_config;
	int doorbell_stride;
	struct nvme_queue *admin_q;
	struct nvme_queue **io_q;
	int io_queue_count;
	int max_transfer_shift;
	unsigned int *dbs;
	struct list_head nss;
	unsigned long *prp_pool;
	unsigned long prp_pool_dma_addr;
	int prp_entry_count;
	spinlock_t lock;
};

int nvme_blk_rw(struct nvme_device *ndev, struct nvme_ns *ns,
		unsigned long blknr, unsigned int count,
		void *buffer, int rw);
int nvme_init(struct nvme_device *ndev);

#endif
