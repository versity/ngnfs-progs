/* SPDX-License-Identifier: GPL-2.0 */

#include <assert.h>

#include "shared/lk/byteorder.h"
#include "shared/lk/cmpxchg.h"
#include "shared/lk/err.h"
#include "shared/lk/in.h"
#include "shared/lk/math64.h"
#include "shared/lk/rcupdate.h"
#include "shared/lk/slab.h"
#include "shared/lk/types.h"
#include "shared/lk/wait.h"

#include "shared/format-msg.h"
#include "shared/fs_info.h"
#include "shared/log.h"
#include "shared/map.h"
#include "shared/msg.h"

/*
 * The maps are updated using RCU. Add an RCU wrapper for the maps
 * struct so we can use lightweight kfree_rcu() instead of expensive
 * synchronize_rcu() when the maps change.
 */
struct ngnfs_maps_rcu {
	struct rcu_head rcu;
	struct ngnfs_maps maps;
};

struct ngnfs_map_info {
	struct wait_queue_head waitq;
	struct sockaddr_in mapd_server_addr;
	struct ngnfs_maps_rcu *maps_rcu;
};

static size_t get_maps_result_size(struct ngnfs_maps *maps)
{
	u8 nr = le64_to_cpu(maps->devd_map.nr_addrs);

	return offsetof(struct ngnfs_msg_get_maps_result, devd_map.addrs[nr]);
}

static void copy_maps(struct ngnfs_maps *dst, struct ngnfs_maps *src)
{
	u8 nr = le64_to_cpu(src->devd_map.nr_addrs);

	memcpy(&dst->devd_map, &src->devd_map, offsetof(struct ngnfs_maps, devd_map.addrs[nr]));
}

static struct ngnfs_maps *msg_to_maps(struct ngnfs_msg_get_maps_result *gmr)
{
	return (struct ngnfs_maps *) &gmr->devd_map;
}

struct ngnfs_msg_get_maps_result *ngnfs_maps_to_msg(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	struct ngnfs_msg_get_maps_result *msg;
	struct ngnfs_maps *maps;
	size_t alloced;
	size_t sz;
	int ret;

	/* XXX ? for Zach: This always fails the first time - is that intentional? */
        alloced = 0;
        msg = NULL;
        for (;;) {
                rcu_read_lock();
                maps = &rcu_dereference(minf->maps_rcu)->maps;
                sz = get_maps_result_size(maps);
                if (alloced == sz)
                        copy_maps(msg_to_maps(msg), maps);
                rcu_read_unlock();

                if (alloced == sz) {
                        ret = 0;
                        break;
                }

                kfree(msg);
                msg = kmalloc(sz, GFP_NOFS);
                if (!msg) {
                        ret = -ENOMEM;
                        break;
                }
                alloced = sz;
        }

	if (ret < 0)
		return ERR_PTR(ret);

	return msg;
}

int ngnfs_update_maps(struct ngnfs_fs_info *nfi, struct ngnfs_maps *new_maps)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	struct ngnfs_maps_rcu *old_rmaps;
	struct ngnfs_maps_rcu *new_rmaps;
	struct ngnfs_maps_rcu *tmp;
	u8 nr = le64_to_cpu(new_maps->devd_map.nr_addrs);

	/* Allocate the RCU wrapper for the new maps */
	new_rmaps = kmalloc(offsetof(struct ngnfs_maps_rcu, maps.devd_map.addrs[nr]), GFP_NOFS);
	if (!new_rmaps)
		return -ENOMEM;

	copy_maps(&new_rmaps->maps, new_maps);

	/* Use cmpxchg to atomically update the pointer to the RCU-wrapped maps */
	do {
		rcu_read_lock();
		old_rmaps = rcu_dereference(minf->maps_rcu);
		rcu_read_unlock();

		tmp = unrcu_pointer(cmpxchg(&minf->maps_rcu, old_rmaps, new_rmaps));
	} while (tmp != old_rmaps);

	if (old_rmaps)
		kfree_rcu(&old_rmaps->rcu);

	return 0;
}

