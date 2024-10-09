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
 * TODO: make the map contents an array with functions to fill/free
 */
struct ngnfs_maps {
	u64 seq_nr;
	struct ngnfs_devd_addrs *devd_array;
};

struct ngnfs_map_info {
	struct wait_queue_head updates_waitq;
	struct mutex mutex;
	struct ngnfs_maps *maps;
};

/*
 * Structure for sending maps as a series of blocks via
 * ngnfs_msg_send. Currently we only send maps that fit in a single block but at
 * some point it will be an array of blocks.
 */
struct maps_msg {
	u64 seq_nr;
	struct page *page;
};

/* Parse the IPv4 addr:port in str and add it to addr_list. */
int ngnfs_map_append_addr(u8 *nr_addrs, struct list_head *addr_list, char *str)
{
	struct ngnfs_map_addr_head *ahead;
	int ret;

	if (*nr_addrs == U8_MAX) {
		log("too many -d addresses specified, exceeded limit of %u", U8_MAX);
		return -EINVAL;
	}

	ahead = malloc(sizeof(struct ngnfs_map_addr_head));
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

void ngnfs_map_free_addrs(struct list_head *addr_list)
{
	struct ngnfs_map_addr_head *ahead;
	struct ngnfs_map_addr_head *tmp;

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
static void maps_destroy(struct ngnfs_maps *maps)
{
	if (maps)
		kfree(maps->devd_array);
	kfree(maps);
}

static void update_maps(struct ngnfs_fs_info *nfi, struct ngnfs_maps *new_maps)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	struct ngnfs_maps *old_maps;

	mutex_lock(&minf->mutex);
	old_maps = minf->maps;
	rcu_assign_pointer(minf->maps, new_maps);
	mutex_unlock(&minf->mutex);

	wake_up(&minf->updates_waitq);

	synchronize_rcu();
	maps_destroy(old_maps);
}

/*
 * Caller is responsible for noticing if the maps have changed and restarting
 * the transaction. TODO: how?
 */
int ngnfs_map_map_block(struct ngnfs_fs_info *nfi, u64 bnr, struct sockaddr_in *addr)
{
	struct ngnfs_devd_addrs *da;
	u32 rem;

	rcu_read_lock();

	da = rcu_dereference(nfi->map_info->maps)->devd_array;
	div_u64_rem(bnr, da->nr_addrs, &rem);
	*addr = da->addrs[rem];

	rcu_read_unlock();

	return 0;
}

/* Marshal maps from memory to network representation. */
static int marshal_maps(struct ngnfs_maps *maps, void *dst, u16 size)
{
	struct ngnfs_devd_addrs *da = maps->devd_array;
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

/* Unmarshal maps from network to memory representation. */
static int unmarshal_maps(struct ngnfs_maps *maps, void *src, u16 size)
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
	maps->devd_array = da;

	ret = 0;
out:
	if (ret < 0)
		maps_destroy(maps);
	return ret;
}

static int msg_to_maps(struct ngnfs_fs_info *nfi, u64 seq_nr, void *data, u16 size)
{
	struct ngnfs_maps *maps;
	int ret;

	maps = kzalloc(sizeof(struct ngnfs_maps), GFP_NOFS);
	if (!maps)
		return -ENOMEM;

	ret = unmarshal_maps(maps, data, size);
	if (ret < 0)
		goto out;

	maps->seq_nr = seq_nr;
	update_maps(nfi, maps);
out:
	if (ret < 0) {
		kfree(maps);
	}
	return ret;
}

static struct maps_msg *alloc_maps_msg(u16 size)
{
	struct maps_msg *mm;

	BUG_ON(size > NGNFS_BLOCK_SIZE);

	mm = kzalloc(sizeof(struct maps_msg), GFP_NOFS);
	if (!mm)
		return ERR_PTR(-ENOMEM);

	mm->page = alloc_page(GFP_NOFS);

	if (!mm->page) {
		kfree(mm);
		mm = ERR_PTR(-ENOMEM);
	}
	return mm;
}

static void free_maps_msg(struct maps_msg *mm)
{
	put_page(mm->page);
	kfree(mm);
}

static struct page *maps_msg_to_page(struct maps_msg *mm)
{
	return mm->page;
}

static struct maps_msg *maps_to_maps_msg(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	struct ngnfs_maps *maps;
	struct maps_msg *mm;
	u64 seq_nr;
	u8 nr;
	int ret;

	/*
	 * The maps (and their size) can change between reading it and allocating
	 * the memory but we can't allocate while holding the RCU read lock. Just
	 * retry if that happens.
	 */
retry:
	rcu_read_lock();
	maps = rcu_dereference(minf->maps);
	seq_nr = maps->seq_nr;
	nr = maps->devd_array->nr_addrs;
	rcu_read_unlock();

	mm = alloc_maps_msg(offsetof(struct ngnfs_devd_addrs, addrs[nr]));
	if (!mm)
		return ERR_PTR(-ENOMEM);

	rcu_read_lock();
	maps = rcu_dereference(minf->maps);
	if (seq_nr != maps->seq_nr) {
		free_maps_msg(mm);
		rcu_read_unlock();
		goto retry;
	}

	ret = marshal_maps(maps, page_address(mm->page), NGNFS_BLOCK_SIZE);
	rcu_read_unlock();

	mm->seq_nr = seq_nr;

	if (ret < 0) {
		free_maps_msg(mm);
		mm = ERR_PTR(-ENOMEM);
	}
	return mm;
}

static int map_get_maps(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_get_maps_result res;
	struct ngnfs_msg_desc res_mdesc;
	struct maps_msg *mm;
	int ret;

	/* XXX permissions? other checks? */
	if ((mdesc->ctl_size != sizeof(struct ngnfs_msg_get_maps)))
		return -EINVAL;

	mm = maps_to_maps_msg(nfi);
	if (IS_ERR(mm))
		ret = PTR_ERR(mm);
	else
		ret = 0;

	res.seq_nr = mm->seq_nr;
	res.err = ngnfs_msg_err(ret);

	res_mdesc.type = NGNFS_MSG_GET_MAPS_RESULT;
	res_mdesc.addr = mdesc->addr;
	res_mdesc.ctl_buf = &res;
	res_mdesc.ctl_size = sizeof(res);
	if (ret < 0) {
		res_mdesc.data_page = NULL;
		res_mdesc.data_size = 0;
	} else {
		res_mdesc.data_page = maps_msg_to_page(mm);
		res_mdesc.data_size = NGNFS_BLOCK_SIZE;
	}

	ret = ngnfs_msg_send(nfi, &res_mdesc);
	free_maps_msg(mm);

	return ret;
}

/* Returns true when the caller's maps are newer than the maps with id seq_nr. */
static int maps_updated(struct ngnfs_map_info *minf, u64 old_seq_nr)
{
	u64 new_seq_nr = 0;

	rcu_read_lock();
	if (minf->maps)
		new_seq_nr = rcu_dereference(minf->maps)->seq_nr;
	rcu_read_unlock();

	return (new_seq_nr > old_seq_nr);
}

/*
 * Request maps from mapd server at addr and wait until an update is received.
 */
int ngnfs_maps_request(struct ngnfs_fs_info *nfi, struct sockaddr_in *addr)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	struct ngnfs_msg_get_maps gm;
	struct ngnfs_msg_desc mdesc;
	u64 seq_nr;
	int ret;

	seq_nr = 0;
	if (minf->maps) {
		rcu_read_lock();
		seq_nr = rcu_dereference(minf->maps)->seq_nr;
		rcu_read_unlock();
	}

	gm.seq_nr = cpu_to_le64(seq_nr);

	mdesc.type = NGNFS_MSG_GET_MAPS;
	mdesc.addr = addr;
	mdesc.ctl_buf = &gm;
	mdesc.ctl_size = sizeof(gm);
	mdesc.data_page = NULL;
	mdesc.data_size = 0;

	ret = ngnfs_msg_send(nfi, &mdesc);

	wait_event(&minf->updates_waitq, maps_updated(minf, seq_nr));

	return ret;
}

