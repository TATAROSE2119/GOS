#include "asm/type.h"
#include "fs.h"
#include "string.h"
#include "list.h"
#include "print.h"
#include "gos.h"
#include "block.h"

static struct dir_entry *init_fs_root = NULL;

struct dir_entry *get_init_fs_root(void)
{
	return init_fs_root;
}

int create_init_fs_blk(void)
{
	extern const char init_fs[];
	extern unsigned long __init_fs_start;
	extern unsigned long __init_fs_end;
	void *start = &__init_fs_start;
	void *end = &__init_fs_end;

	if (blk_create_ram_device("init-ramfs", (void *)init_fs,
				  end - start)) {
		print("init-fs: create ram device fail\n");
		return -1;
	}

	return 0;
}

void mount_init_fs(void)
{
	int id;

	id = load_fs(FS_TYPE_EXT4, get_blk_device("init-ramfs"));
	if (id == -1)
		return;

	init_fs_root = enter_fs(id);
}

void change_init_fs(char *name)
{
	int id;

	id = load_fs(FS_TYPE_EXT4, get_blk_device(name));
	if (id == -1)
		return;

	init_fs_root = enter_fs(id);
}
