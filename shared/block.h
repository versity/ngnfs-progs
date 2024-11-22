/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_BLOCK_H
#define NGNFS_SHARED_BLOCK_H

#include <stdlib.h>

struct ngnfs_block;

#include "shared/fs_info.h"
#include "shared/lk/gfp.h"
#include "shared/lk/list.h"
#include "shared/lk/types.h"

typedef enum {
	/* return a new block, allocate if missing, forget if existing */
	NBF_NEW = (1 << 0),
	/* acquire a read reference that can not be modified */
	NBF_READ = (1 << 1),
	/*
	 * Acquire an exclusive reference with an intent to write.  The
	 * block contents won't be directly written by the acquiring
	 * caller but will be done within _dirty_begin and _dirty_end.
	 */
	NBF_WRITE = (1 << 2),
} nbf_t;

/* these flags are mutually exclusive */
#define NBF_RW_EXCL	(NBF_READ | NBF_WRITE)

enum {
	NGNFS_BTX_OP_GET_READ,
	NGNFS_BTX_OP_GET_WRITE,
	NGNFS_BTX_OP_WRITE,
};

struct ngnfs_block_transport_ops {
	void *(*setup)(struct ngnfs_fs_info *nfi, void *arg);
	void (*shutdown)(struct ngnfs_fs_info *nfi, void *btr_info);
	void (*destroy)(struct ngnfs_fs_info *nfi, void *btr_info);
	int (*queue_depth)(struct ngnfs_fs_info *nfi, void *btr_info);
	int (*submit_block)(struct ngnfs_fs_info *nfi, void *btr_info,
			    int op, u64 bnr, struct page *data_page);
};

struct ngnfs_block *ngnfs_block_get(struct ngnfs_fs_info *nfi, u64 bnr, nbf_t nbf);
void ngnfs_block_put(struct ngnfs_block *bl);
void *ngnfs_block_buf(struct ngnfs_block *bl);
struct page *ngnfs_block_page(struct ngnfs_block *bl);

int ngnfs_block_dirty_begin(struct ngnfs_fs_info *nfi, struct list_head *list, ssize_t off);
void ngnfs_block_dirty_end(struct ngnfs_fs_info *nfi, struct list_head *list, ssize_t off);
int ngnfs_block_sync(struct ngnfs_fs_info *nfi);

void ngnfs_block_end_io(struct ngnfs_fs_info *nfi, u64 bnr, struct page *data_page, int err);

void ngnfs_block_shutdown(struct ngnfs_fs_info *nfi);
int ngnfs_block_setup(struct ngnfs_fs_info *nfi, struct ngnfs_block_transport_ops *btr_ops,
		      void *btr_setup_arg);
void ngnfs_block_destroy(struct ngnfs_fs_info *nfi);

#endif

