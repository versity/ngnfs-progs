/* SPDX-License-Identifier: GPL-2.0 */

#include "shared/lk/err.h"
#include "shared/lk/limits.h"
#include "shared/lk/list.h"
#include "shared/lk/math64.h"
#include "shared/lk/slab.h"
#include "shared/lk/types.h"

#include "shared/fs_info.h"
#include "shared/log.h"
#include "shared/manifest.h"
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
	struct ngnfs_manifest_contents *contents;
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

/* Caller must already have excluded other RCU readers. */
static void manifest_contents_destroy(struct ngnfs_manifest_contents *mfc)
{
	if (mfc)
		kfree(mfc->devd_array);
	kfree(mfc);
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

	nfi->manifest_info = mfinf;

	ret = 0;
out:
	if (ret < 0)
		manifest_info_destroy(nfi);
	return ret;
}

void ngnfs_manifest_destroy(struct ngnfs_fs_info *nfi)
{
	manifest_info_destroy(nfi);
}

int ngnfs_manifest_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr)
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

out:
	if (ret < 0)
		ngnfs_manifest_destroy(nfi);
	return ret;
}
