/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_UNBUF_H
#define NGNFS_SHARED_UNBUF_H

struct ngnfs_undo_buf;

int ngnfs_unbuf_alloc(void *base, size_t bytes, struct ngnfs_undo_buf **unbuf_ret);
void ngnfs_unbuf_free(struct ngnfs_undo_buf *unbuf);

void ngnfs_unbuf_save(struct ngnfs_undo_buf *unbuf, void *ptr, size_t size);
void ngnfs_unbuf_restore(struct ngnfs_undo_buf *unbuf);

#endif
