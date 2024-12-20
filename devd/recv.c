/* SPDX-License-Identifier: GPL-2.0 */

/*
 * devd's handling of received messages.
 */

#include <string.h>
#include <errno.h>

#include "shared/block.h"
#include "shared/format-block.h"
#include "shared/format-msg.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/msg.h"
#include "shared/txn.h"

#include "devd/recv.h"

static int devd_get_block(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_get_block *gb = mdesc->ctl_buf;
	struct ngnfs_msg_get_block_result res;
	struct ngnfs_msg_desc res_mdesc;
	struct ngnfs_block *bl;
	int ret;

	if ((mdesc->ctl_size != sizeof(struct ngnfs_msg_get_block)) ||
	    (gb->access >= NGNFS_MSG_BLOCK_ACCESS__UNKNOWN) ||
	    (mdesc->data_size != 0))
		return -EINVAL;

	/* XXX there'd be fs bnr -> dev bnr mapping */
	/* XXX that'd catch invalid bnr's coming in? */

	bl = ngnfs_block_get(nfi, le64_to_cpu(gb->bnr), NBF_READ);
	if (IS_ERR(bl))
		ret = PTR_ERR(bl);
	else
		ret = 0;

	res.bnr = gb->bnr;
	res.access = gb->access;
	res.err = ngnfs_msg_err(ret);

	res_mdesc.type = NGNFS_MSG_GET_BLOCK_RESULT;
	res_mdesc.addr = mdesc->addr;
	res_mdesc.ctl_buf = &res;
	res_mdesc.ctl_size = sizeof(res);
	if (ret < 0) {
		res_mdesc.data_page = NULL;
		res_mdesc.data_size = 0;
	} else {
		res_mdesc.data_page = ngnfs_block_page(bl);
		res_mdesc.data_size = NGNFS_BLOCK_SIZE;
	}

	ret = ngnfs_msg_send(nfi, &res_mdesc);
	ngnfs_block_put(nfi, bl, NBF_READ);

	return ret;
}

/*
 * Write a block by dirtying it in the block cache and immediately
 * syncing.  This is temporary and will change quite a bit once devd
 * gets a more robust structure with journaled blocks and transactions.
 *
 * We're copying the block contents today, but we should be able to swap
 * data pages as long as we appropriately manage concurrent readers.
 */
static int write_block_sync(struct ngnfs_fs_info *nfi, u64 bnr, struct page *data_page)
{
	struct ngnfs_transaction txn;
	void *ptr;
	int ret;

	ngnfs_txn_init(&txn);

	do {
		ret = ngnfs_txn_get_block(nfi, &txn, bnr, NBF_NEW | NBF_WRITE, NULL, &ptr);
		if (ret == 0)
			memcpy(ptr, page_address(data_page), NGNFS_BLOCK_SIZE);
	} while (ngnfs_txn_retry(nfi, &txn, &ret));

	ngnfs_txn_teardown(nfi, &txn);

	if (ret == 0)
		ret = ngnfs_block_sync(nfi);

	return ret;
}

static int devd_write_block(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_transaction txn = INIT_NGNFS_TXN(txn);
	struct ngnfs_msg_write_block *wb = mdesc->ctl_buf;
	struct ngnfs_msg_write_block_result res;
	struct ngnfs_msg_desc res_mdesc;
	int ret;

	/* XXX errors that shutdown the session? */
	/* XXX verify more fields? */
	if (mdesc->ctl_size != sizeof(struct ngnfs_msg_write_block) ||
	    mdesc->data_size != NGNFS_BLOCK_SIZE) {
		ret = -EIO;
		goto out;
	}

	/* XXX there'd be fs bnr -> dev bnr mapping */
	ret = write_block_sync(nfi, le64_to_cpu(wb->bnr), mdesc->data_page);

	res.bnr = wb->bnr;
	res.err = ngnfs_msg_err(ret);

	res_mdesc.type = NGNFS_MSG_WRITE_BLOCK_RESULT;
	res_mdesc.addr = mdesc->addr;
	res_mdesc.ctl_buf = &res;
	res_mdesc.ctl_size = sizeof(res);
	res_mdesc.data_page = NULL;
	res_mdesc.data_size = 0;

	ret = ngnfs_msg_send(nfi, &res_mdesc);
out:
	return ret;
}

int devd_recv_setup(struct ngnfs_fs_info *nfi)
{
	return ngnfs_msg_register_recv(nfi, NGNFS_MSG_GET_BLOCK, devd_get_block) ?:
	       ngnfs_msg_register_recv(nfi, NGNFS_MSG_WRITE_BLOCK, devd_write_block);
}

void devd_recv_destroy(struct ngnfs_fs_info *nfi)
{
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_GET_BLOCK, devd_get_block);
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_WRITE_BLOCK, devd_write_block);
}