static int map_get_maps_result(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_get_maps_result *gmr = mdesc->ctl_buf;

	if (mdesc->ctl_size != sizeof(struct ngnfs_msg_get_maps_result) ||
	    ((gmr->err == NGNFS_MSG_ERR_OK) && (mdesc->data_size != NGNFS_BLOCK_SIZE)) ||
	    ((gmr->err != NGNFS_MSG_ERR_OK) && (mdesc->data_size != 0)))
		return -EINVAL;

	if (gmr->err < 0)
		return ngnfs_msg_err(gmr->err);

	return msg_to_maps(nfi, le64_to_cpu(gmr->seq_nr), page_address(mdesc->data_page), mdesc->data_size);
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
	struct ngnfs_map_addr_head *ahead;
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
 * Can only be called when there are no users of the maps running, such as
 * the block layer and the map message handlers.
 */
static void map_info_destroy(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf = nfi->map_info;

	if (minf) {
		if (minf->maps) {
			maps_destroy(minf->maps);
			minf->maps = NULL;
		}
		kfree(minf);
		nfi->map_info = NULL;
	}
}

static int maps_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	struct ngnfs_maps *maps;
	struct ngnfs_devd_addrs *da;
	int ret;

	if (nr == 0)
		return -EINVAL;

	maps = kzalloc(sizeof(struct ngnfs_maps), GFP_NOFS);
	if (!maps) {
		ret = -ENOMEM;
		goto out;
	}

	da = list_to_addr_array(list, nr);
	if (!da) {
		ret = -ENOMEM;
		goto out;
	}

	maps->devd_array = da;
	minf->maps = maps;

	ret = 0;
out:
	if (ret < 0)
		kfree(maps);
	return ret;
}