static struct sockaddr_in map_to_addr(struct ngnfs_ipv4_addr *src_addr)
{
	struct sockaddr_in addr = { };

	addr.sin_addr.s_addr = le32_to_cpu(src_addr->addr);
	addr.sin_port = le16_to_cpu(src_addr->port);
	addr.sin_family = AF_INET;

	return addr;
}

/*
 * Request the maps but don't wait for them.
 */
int ngnfs_map_request_maps(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	struct ngnfs_msg_get_maps gm;
	struct ngnfs_msg_desc mdesc;

	mdesc.type = NGNFS_MSG_GET_MAPS;
	mdesc.addr = &minf->mapd_server_addr;
	mdesc.ctl_buf = &gm;
	mdesc.ctl_size = sizeof(gm);
	mdesc.data_page = NULL;
	mdesc.data_size = 0;

	return ngnfs_msg_send(nfi, &mdesc);
}

/*
 * Request the maps from the mapd server and wait till they arrive.
 */
int ngnfs_map_get_maps(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf = nfi->map_info;
	int ret;

	if (minf->maps_rcu != NULL)
		return 0;

	ret = ngnfs_map_request_maps(nfi);
	if (ret < 0)
		return ret;

	wait_event(&minf->waitq, (minf->maps_rcu != NULL));

	return ret;
}

/*
 * Caller is responsible for noticing if the maps have changed and restarting
 * the transaction. TODO: how?
 */
int ngnfs_map_map_block(struct ngnfs_fs_info *nfi, u64 bnr, struct sockaddr_in *addr)
{
	struct ngnfs_maps_rcu *nm;
	u32 rem;
	int ret;

	ret = ngnfs_map_get_maps(nfi);
	if (ret < 0)
		return ret;

	rcu_read_lock();

	nm = rcu_dereference(nfi->map_info->maps_rcu);
	div_u64_rem(bnr, le64_to_cpu(nm->maps.devd_map.nr_addrs), &rem);
	*addr = map_to_addr(&nm->maps.devd_map.addrs[rem]);

	rcu_read_unlock();

	return 0;
}

/*
 * Read the maps sent from the mapd server and load them.
 */
static int map_get_maps_result(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_get_maps_result *gmr = mdesc->ctl_buf;
	int ret;

	if (gmr->err < 0)
		return ngnfs_msg_err(gmr->err);

	ret = ngnfs_update_maps(nfi, msg_to_maps(gmr));
	wake_up(&nfi->map_info->waitq);

	return ret;
}

void ngnfs_map_destroy(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf = nfi->map_info;

	if (minf) {
		kfree(minf->maps_rcu);
		kfree(minf);
		nfi->map_info = NULL;
	}
}

int ngnfs_map_setup(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_map_info *minf;
	int ret;

	minf = kzalloc(sizeof(struct ngnfs_map_info), GFP_NOFS);
	if (!minf) {
		ret = -ENOMEM;
		goto out;
	}

	init_waitqueue_head(&minf->waitq);
	nfi->map_info = minf;

	ret = 0;
out:
	if (ret < 0)
		ngnfs_map_destroy(nfi);
	return ret;
}

void ngnfs_map_client_destroy(struct ngnfs_fs_info *nfi)
{
	ngnfs_msg_unregister_recv(nfi, NGNFS_MSG_GET_MAPS_RESULT, map_get_maps_result);
}

int ngnfs_map_client_setup(struct ngnfs_fs_info *nfi, struct sockaddr_in *mapd_server_addr)
{
	struct ngnfs_map_info *minf = nfi->map_info;

	minf->mapd_server_addr = *mapd_server_addr;

	return ngnfs_msg_register_recv(nfi, NGNFS_MSG_GET_MAPS_RESULT, map_get_maps_result);
}
