/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/ktime.h"

#include "shared/format-block.h"
#include "shared/fs_info.h"
#include "shared/inode.h"
#include "shared/mkfs.h"
#include "shared/txn.h"

/*
 * This only creates the inode for the root directory.  It will need to
 * initialize allocation blocks and that means needing to see the size
 * of the volume.
 */
int ngnfs_mkfs(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_inode_txn_ref itref;
	struct ngnfs_transaction txn;
	u64 nsec;
	int ret;

	ngnfs_txn_init(&txn);
	nsec = ktime_to_ns(ktime_get_real());

	do {
		ret = ngnfs_inode_get(nfi, &txn, NBF_WRITE, NGNFS_ROOT_INO, &itref)		?:
		      ngnfs_inode_init(&itref, NGNFS_ROOT_INO, 1, 1, 0755, nsec);

	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);

	return ret;
}
