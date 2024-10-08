/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_MANIFEST_H
#define NGNFS_SHARED_MANIFEST_H

#include "shared/lk/in.h"
#include "shared/lk/list.h"

#include "shared/fs_info.h"

struct ngnfs_manifest_addr_head {
	struct list_head head;
	struct sockaddr_in addr;
};

int ngnfs_manifest_append_addr(u8 *nr_addrs, struct list_head *addr_list, char *str);
void ngnfs_manifest_free_addrs(struct list_head *addr_list);

int ngnfs_manifest_map_block(struct ngnfs_fs_info *nfi, u64 bnr, struct sockaddr_in *addr);

int ngnfs_manifest_request(struct ngnfs_fs_info *nfi, struct sockaddr_in *addr);

int ngnfs_manifest_client_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr);
void ngnfs_manifest_client_destroy(struct ngnfs_fs_info *nfi);

int ngnfs_manifest_server_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr);
void ngnfs_manifest_server_destroy(struct ngnfs_fs_info *nfi);

#endif