static int map_info_setup(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf;
	int ret;

	minf = kzalloc(sizeof(struct ngnfs_map_info), GFP_NOFS);
	if (!minf) {
		ret = -ENOMEM;
		goto out;
	}

	mutex_init(&minf->mutex);
	init_waitqueue_head(&minf->updates_waitq);
	nfi->map_info = minf;

	ret = 0;
out:
	if (ret < 0)
		map_info_destroy(nfi);
	return ret;
}

void ngnfs_map_client_destroy(struct ngnfs_fs_info *nfi)
{
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_GET_MAPS, map_get_maps_result);
	map_info_destroy(nfi);
}

int ngnfs_map_client_setup(struct ngnfs_fs_info *nfi, struct sockaddr_in *mapd_server_addr, struct list_head *list, u8 nr)
{
	int ret;

	/*
	 * For the client, we want to register to receive map messages before we set
	 * up the map info, since we may request it from a mapd server.
	 */
	ret = ngnfs_msg_register_recv(nfi, NGNFS_MSG_GET_MAPS_RESULT, map_get_maps_result);
	if (ret < 0)
		return ret;

	ret = map_info_setup(nfi);
	if (ret < 0)
		goto out;

	/*
	 * Only ask for maps from the mapd server if no maps are specified on the
	 * command line. Presumably the user has good reason to force that.
	 */
	if (nr != 0)
		ret = maps_setup(nfi, list, nr);
	else
		ret = ngnfs_maps_request(nfi, mapd_server_addr);
out:
	if (ret < 0)
		ngnfs_map_client_destroy(nfi);
	return ret;
}

void ngnfs_map_server_destroy(struct ngnfs_fs_info *nfi)
{
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_GET_MAPS, map_get_maps);
	map_info_destroy(nfi);
}

int ngnfs_map_server_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr)
{
	int ret;

	/*
	 * For the server, we want the maps set up before we register to accept
	 * messages to serve them.
	 */
	ret = map_info_setup(nfi);
	if (ret < 0)
		return ret;

	ret = maps_setup(nfi, list, nr);
	if (ret < 0)
		goto out;

	/* TODO: load the sequence number from persistent storage. */
	nfi->map_info->maps->seq_nr = 1;

	ret = ngnfs_msg_register_recv(nfi, NGNFS_MSG_GET_MAPS, map_get_maps);
out:
	if (ret < 0)
		ngnfs_map_server_destroy(nfi);
	return ret;
}
