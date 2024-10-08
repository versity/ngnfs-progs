/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/errno.h"
#include "shared/lk/gfp.h"

#include "shared/block.h"
#include "shared/btr-msg.h"
#include "shared/format-block.h"
#include "shared/fs_info.h"
#include "shared/map.h"
#include "shared/msg.h"

static int ngnfs_btr_msg_get_block_result(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_get_block_result *gbr = mdesc->ctl_buf;

	/*
	 * This may grow cases where it's fine to be granted write
	 * access to an existing block without a data payload because
	 * you indicated the intent to free it without reading it.
	 */
	if (mdesc->ctl_size != sizeof(struct ngnfs_msg_get_block_result) ||
	    ((gbr->err == NGNFS_MSG_ERR_OK) && (mdesc->data_size != NGNFS_BLOCK_SIZE)) ||
	    ((gbr->err != NGNFS_MSG_ERR_OK) && (mdesc->data_size != 0)))
		return -EINVAL;

	ngnfs_block_end_io(nfi, le64_to_cpu(gbr->bnr), mdesc->data_page, ngnfs_msg_errno(gbr->err));

	return 0;
}

static int ngnfs_btr_msg_write_block_result(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_write_block_result *wbr = mdesc->ctl_buf;

	if (mdesc->ctl_size != sizeof(struct ngnfs_msg_write_block_result) ||
	    mdesc->data_size != 0)
		return -EINVAL;

	ngnfs_block_end_io(nfi, le64_to_cpu(wbr->bnr), mdesc->data_page, ngnfs_msg_errno(wbr->err));

	return 0;
}

static int ngnfs_btr_msg_submit_block(struct ngnfs_fs_info *nfi, void *btr_info, int op, u64 bnr,
				      struct page *data_page)
{
	union {
		struct ngnfs_msg_get_block gb;
		struct ngnfs_msg_write_block wb;
	} u;
	struct ngnfs_msg_desc mdesc;
	struct sockaddr_in addr;
	int ret;

	switch (op) {
		case NGNFS_BTX_OP_GET_READ:
		case NGNFS_BTX_OP_GET_WRITE:
			u.gb.bnr = cpu_to_le64(bnr);
			u.gb.access = op == NGNFS_BTX_OP_GET_READ ? NGNFS_MSG_BLOCK_ACCESS_READ :
								    NGNFS_MSG_BLOCK_ACCESS_WRITE;
			mdesc.ctl_buf = &u.gb;
			mdesc.ctl_size = sizeof(u.gb);
			mdesc.data_page = NULL;
			mdesc.data_size = 0;
			mdesc.type = NGNFS_MSG_GET_BLOCK;
			break;

		case NGNFS_BTX_OP_WRITE:
			u.wb.bnr = cpu_to_le64(bnr);
			mdesc.ctl_buf = &u.wb;
			mdesc.ctl_size = sizeof(u.wb);
			mdesc.data_page = data_page;
			mdesc.data_size = NGNFS_BLOCK_SIZE;
			mdesc.type = NGNFS_MSG_WRITE_BLOCK;
			break;

		default:
			ret = -EOPNOTSUPP;
			goto out;
	}

	ret = ngnfs_map_map_block(nfi, bnr, &addr);
	if (ret == 0) {
		mdesc.addr = &addr;
		ret = ngnfs_msg_send(nfi, &mdesc);
	}
out:
	return ret;
}

static int ngnfs_btr_msg_queue_depth(struct ngnfs_fs_info *nfi, void *btr_info)
{
	return 32; /* XXX *shrug* */
}

static void *ngnfs_btr_msg_setup(struct ngnfs_fs_info *nfi, void *arg)
{
	int ret;

	ret = ngnfs_msg_register_recv(nfi, NGNFS_MSG_GET_BLOCK_RESULT,
				      ngnfs_btr_msg_get_block_result) ?:
	      ngnfs_msg_register_recv(nfi, NGNFS_MSG_WRITE_BLOCK_RESULT,
				      ngnfs_btr_msg_write_block_result);
	if (ret < 0)
		return ERR_PTR(ret);

	return NULL;
}

static void ngnfs_btr_msg_destroy(struct ngnfs_fs_info *nfi, void *btr_info)
{
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_GET_BLOCK_RESULT, ngnfs_btr_msg_get_block_result);
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_WRITE_BLOCK_RESULT,
				  ngnfs_btr_msg_write_block_result);
}

struct ngnfs_block_transport_ops ngnfs_btr_msg_ops = {
	.setup = ngnfs_btr_msg_setup,
	.destroy = ngnfs_btr_msg_destroy,
	.queue_depth = ngnfs_btr_msg_queue_depth,
	.submit_block = ngnfs_btr_msg_submit_block,
};
