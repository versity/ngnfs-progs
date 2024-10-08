/* SPDX-License-Identifier: GPL-2.0 */

#include <assert.h>

#include "shared/lk/bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/gfp.h"
#include "shared/lk/limits.h"
#include "shared/lk/list.h"
#include "shared/lk/math64.h"
#include "shared/lk/mutex.h"
#include "shared/lk/slab.h"
#include "shared/lk/types.h"
#include "shared/lk/wait.h"

#include "shared/format-block.h"
#include "shared/format-msg.h"
#include "shared/fs_info.h"
#include "shared/lk/wait.h"
#include "shared/log.h"
#include "shared/map.h"
#include "shared/msg.h"
#include "shared/parse.h"
#include "shared/urcu.h"

struct ngnfs_devd_addrs {
	u8 nr_addrs;
	struct sockaddr_in addrs[];
};

/*
 * TODO: make the manifest contents an array with functions to fill/free
 */
struct ngnfs_manifest_contents {
	u64 seq_nr;
	struct ngnfs_devd_addrs *devd_array;
};

struct ngnfs_manifest_info {
	struct wait_queue_head updates_waitq;
	struct mutex mutex;
	struct ngnfs_manifest_contents *contents;
};

/*
 * Structure for sending a manifest as a series of blocks via
 * ngnfs_msg_send. Currently we only send manifests that fit in a single block
 * but at some point it will be an array of blocks.
 */
struct manifest_msg {
	u64 seq_nr;
	struct page *page;
};

/* Parse the IPv4 addr:port in str and add it to addr_list. */
int ngnfs_manifest_append_addr(u8 *nr_addrs, struct list_head *addr_list, char *str)
{
	struct ngnfs_manifest_addr_head *ahead;
	int ret;

	if (*nr_addrs == U8_MAX) {
		log("too many -d addresses specified, exceeded limit of %u", U8_MAX);
		return -EINVAL;
	}

	ahead = malloc(sizeof(struct ngnfs_manifest_addr_head));
	if (!ahead)
		return -ENOMEM;

	ret = parse_ipv4_addr_port(&ahead->addr, str);
	if (ret < 0) {
		log("error parsing -d address");
		goto out;
	}

	list_add_tail(&ahead->head, addr_list);
	(*nr_addrs)++;
	return ret;
out:
	free(ahead);
	return ret;
}

void ngnfs_manifest_free_addrs(struct list_head *addr_list)
{
	struct ngnfs_manifest_addr_head *ahead;
	struct ngnfs_manifest_addr_head *tmp;

	list_for_each_entry_safe(ahead, tmp, addr_list, head) {
		list_del_init(&ahead->head);
		free(ahead);
	}
}

static struct ngnfs_devd_addrs *alloc_devd_array(u8 nr)
{
	return kzalloc(offsetof(struct ngnfs_devd_addrs, addrs[nr]), GFP_NOFS);
}

/* Caller must already have excluded other RCU readers. */
static void manifest_contents_destroy(struct ngnfs_manifest_contents *mfc)
{
	if (mfc)
		kfree(mfc->devd_array);
	kfree(mfc);
}

static void update_manifest_contents(struct ngnfs_fs_info *nfi, struct ngnfs_manifest_contents *new_mfc)
{
	struct ngnfs_manifest_info *mfinf = nfi->manifest_info;
	struct ngnfs_manifest_contents *old_mfc;

	mutex_lock(&mfinf->mutex);
	old_mfc = mfinf->contents;
	rcu_assign_pointer(mfinf->contents, new_mfc);
	mutex_unlock(&mfinf->mutex);

	wake_up(&mfinf->updates_waitq);

	synchronize_rcu();
	manifest_contents_destroy(old_mfc);
}

/*
 * Caller is responsible for noticing if the manifest info has changed and
 * restarting the transaction.
 */
int ngnfs_manifest_map_block(struct ngnfs_fs_info *nfi, u64 bnr, struct sockaddr_in *addr)
{
	struct ngnfs_devd_addrs *da;
	u32 rem;

	rcu_read_lock();

	da = rcu_dereference(nfi->manifest_info->contents)->devd_array;
	div_u64_rem(bnr, da->nr_addrs, &rem);
	*addr = da->addrs[rem];

	rcu_read_unlock();

	return 0;
}

