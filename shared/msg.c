/* SPDX-License-Identifier: GPL-2.0 */

/*
 * The messaging layer provides one-way communication to named peers.
 * Peers are identified by ipv4 addrs.  We have a peer struct for each
 * remote address we can send to.  Peers are instantiated either by
 * trying to send to an address without a matching peer or by accepting
 * a connection on a listening address.
 *
 * Message delivery is very loose right now.  There's no timeouts,
 * reconnect attempts, or retransmission.  This'll all get fleshed out
 * as we better understand the layers that are communicating.
 *
 * The receive path is marshalled by having layers register receive
 * handlers for a u8 type in a message header.
 *
 * Most of the heavy lifting is handled by message transport layers.
 * They register ops to be called by messaging and call into messaging
 * with incoming peer connections or messages.
 *
 * XXX:
 *  - peer refcounts/rcu free
 *  - peer hash precence needs ref
 *  - block peer while ntx is connecting
 *  - teardown and remove from hash table
 */

#include "shared/lk/byteorder.h"
#include "shared/lk/bug.h"
#include "shared/lk/err.h"
#include "shared/lk/errno.h"
#include "shared/lk/kernel.h"
#include "shared/lk/limits.h"
#include "shared/lk/rcupdate.h"
#include "shared/lk/rhashtable.h"
#include "shared/lk/stddef.h"

#include "shared/msg.h"

struct ngnfs_msg_info {
	struct rhashtable ht;
	ngnfs_msg_recv_fn_t *recv_fns[NGNFS_MSG__NR];

	struct ngnfs_msg_transport_ops *mtr_ops;
	void *mtr_info;
	void *listen_info;
};

struct ngnfs_peer {
	struct rcu_head rcu;
	atomic_t refcount;
	struct rhash_head rhead;
	struct sockaddr_in addr;
	void *info;
};

static const struct rhashtable_params ngnfs_msg_ht_params = {
        .head_offset = offsetof(struct ngnfs_peer, rhead),
        .key_offset = offsetof(struct ngnfs_peer, addr),
        .key_len = sizeof_field(struct ngnfs_peer, addr),
};

static void put_peer(struct ngnfs_msg_info *minf, struct ngnfs_peer *peer)
{
	if (!IS_ERR_OR_NULL(peer) && atomic_dec_return(&peer->refcount) == 0) {
		if (peer->info && minf->mtr_ops->destroy_peer)
			minf->mtr_ops->destroy_peer(peer->info);
		kfree_rcu(&peer->rcu);
	}
}

/*
 * Get a peer for a given address.  If the peer doesn't exist then we
 * allocate a new one, initialize it, and start it up if it won the race
 * to be inserted into the hash table.
 *
 * The caller's 'accepted' arg tells us if we're initiating an outgoing
 * connection or are reacting to an incoming connection.
 */
static struct ngnfs_peer *get_peer(struct ngnfs_fs_info *nfi, struct ngnfs_msg_info *minf,
				   struct sockaddr_in *addr, void *accepted)
{
	struct ngnfs_peer *exist;
	struct ngnfs_peer *peer;
	int ret;

	rcu_read_lock();
	peer = rhashtable_lookup(&minf->ht, addr, ngnfs_msg_ht_params);
	if (peer) {
		if (accepted) {
			ret = -EEXIST;
		} else {
			atomic_inc(&peer->refcount);
			ret = 0;
		}
	} else {
		ret = 0;
	}
	rcu_read_unlock();
	if (peer || ret < 0)
		goto out;

	peer = kzalloc(sizeof(struct ngnfs_peer) + minf->mtr_ops->peer_info_size, GFP_NOFS);
	if (!peer) {
		ret = -ENOMEM;
		goto out;
	}

	atomic_set(&peer->refcount, 1);
	memcpy(&peer->addr, addr, sizeof(peer->addr)); /* memcpy for ht memcmp */

	if (minf->mtr_ops->peer_info_size > 0) {
		peer->info = (peer + 1);
		if (minf->mtr_ops->init_peer)
			minf->mtr_ops->init_peer(peer->info, nfi);
	}

	atomic_inc(&peer->refcount);
	rcu_read_lock();
	exist = rhashtable_lookup_get_insert_fast(&minf->ht, &peer->rhead, ngnfs_msg_ht_params);
	if (exist)
		atomic_inc(&exist->refcount);
	rcu_read_unlock();
	if (exist != NULL) {
		put_peer(minf, peer);
		put_peer(minf, peer);
		peer = exist;
		if (accepted)
			ret = -EEXIST;
		else
			ret = 0;
		goto out;
	}

	ret = minf->mtr_ops->start(peer->info, addr, accepted);
out:
	if (ret < 0) {
		put_peer(minf, peer);
		peer = ERR_PTR(ret);
	}
	return peer;
}

/*
 * This, perhaps too generously, accepts both positive and negative
 * errno.
 */
u8 ngnfs_msg_err(int eno)
{
#define err_case(e) \
	case e: return NGNFS_MSG_ERR_##e;

	switch (abs(eno)) {
		case 0: return NGNFS_MSG_ERR_OK;
		err_case(EIO)
		err_case(ENOMEM)
		default: return NGNFS_MSG_ERR_UNKNOWN;
	}
}

