/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_PFS_H
#define NGNFS_SHARED_PFS_H

#include "shared/format-block.h"
#include "shared/lk/time64.h"
#include "shared/txn.h"

int ngnfs_pfs_mkfs(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn,
		   u64 root_ino, u64 nsec);
int ngnfs_pfs_read_inode(struct ngnfs_fs_info *nfi, struct ngnfs_transaction *txn, u64 ino,
			 struct ngnfs_inode *ninode, size_t size);

#endif