/* Marshal from memory to network representation of manifest. */
static int manifest_marshal(struct ngnfs_manifest_contents *mfc, void *dst, u16 size)
{
	struct ngnfs_devd_addrs *da = mfc->devd_array;
	u8 nr = da->nr_addrs;
	int i;

	/* TODO: totally unsafe, will convert to protobuf */
	memcpy(dst, &da->nr_addrs, sizeof(da->nr_addrs));
	dst += sizeof(da->nr_addrs);
	for (i = 0; i < nr; i++) {
		memcpy(dst, &da->addrs[i], sizeof(da->addrs[i]));
		dst += sizeof(da->addrs[i]);
	}

	return 0;
}

/* Unmarshal from network to memory representation of manifest. */
static int manifest_unmarshal(struct ngnfs_manifest_contents *mfc, void *src, u16 size)
{
	struct ngnfs_devd_addrs *da;
	u8 nr;
	int i;
	int ret;

	/* TODO: totally unsafe, will convert to protobuf */
	nr = ((struct ngnfs_devd_addrs *)src)->nr_addrs;

	da = alloc_devd_array(nr);
	if (!da) {
		ret = -ENOMEM;
		goto out;
	}

	da->nr_addrs = nr;
	src += sizeof(da->nr_addrs);
	for (i = 0; i < nr; i++) {
		memcpy(&da->addrs[i], src, sizeof(da->addrs[i]));
		src += sizeof(da->addrs[i]);
	}
	mfc->devd_array = da;

	ret = 0;
out:
	if (ret < 0)
		manifest_contents_destroy(mfc);
	return ret;
}

static int manifest_msg_to_contents(struct ngnfs_fs_info *nfi, u64 seq_nr, void *data, u16 size)
{
	struct ngnfs_manifest_contents *mfc;
	int ret;

	mfc = kzalloc(sizeof(struct ngnfs_manifest_contents), GFP_NOFS);
	if (!mfc)
		return -ENOMEM;

	ret = manifest_unmarshal(mfc, data, size);
	if (ret < 0)
		goto out;

	mfc->seq_nr = seq_nr;
	update_manifest_contents(nfi, mfc);
out:
	if (ret < 0) {
		kfree(mfc);
	}
	return ret;
}

static struct manifest_msg *manifest_alloc_msg(u16 size)
{
	struct manifest_msg *mfm;

	BUG_ON(size > NGNFS_BLOCK_SIZE);

	mfm = kzalloc(sizeof(struct manifest_msg), GFP_NOFS);
	if (!mfm)
		return ERR_PTR(-ENOMEM);

	mfm->page = alloc_page(GFP_NOFS);

	if (!mfm->page) {
		kfree(mfm);
		mfm = ERR_PTR(-ENOMEM);
	}
	return mfm;
}

static void manifest_free_msg(struct manifest_msg *mfm)
{
	put_page(mfm->page);
	kfree(mfm);
}

static struct page *manifest_msg_to_page(struct manifest_msg *mfm)
{
	return mfm->page;
}

static struct manifest_msg *manifest_contents_to_msg(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_manifest_info *mfinf = nfi->manifest_info;
	struct ngnfs_manifest_contents *mfc;
	struct manifest_msg *mfm;
	u64 seq_nr;
	u8 nr;
	int ret;

	/*
	 * The size of the manifest contents can change between reading it and
	 * allocating the memory but we can't allocate while holding the RCU read
	 * lock. Just retry if that happens.
	 */
retry:
	rcu_read_lock();
	mfc = rcu_dereference(mfinf->contents);
	seq_nr = mfc->seq_nr;
	nr = mfc->devd_array->nr_addrs;
	rcu_read_unlock();

	mfm = manifest_alloc_msg(offsetof(struct ngnfs_devd_addrs, addrs[nr]));
	if (!mfm)
		return ERR_PTR(-ENOMEM);

	rcu_read_lock();
	mfc = rcu_dereference(mfinf->contents);
	if (seq_nr != mfc->seq_nr) {
		manifest_free_msg(mfm);
		rcu_read_unlock();
		goto retry;
	}

	ret = manifest_marshal(mfc, page_address(mfm->page), NGNFS_BLOCK_SIZE);
	rcu_read_unlock();

	mfm->seq_nr = seq_nr;

	if (ret < 0) {
		manifest_free_msg(mfm);
		mfm = ERR_PTR(-ENOMEM);
	}
	return mfm;
}

