/* SPDX-License-Identifier: GPL-2.0 */

#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "shared/lk/build_bug.h"
#include "shared/lk/byteorder.h"
#include "shared/lk/err.h"
#include "shared/lk/minmax.h"
#include "shared/lk/wait.h"

#include "shared/log.h"
#include "shared/msg.h"
#include "shared/mtr-socket.h"
#include "shared/shutdown.h"
#include "shared/thread.h"

/*
 * Provide a msg transport based on threads using sockets.
 */

struct socket_peer_info {
	struct ngnfs_fs_info *nfi;
	struct sockaddr_in addr;
	wait_queue_head_t waitq;
	struct cds_wfcq_head send_q_head;
	struct cds_wfcq_tail send_q_tail;
	struct thread connect_thr;
	struct thread listen_thr;
	struct thread send_thr;
	struct thread recv_thr;
	int fd;
	int err;
	int shutdown;
};

struct socket_send_buf {
	struct cds_wfcq_node q_node;
	size_t size;
	/* allocated packet contents to send start with header */
	struct ngnfs_msg_header hdr;
};

/*
 * Stop activity on the peer.  We shut down the socket and indicate that
 * the threads should return.  Resources are cleaned up as the peer is
 * freed after the threads have been joined.  This can be called
 * multiple times on a given peer.
 */
static void shutdown_peer(struct socket_peer_info *pinf, int err)
{
	if (uatomic_cmpxchg(&pinf->shutdown, 0, 1) == 0) {
		thread_stop_indicate(&pinf->connect_thr);
		thread_stop_indicate(&pinf->listen_thr);
		thread_stop_indicate(&pinf->send_thr);
		thread_stop_indicate(&pinf->recv_thr);
		if (pinf->fd >= 0)
			shutdown(pinf->fd, SHUT_RDWR);
		wake_up(&pinf->waitq);
	}

	/* don't really mind if this races */
	if (err < 0 && pinf->err == 0)
		pinf->err = err;
}

typedef ssize_t (*iovec_func)(int fd, const struct iovec *iov, int iovcnt);

/*
 * This is called to read and write vectored buffers from and to
 * sockets.  The iovcnt can be zero and all elements, if they exist,
 * will have non-zero lengths.
 *
 * 0 is returned if the all the buffers were transferred successfully.
 *
 * If the func returns 0 then the remote has disconnected the socket and
 * we return -ESHUTDOWN.
 */
static int whole_iovec(iovec_func func, int fd, struct iovec *iov, int iovcnt)
{
	ssize_t sret;
	size_t part;

	while (iovcnt > 0) {
		sret = func(fd, iov, iovcnt);
		if (sret < 0)
			return -errno;
		else if (sret == 0)
			return -ESHUTDOWN;

		while (sret > 0 && iovcnt > 0) {
			part = min(iov->iov_len, sret);
			iov->iov_base += part;
			iov->iov_len -= part;
			sret -= part;

			if (iov->iov_len == 0) {
				iov++;
				iovcnt--;
			}
		}
	}

	return 0;
}

static int iov_append(struct iovec *iov, int iovcnt, void *base, size_t len)
{
	if (len == 0)
		return iovcnt;

	iov[iovcnt].iov_base = base;
	iov[iovcnt].iov_len = len;

	return iovcnt + 1;
}

static void socket_send_thread(struct thread *thr, void *arg)
{
	struct socket_peer_info *pinf = arg;
	struct socket_send_buf *sbuf;
	struct cds_wfcq_node *node;
	struct cds_wfcq_head head;
	struct cds_wfcq_tail tail;
	struct iovec iov;
	int ret = 0;

	cds_wfcq_init(&head, &tail);

	while (!thread_should_return(thr)) {

		wait_event(&pinf->waitq, !cds_wfcq_empty(&pinf->send_q_head, &pinf->send_q_tail) ||
			   thread_should_return(thr));

		__cds_wfcq_splice_nonblocking(&head, &tail, &pinf->send_q_head, &pinf->send_q_tail);

		while ((node = __cds_wfcq_dequeue_nonblocking(&head, &tail))) {
			/* testing the theory that a single splice will never need to block */
			assert(node != CDS_WFCQ_WOULDBLOCK);
			sbuf = caa_container_of(node, struct socket_send_buf, q_node);

			iov_append(&iov, 0, &sbuf->hdr, sbuf->size);

			ret = whole_iovec(writev, pinf->fd, &iov, 1);
			if (ret < 0)
				goto out;

			free(sbuf);
		}
	}

	ret = 0;
out:
	while ((node = __cds_wfcq_dequeue_nonblocking(&head, &tail))) {
		assert(node != CDS_WFCQ_WOULDBLOCK);
		sbuf = caa_container_of(node, struct socket_send_buf, q_node);
		free(sbuf);
	}

	shutdown_peer(pinf, ret);
}

