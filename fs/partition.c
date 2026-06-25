#include "print.h"
#include "string.h"
#include "fs.h"
#include "block.h"

struct partition_entry {
	unsigned char boot_idicator;
	unsigned char s_head;
	unsigned char s_sector:6;
	unsigned char s_cylinder_hi:2;
	unsigned char s_cylinder;
	unsigned char sys_id;
	unsigned char e_head;
	unsigned char e_sector:6;
	unsigned char e_cylinder_hi:2;
	unsigned char e_cylinder;
	unsigned int relative_sector;
	unsigned int total_sectors;
} __attribute__((packed));

struct mbr {
	unsigned char bootstrap_code[440];
	unsigned int unique_mbr_disk_sig;
	unsigned short resv;
	struct partition_entry partition_table[4];
	unsigned short sig;
} __attribute__((packed));

static int is_mbr(struct blk_device *bdev, int section_size)
{
	unsigned short signature;

	if (blk_read(bdev, section_size - 2, (char *)&signature, 2))
		return 0;

	return (signature == 0xAA55);
}

static int is_gpt(struct blk_device *bdev, int section_size)
{
	int len;
	char *sig;

	sig = blk_get_buffer(bdev, section_size, section_size, &len);
	if (!sig)
		return 0;

	if (!strncmp(sig, "EFI PART", sizeof("EFI PART") - 1))
		return 1;

	return 0;
}

static void mbr_parse_partition(struct blk_device *bdev,
				void (*cb)(unsigned long sec_offset, int sec_count,
					   int id, void *priv),
				void *priv)
{
	struct mbr *mbr;
	int ret_len, i;

	if (!cb)
		return;

	mbr = blk_get_buffer(bdev, 0, sizeof(mbr), &ret_len);
	if (!mbr || (ret_len != sizeof(mbr))) {
		print("partition: %s fail\n", __FUNCTION__);
		return;
	}
	for (i = 0; i < 4; i++) {
		struct partition_entry *entry = &mbr->partition_table[i];
		if (entry->sys_id == 0)
			continue;
#if 0
		print("p%d\n", i + 1);
		print("  Start:   %d\n", entry->relative_sector);
		print("  End:     %d\n", entry->relative_sector + entry->total_sectors - 1);
		print("  Sectors: %d\n", entry->total_sectors);
		print("  Size:    0x%lx\n", entry->total_sectors * 512);
#endif
		cb(entry->relative_sector, entry->total_sectors, i + 1, priv);
	}
}

static void gpt_parse_partition(struct blk_device *bdev,
				void (*cb)(unsigned long sec_offset, int sec_count,
					   int id, void *pirv),
				void *priv)
{
	print("@@@@@@@@ %s \n", __FUNCTION__);

}

static struct fs_partition_type fs_partitions[] = {
	{ "MBR", is_mbr, mbr_parse_partition },
	{ "GPT", is_gpt, gpt_parse_partition },
};
#define FS_PARTITION_TYPE_CNT (sizeof(fs_partitions) / sizeof(fs_partitions[0]))

int fs_parse_partition(struct blk_device *bdev, int section_size,
		       void (*cb)(unsigned long sec_offset, int sec_count,
				  int id, void *priv),
		       void *priv)
{
	int i;

	for (i = 0; i < FS_PARTITION_TYPE_CNT; i++) {
		if (fs_partitions[i].is_type(bdev, section_size)) {
			fs_partitions[i].parse_partition(bdev, cb, priv);
			break;
		}
	}

	return 0;
}