static int manifest_get_manifest(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_get_manifest_result res;
	struct ngnfs_msg_desc res_mdesc;
	struct manifest_msg *mfm;
	int ret;

	/* XXX permissions? other checks? */
	if ((mdesc->ctl_size != sizeof(struct ngnfs_msg_get_manifest)))
		return -EINVAL;

	mfm = manifest_contents_to_msg(nfi);
	if (IS_ERR(mfm))
		ret = PTR_ERR(mfm);
	else
		ret = 0;

	res.seq_nr = mfm->seq_nr;
	res.err = ngnfs_msg_err(ret);

	res_mdesc.type = NGNFS_MSG_GET_MANIFEST_RESULT;
	res_mdesc.addr = mdesc->addr;
	res_mdesc.ctl_buf = &res;
	res_mdesc.ctl_size = sizeof(res);
	if (ret < 0) {
		res_mdesc.data_page = NULL;
		res_mdesc.data_size = 0;
	} else {
		res_mdesc.data_page = manifest_msg_to_page(mfm);
		res_mdesc.data_size = NGNFS_BLOCK_SIZE;
	}

	ret = ngnfs_msg_send(nfi, &res_mdesc);
	manifest_free_msg(mfm);

	return ret;
}

/* Returns true when there is a new manifest than seq_nr. */
static int is_newer_manifest(struct ngnfs_manifest_info *mfinf, u64 old_seq_nr)
{
	u64 new_seq_nr = 0;

	rcu_read_lock();
	if (mfinf->contents)
		new_seq_nr = rcu_dereference(mfinf->contents)->seq_nr;
	rcu_read_unlock();

	return (new_seq_nr > old_seq_nr);
}

/*
 * Request manifest contents from addr and wait until an update is received.
 */
int ngnfs_manifest_request(struct ngnfs_fs_info *nfi, struct sockaddr_in *addr)
{
	struct ngnfs_manifest_info *mfinf = nfi->manifest_info;
	struct ngnfs_msg_get_manifest gm;
	struct ngnfs_msg_desc mdesc;
	u64 seq_nr;
	int ret;

	seq_nr = 0;
	if (mfinf->contents) {
		rcu_read_lock();
		seq_nr = rcu_dereference(mfinf->contents)->seq_nr;
		rcu_read_unlock();
	}

	gm.seq_nr = cpu_to_le64(seq_nr);

	mdesc.type = NGNFS_MSG_GET_MANIFEST;
	mdesc.addr = addr;
	mdesc.ctl_buf = &gm;
	mdesc.ctl_size = sizeof(gm);
	mdesc.data_page = NULL;
	mdesc.data_size = 0;

	ret = ngnfs_msg_send(nfi, &mdesc);

	wait_event(&mfinf->updates_waitq, is_newer_manifest(mfinf, seq_nr));

	return ret;
}

static int manifest_get_manifest_result(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_get_manifest_result *gmr = mdesc->ctl_buf;

	if (mdesc->ctl_size != sizeof(struct ngnfs_msg_get_manifest_result) ||
	    ((gmr->err == NGNFS_MSG_ERR_OK) && (mdesc->data_size != NGNFS_BLOCK_SIZE)) ||
	    ((gmr->err != NGNFS_MSG_ERR_OK) && (mdesc->data_size != 0)))
		return -EINVAL;

	if (gmr->err < 0)
		return ngnfs_msg_err(gmr->err);

	return manifest_msg_to_contents(nfi, le64_to_cpu(gmr->seq_nr), page_address(mdesc->data_page), mdesc->data_size);
}

/*
 * Just a u8 to limit the largest possible allocation.
 *
 * It's surprisingly ok to have duplicate addresses in the array
 * currently because we're not actually mapping the fs scoped block
 * numbers to device block numbers.  Each device must be able to store
 * the entire block space.
 */
static struct ngnfs_devd_addrs *list_to_addr_array(struct list_head *list, u8 nr)
{
	struct ngnfs_manifest_addr_head *ahead;
	struct ngnfs_devd_addrs *da;
	struct sockaddr_in *addr;
	int ret;