static void socket_recv_thread(struct thread *thr, void *arg)
{
	struct socket_peer_info *pinf = arg;
	struct page *ctl_page = NULL;
	struct ngnfs_msg_header hdr;
	struct ngnfs_msg_desc mdesc;
	struct iovec iov[3];
	int iovcnt;
	int ret;

	/* we'll want sub page alloc */
	BUILD_BUG_ON(PAGE_SIZE != NGNFS_MSG_MAX_DATA_SIZE);

	ctl_page = alloc_page(GFP_NOFS);
	if (!ctl_page) {
		ret = -ENOMEM;
		goto out;
	}

	mdesc.addr = &pinf->addr;
	mdesc.ctl_buf = page_address(ctl_page);

	ret = 0;
	while (!thread_should_return(thr)) {

		iov_append(iov, 0, &hdr, sizeof(hdr));
		ret = whole_iovec(readv, pinf->fd, iov, 1);
		if (ret < 0)
			break;

		ret = ngnfs_msg_verify_header(&hdr);
		if (ret < 0)
			break;

		mdesc.data_size = le16_to_cpu(hdr.data_size);
		mdesc.ctl_size = hdr.ctl_size;
		mdesc.type = hdr.type;

		if (mdesc.data_size) {
			mdesc.data_page = alloc_page(GFP_NOFS);
			if (!mdesc.data_page) {
				ret = -ENOMEM;
				break;
			}
		} else {
			mdesc.data_page = NULL;
		}

		iovcnt = iov_append(iov, 0, page_address(ctl_page), mdesc.ctl_size);
		iovcnt = iov_append(iov, iovcnt, page_address(mdesc.data_page), mdesc.data_size);

		ret = whole_iovec(readv, pinf->fd, iov, iovcnt);
		if (ret < 0)
			break;

		ret = ngnfs_msg_recv(pinf->nfi, &mdesc);

		if (mdesc.data_page) {
			put_page(mdesc.data_page);
			mdesc.data_page = NULL;
		}
		if (ret < 0)
			break;
	}

out:
	shutdown_peer(pinf, ret);
}

static int start_send_recv(struct socket_peer_info *pinf)
{
	return thread_start(&pinf->send_thr, socket_send_thread, pinf) ?:
	       thread_start(&pinf->recv_thr, socket_recv_thread, pinf);
}

/*
 * Set the options that we enable on active connected sockets, from
 * either accepting or connecting.
 */
static int set_connected_options(int fd)
{
	int optval;
	int ret;

	optval = 1;
	ret = setsockopt(fd, SOL_TCP, TCP_NODELAY, &optval, sizeof(optval));
	if (ret < 0) {
		ret = -errno;
		log("error setting TCP_NODELAY=%d on fd %d: " ENOF, optval, fd, ENOA(-ret));
	}

	return ret;
}

static void socket_connect_thread(struct thread *thr, void *arg)
{
	struct socket_peer_info *pinf = arg;
	int fd = -1;
	int ret;

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	ret = connect(fd, (struct sockaddr *)&pinf->addr, sizeof(pinf->addr));
	if (ret < 0) {
		ret = -errno;
		log("error creating send thread: "ENOF, ENOA(-ret));
		goto out;
	}

	ret = set_connected_options(fd);
	if (ret < 0)
		goto out;

	pinf->fd = fd;
	fd = -1;

	ret = start_send_recv(pinf);
out:
	if (fd >= 0)
		close(fd);
	if (ret < 0) {
		shutdown_peer(pinf, ret);
		ngnfs_shutdown(pinf->nfi, ret);
	}
}

static void socket_listen_thread(struct thread *thr, void *arg)
{
	struct socket_peer_info *pinf = arg;
	struct sockaddr_in addr;
	socklen_t len;
	int ret = 0;
	int fd;

	/* XXX is it ok to keep operating if we can't accept new connections? */

	while (!thread_should_return(thr)) {

		len = sizeof(addr);
		fd = accept(pinf->fd, (struct sockaddr *)&addr, &len);
		if (fd < 0) {
			ret = -errno;
			log("accept error: "ENOF, ENOA(-ret));
			break;
		}

		/* not possible after listening, surely */
		if (len != sizeof(struct sockaddr_in) || addr.sin_family != AF_INET) {
			log("invalid accepted sockaddr len %u or family %u", len, addr.sin_family);
			ret = -EINVAL;
			break;
		}

		ret = set_connected_options(fd) ?:
		      ngnfs_msg_accept(pinf->nfi, &addr, &fd);
		if (ret < 0) {
			close(fd);
			continue;
		}
	}

	if (!thread_should_return(thr)) {
		log("fatal listening thread error: "ENOF, ENOA(-ret));
		exit(1);
	}
}

static void socket_init_peer(void *info, struct ngnfs_fs_info *nfi)
{
	struct socket_peer_info *pinf = info;

	pinf->nfi = nfi;
	init_waitqueue_head(&pinf->waitq);
	cds_wfcq_init(&pinf->send_q_head, &pinf->send_q_tail);
	thread_init(&pinf->connect_thr);
	thread_init(&pinf->listen_thr);
	thread_init(&pinf->send_thr);
	thread_init(&pinf->recv_thr);
	pinf->fd = -1;
}

