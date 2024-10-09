/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_FS_INFO_H
#define NGNFS_SHARED_FS_INFO_H

/*
 * The _fs_info struct is the global system context reference.  Each layer has its
 * info per-system info stored here.
 */
struct ngnfs_block_info;
struct ngnfs_map_info;
struct ngnfs_msg_info;

struct ngnfs_fs_info {
	struct ngnfs_block_info *block_info;
	struct ngnfs_map_info *map_info;
	struct ngnfs_msg_info *msg_info;
};

#define INIT_NGNFS_FS_INFO { NULL, }

#endif