/* return -ve errno from our over-the-wire err */
int ngnfs_msg_errno(u8 err)
{
#define eno_case(e) \
	[NGNFS_MSG_ERR_##e] = e,

	static int eno[] = {
		eno_case(EIO)
		eno_case(ENOMEM)
	};

	switch (err) {
		case NGNFS_MSG_ERR_OK:			return 0;
		case NGNFS_MSG_ERR_UNKNOWN:		return -EIO;
		case NGNFS_MSG_ERR__INVALID ... U8_MAX:	return -EPROTO;
		default:				return -eno[err];
	}
}

int ngnfs_msg_verify_header(struct ngnfs_msg_header *hdr)
{
	if ((hdr->ctl_size == 0 && hdr->data_size == 0) ||
	    hdr->ctl_size > NGNFS_MSG_MAX_CTL_SIZE ||
	    le16_to_cpu(hdr->data_size) > NGNFS_MSG_MAX_DATA_SIZE ||
	    hdr->type >= NGNFS_MSG__NR)
		return -EINVAL;

	return 0;
}

/*
 * Establish a peer context and then hand the send off to the transport.
 * The transport will be copying the buf and page contents so the caller
 * can free the sent data once this returns.  (XXX We'll want to change
 * this to send by reference.)
 */
int ngnfs_msg_send(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_info *minf = nfi->msg_info;
	struct ngnfs_peer *peer;
	int ret;

	peer = get_peer(nfi, minf, mdesc->addr, NULL);
	if (IS_ERR(peer)) {
		ret = PTR_ERR(peer);
	} else {
		ret = minf->mtr_ops->send(peer->info, mdesc);
		put_peer(minf, peer);
	}

	return ret;
}

/*
 * The caller has only verified the internal validity of the header.
 */
int ngnfs_msg_recv(struct ngnfs_fs_info *nfi, struct ngnfs_msg_desc *mdesc)
{
	struct ngnfs_msg_info *minf = nfi->msg_info;

	if (mdesc->type < ARRAY_SIZE(minf->recv_fns) && minf->recv_fns[mdesc->type])
		return minf->recv_fns[mdesc->type](nfi, mdesc);
	else
		return -EINVAL;
}

/*
 * A transport has an incoming connection.  We look up the peer to
 * trigger starting up a new peer or, by providing the caller's non-null
 * arg, getting EEXIST if we already have a peer for the incoming
 * address.
 */
int ngnfs_msg_accept(struct ngnfs_fs_info *nfi, struct sockaddr_in *addr, void *arg)
{
	struct ngnfs_msg_info *minf = nfi->msg_info;
	struct ngnfs_peer *peer;
	int ret;

	if (WARN_ON_ONCE(arg == NULL))
		return -EINVAL;

	peer = get_peer(nfi, minf, addr, arg);
	if (IS_ERR(peer)) {
		ret = PTR_ERR(peer);
	} else {
		put_peer(minf, peer);
		ret = 0;
	}

	return ret;
}

/*
 * {un,}registration must be strictly single threaded.
 */
int ngnfs_msg_register_recv(struct ngnfs_fs_info *nfi, u8 type, ngnfs_msg_recv_fn_t fn)
{
	struct ngnfs_msg_info *minf = nfi->msg_info;

	if (type >= ARRAY_SIZE(minf->recv_fns) || minf->recv_fns[type])
		return -EEXIST;

	minf->recv_fns[type] = fn;
	return 0;
}

/*
 * We support _unregister being called in teardown paths without
 * messaging having been setup.  We just return if minf is null.
 */
void ngnfs_msg_unregister_recv(struct ngnfs_fs_info *nfi, u8 type, ngnfs_msg_recv_fn_t fn)
{
	struct ngnfs_msg_info *minf = nfi->msg_info;

	if (minf && type < ARRAY_SIZE(minf->recv_fns) && minf->recv_fns[type] == fn)
		minf->recv_fns[type] = NULL;
}

int ngnfs_msg_setup(struct ngnfs_fs_info *nfi, struct ngnfs_msg_transport_ops *mtr_ops,
		    void *setup_arg, struct sockaddr_in *listen_addr)
{
	struct ngnfs_msg_info *minf;
	void *info;
	int ret;

	minf = kzalloc(sizeof(struct ngnfs_msg_info), GFP_KERNEL);
	if (!minf) {
		ret = -ENOMEM;
		goto out;
	}

	minf->mtr_ops = mtr_ops;

	if (minf->mtr_ops->setup) {
		info = minf->mtr_ops->setup(nfi, setup_arg);
		if (IS_ERR(info)) {
			ret = PTR_ERR(info);
			goto out;
		}
		minf->mtr_info = info;
	}

	ret = rhashtable_init(&minf->ht, &ngnfs_msg_ht_params);
	if (ret < 0) {
		kfree(minf);
		goto out;
	}

	nfi->msg_info = minf;

	if (listen_addr) {
		info = minf->mtr_ops->start_listen(nfi, listen_addr);
		if (IS_ERR(info)) {
			ret = PTR_ERR(info);
			goto out;
		}
		minf->listen_info = info;
	}

	ret = 0;
out:
	return ret;
}

static void free_ht_node_peer(void *ptr, void *arg)
{
	struct ngnfs_peer *peer = ptr;
	struct ngnfs_msg_info *minf = arg;

	put_peer(minf, peer);
}

void ngnfs_msg_destroy(struct ngnfs_fs_info *nfi)
{
	struct ngnfs_msg_info *minf = nfi->msg_info;

	if (minf) {
		if (minf->listen_info)
			minf->mtr_ops->stop_listen(nfi, minf->listen_info);
		if (minf->mtr_ops->shutdown)
			minf->mtr_ops->shutdown(nfi, minf->mtr_info);
		if (minf->mtr_ops->destroy)
			minf->mtr_ops->destroy(nfi, minf->mtr_info);
		rhashtable_free_and_destroy(&minf->ht, free_ht_node_peer, minf);
		kfree(minf);
	}
}
