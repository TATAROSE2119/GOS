#ifndef __FS_H__
#define __FS_H__

#include "asm/type.h"
#include "list.h"
#include "spinlocks.h"
#include "block.h"

#define FS_INIT_TABLE     __fs_init_table
#define FS_INIT_TABLE_END __fs_init_table_end

struct fs_partition_type {
	char name[16];
	int (*is_type)(struct blk_device *bdev, int section_size);
	void (*parse_partition)(struct blk_device *bdev,
				void (*cb)(unsigned long sec_offset, int sec_count,
					   int id, void *priv),
				void *priv);
};

struct fs_init_entry {
	int type;
	void (*init)(void);
};

#define FS_REGISTER(name, init_fn)                        \
	static const struct fs_init_entry __attribute__((used)) \
		fs_init_##name                                  \
		__attribute__((section(".fs_init_table"))) = {  \
			.init = init_fn,                        \
		}

enum {
	FS_TYPE_EXT4,
};

struct fs_type {
	struct list_head list;
	u8 type;
	struct fs_type_ops *ops;
};

struct fs_node;

struct file_entry {
	struct list_head list;
	char name[64];
	void *data;
	struct fs_node *node;
};

struct dir_entry {
	struct list_head list;
	char name[64];
	struct list_head child_entry;
	struct list_head file_entry;
	struct dir_entry *parent;
};

struct fs_node {
	struct list_head list;
	struct blk_device *bdev;
	int id;
	struct fs_type *fs;
	void *base;
	void *priv_data;
	struct dir_entry *root_entry;
};

struct fs_type_ops {
	void (*print_fs_info)(struct fs_node * node);
	int (*parse_meta)(struct fs_node * node);
	int (*parse_dir)(struct fs_node * node, char *name);
	int (*load_file_content)(struct file_entry * entry,
				 int(*parse_block_content)(unsigned long start,
							   unsigned int len,
							   void *priv),
				 void *priv);
};

struct file_buffer {
	struct list_head list;
	void *buffer;
	int len;
	int pos;
};

struct file {
	char name[128];
	struct list_head list;
	struct file_entry *file_e;
	struct list_head buffer;
	spinlock_t lock;
	int pos;
	int end_pos;
};

enum {
	SEEK_SET = 0,
	SEEK_CUR,
	SEEK_END,
};

struct dir_entry *enter_fs(int id);

int fs_parse_partition(struct blk_device *bdev, int section_size,
		       void (*cb)(unsigned long sec_offset, int sec_count,
				  int id, void *priv),
		       void *priv);
int load_file(struct file_entry *entry,
	      int (*parse_block_content)(unsigned long start,
					 unsigned int len,
					 void *priv),
	      void *priv);
void ls_all_fs_node(int print_info);
void mount_all_fs(void);
int load_fs(u8 type, struct blk_device *bdev);
int create_init_fs_blk(void);
void mount_init_fs(void);
void change_init_fs(char *name);
void fs_init(void);
int register_fs_type(struct fs_type *new);
struct dir_entry *get_init_fs_root(void);

void *fopen(const char *filename, const char *mode);
void fclose(void *handle);
int fread(void *handle, void *buf, int count);
int fseek(void *handle, unsigned long offset, int whence);
int ftell(void *handle);

#endif
