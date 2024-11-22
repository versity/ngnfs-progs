/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Tell the whole file system to shutdown.
 */

#include "shared/block.h"
#include "shared/fs_info.h"
#include "shared/map.h"
#include "shared/shutdown.h"
#include "shared/thread.h"

bool ngnfs_should_shutdown(struct ngnfs_fs_info *nfi) {
	return nfi->shutdown;
}

void ngnfs_shutdown(struct ngnfs_fs_info *nfi, int err)
{
	if (err != 0)
		nfi->global_errno = err;
	nfi->shutdown = true;

	/*
	 * XXX We can't start ngnfs_*_destroy() concurrently with fs
	 * operations yet so we must wake any client threads waiting on
	 * map requests or block io. The fix is to implement the full
	 * prepare/shutdown/destroy sequence for all the subsystems.
	 */
	ngnfs_map_client_shutdown(nfi);
	ngnfs_block_shutdown(nfi);
	thread_shutdown_all();
}
