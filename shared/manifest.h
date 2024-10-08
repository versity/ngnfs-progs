/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_MANIFEST_H
#define NGNFS_SHARED_MANIFEST_H

#include "shared/lk/in.h"
#include "shared/lk/list.h"

#include "shared/fs_info.h"

struct ngnfs_manifest_addr_head {
	struct list_head head;
	struct sockaddr_in addr;
};

int ngnfs_manifest_map_block(struct ngnfs_fs_info *nfi, u64 bnr, struct sockaddr_in *addr);
int ngnfs_manifest_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr);
void ngnfs_manifest_destroy(struct ngnfs_fs_info *nfi);

#endif