	if (nr == 0)
		return ERR_PTR(-EINVAL);

	da = kmalloc(offsetof(struct ngnfs_devd_addrs, addrs[nr]), GFP_NOFS);
	if (!da) {
		ret = -ENOMEM;
		goto out;
	}

	da->nr_addrs = nr;

	addr = &da->addrs[0];
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
	ret = 0;
out:
	if (ret < 0) {
		kfree(da);
		return ERR_PTR(ret);
	}
	return da;
}

/*
 * Can only be called when there are no users of the manifest running, such as
 * the block layer and the manifest message handlers.
 */
static void manifest_info_destroy(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_manifest_info *mfinf = nfi->manifest_info;

	if (mfinf) {
		if (mfinf->contents) {
			manifest_contents_destroy(mfinf->contents);
			mfinf->contents = NULL;
		}
		kfree(mfinf);
		nfi->manifest_info = NULL;
	}
}

static int manifest_contents_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr)
{
	struct ngnfs_manifest_info *mfinf = nfi->manifest_info;
	struct ngnfs_manifest_contents *mfc;
	struct ngnfs_devd_addrs *da;
	int ret;

	if (nr == 0)
		return -EINVAL;

	mfc = kzalloc(sizeof(struct ngnfs_manifest_contents), GFP_NOFS);
	if (!mfc) {
		ret = -ENOMEM;
		goto out;
	}

	da = list_to_addr_array(list, nr);
	if (!da) {
		ret = -ENOMEM;
		goto out;
	}

	mfc->devd_array = da;
	mfinf->contents = mfc;

	ret = 0;
out:
	if (ret < 0)
		kfree(mfc);
	return ret;
}

static int manifest_info_setup(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_manifest_info *mfinf;
	int ret;

	mfinf = kzalloc(sizeof(struct ngnfs_manifest_info), GFP_NOFS);
	if (!mfinf) {
		ret = -ENOMEM;
		goto out;
	}

	mutex_init(&mfinf->mutex);
	init_waitqueue_head(&mfinf->updates_waitq);
	nfi->manifest_info = mfinf;

	ret = 0;
out:
	if (ret < 0)
		manifest_info_destroy(nfi);
	return ret;
}

void ngnfs_manifest_client_destroy(struct ngnfs_fs_info *nfi)
{
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_GET_MANIFEST, manifest_get_manifest_result);
	manifest_info_destroy(nfi);
}

int ngnfs_manifest_client_setup(struct ngnfs_fs_info *nfi, struct sockaddr_in *manifest_server_addr, struct list_head *list, u8 nr)
{
	int ret;

	/*
	 * For the client, we want to register to receive manifest results before we
	 * set up the manifest info.
	 */
	ret = ngnfs_msg_register_recv(nfi, NGNFS_MSG_GET_MANIFEST_RESULT, manifest_get_manifest_result);
	if (ret < 0)
		return ret;

	ret = manifest_info_setup(nfi);
	if (ret < 0)
		goto out;

	/*
	 * Only ask for a manifest from the manifest server if no devices are
	 * specified on the command line. Presumably the user has good reason to
	 * force that.
	 */
	if (nr != 0)
		ret = manifest_contents_setup(nfi, list, nr);
	else
		ret = ngnfs_manifest_request(nfi, manifest_server_addr);
out:
	if (ret < 0)
		ngnfs_manifest_client_destroy(nfi);
	return ret;
}

void ngnfs_manifest_server_destroy(struct ngnfs_fs_info *nfi)
{
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_GET_MANIFEST, manifest_get_manifest);
	manifest_info_destroy(nfi);
}

int ngnfs_manifest_server_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr)
{
	int ret;

	/*
	 * For the server, we want the manifest info setup before we register to
	 * accept messages to serve it.
	 */
	ret = manifest_info_setup(nfi);
	if (ret < 0)
		return ret;

	ret = manifest_contents_setup(nfi, list, nr);
	if (ret < 0)
		goto out;

	/* TODO: load the sequence number from persistent storage. */
	nfi->manifest_info->contents->seq_nr = 1;

	ret = ngnfs_msg_register_recv(nfi, NGNFS_MSG_GET_MANIFEST, manifest_get_manifest);
out:
	if (ret < 0)
		ngnfs_manifest_server_destroy(nfi);
	return ret;
}