static void socket_destroy_peer(void *info)
{
	struct socket_peer_info *pinf = info;

	thread_stop_wait(&pinf->connect_thr);
	thread_stop_wait(&pinf->listen_thr);
	thread_stop_indicate(&pinf->send_thr);
	wake_up(&pinf->waitq);
	thread_stop_wait(&pinf->send_thr);
	thread_stop_wait(&pinf->recv_thr);

	if (pinf->fd >= 0)
		close(pinf->fd);
}

/*
 * Start up a socket for a peer in the msg core.
 *
 * If the start arg is null then the call is coming from a send and we
 * start the connect thread to try and get the socket.
 *
 * If we have start arg then we're coming from accept and already have
 * an accepted socket fd, we start the send and recv threads.
 */
static int socket_start(void *info, struct sockaddr_in *addr, void *accepted)
{
	struct socket_peer_info *pinf = info;
	int ret;

	pinf->addr = *addr;

	if (accepted) {
		int *fd = accepted;

		pinf->fd = *fd;
		ret = start_send_recv(pinf);
	} else {
		ret = thread_start(&pinf->connect_thr, socket_connect_thread, pinf);
	}

	if (ret < 0)
		shutdown_peer(pinf, ret);

	return 0;
}

/*
 * We re-use the peer structure for the listening context, but it's only
 * used to accept new connections.  The listen_thread is started as the
 * recv_thr and the send queue is never used.
 */
static void *socket_start_listen(struct ngnfs_fs_info *nfi, struct sockaddr_in *addr)
{
	struct socket_peer_info *pinf = NULL;
	int optval;
	int ret;

	pinf = calloc(1, sizeof(struct socket_peer_info));
	if (!pinf) {
		ret = -ENOMEM;
		goto out;
	}

	socket_init_peer(pinf, nfi);

	pinf->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (pinf->fd < 0) {
		ret = -errno;
		goto out;
	}

	optval = 1;
	ret = setsockopt(pinf->fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	if (ret < 0) {
		ret = -errno;
		log("setting SO_REUSEADDR failed");
		goto out;
	}

	ret = bind(pinf->fd, (struct sockaddr *)addr, sizeof(*addr));
	if (ret < 0) {
		ret = -errno;
		log("binding to "IPV4F" failed", IPV4A(addr));
		goto out;
	}

	ret = listen(pinf->fd, 255);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	ret = thread_start(&pinf->listen_thr, socket_listen_thread, pinf);
	if (ret < 0)
		log("error creating listen thread: "ENOF, ENOA(-ret));
out:
	if (ret < 0) {
		if (pinf) {
			socket_destroy_peer(pinf);
			free(pinf);
		}
		pinf = ERR_PTR(ret);
	}

	return pinf;
}

static void socket_stop_listen(struct ngnfs_fs_info *nfi, void *info)
{
	struct socket_peer_info *pinf = info;

	if (!IS_ERR_OR_NULL(pinf)) {
		shutdown_peer(pinf, 0);
		socket_destroy_peer(pinf);
		free(pinf);
	}
}

/*
 * Copy the send data into an allocated buffer and queue it for the send
 * thread.  We copy the send page today but could use a page reference
 * in the future.
 */
static int socket_send(void *info, struct ngnfs_msg_desc *mdesc)
{
	struct socket_peer_info *pinf = info;
	struct socket_send_buf *sbuf;
	void *data;
	void *ctl;
	int ret;

	if (pinf->err) {
		ret = pinf->err;
		goto out;
	}

	sbuf = malloc(sizeof(struct socket_send_buf) + mdesc->ctl_size + mdesc->data_size);
	if (!sbuf) {
		ret = -ENOMEM;
		goto out;
	}

	/* XXX crc not used yet */
	cds_wfcq_node_init(&sbuf->q_node);
	sbuf->size = sizeof(struct ngnfs_msg_header) + mdesc->ctl_size + mdesc->data_size;
	sbuf->hdr.data_size = cpu_to_le16(mdesc->data_size);
	sbuf->hdr.ctl_size = mdesc->ctl_size;
	sbuf->hdr.type = mdesc->type;

	ctl = &sbuf->hdr + 1;
	data = ctl + mdesc->ctl_size;

	if (mdesc->ctl_size)
		memcpy(ctl, mdesc->ctl_buf, mdesc->ctl_size);
	if (mdesc->data_size)
		memcpy(data, page_address(mdesc->data_page), mdesc->data_size);

	cds_wfcq_enqueue(&pinf->send_q_head, &pinf->send_q_tail, &sbuf->q_node);
	wake_up(&pinf->waitq);
	ret = 0;
out:
	return ret;
}

struct ngnfs_msg_transport_ops ngnfs_mtr_socket_ops = {
	.start_listen = socket_start_listen,
	.stop_listen = socket_stop_listen,

	.peer_info_size = sizeof(struct socket_peer_info),
	.init_peer = socket_init_peer,
	.destroy_peer = socket_destroy_peer,
	.start = socket_start,
	.send = socket_send,
};
