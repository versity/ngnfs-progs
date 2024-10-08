/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/errno.h"
#include "shared/lk/list.h"
#include "shared/lk/in.h"
#include "shared/lk/math64.h"
#include "shared/lk/slab.h"
#include "shared/lk/stddef.h"
#include "shared/lk/string.h"
#include "shared/lk/types.h"

#include "shared/fs_info.h"
#include "shared/manifest.h"

struct ngnfs_manifest_info {
	u8 nr_addrs;
	struct sockaddr_in addrs[];
};

int ngnfs_manifest_map_block(struct ngnfs_fs_info *nfi, u64 bnr, struct sockaddr_in *addr)
{
	struct ngnfs_manifest_info *mfinf = nfi->manifest_info;
	u32 rem;

	div_u64_rem(bnr, mfinf->nr_addrs, &rem);
	*addr = mfinf->addrs[rem];

	return 0;
}

/*
 * Just a u8 to limit the largest possible allocation.
 *
 * It's surprisingly ok to have duplicate addresses in the array
 * currently because we're not actually mapping the fs scoped block
 * numbers to device block numbers.  Each device must be able to store
 * the entire block space.
 */
int ngnfs_manifest_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr)
{
	struct ngnfs_manifest_addr_head *ahead;
	struct ngnfs_manifest_info *mfinf;
	struct sockaddr_in *addr;
	int ret;

	mfinf = kmalloc(offsetof(struct ngnfs_manifest_info, addrs[nr]), GFP_NOFS);
	if (!mfinf) {
		ret = -ENOMEM;
		goto out;
	}

	mfinf->nr_addrs = nr;

	addr = &mfinf->addrs[0];
	list_for_each_entry(ahead, list, head) {
		if (nr-- == 0) {
			ret = -EINVAL;
			goto out;
		}

		*addr = ahead->addr;
		addr++;
	}

	if (nr != 0) {
		ret = -EINVAL;
		goto out;
	}

	nfi->manifest_info = mfinf;
	ret = 0;
out:
	if (ret < 0)
		kfree(mfinf);
	return ret;
}

void ngnfs_manifest_destroy(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_manifest_info *mfinf = nfi->manifest_info;

	if (mfinf) {
		kfree(mfinf);
		nfi->manifest_info = NULL;
	}
}
