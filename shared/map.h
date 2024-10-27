/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_MAP_H
#define NGNFS_SHARED_MAP_H

#include "shared/lk/in.h"
#include "shared/lk/types.h"

#include "shared/fs_info.h"

struct ngnfs_maps;

int ngnfs_update_maps(struct ngnfs_fs_info *nfi, struct ngnfs_maps *new_maps);
struct ngnfs_msg_get_maps_result *ngnfs_maps_to_msg(struct ngnfs_fs_info *nfi);

int ngnfs_map_map_block(struct ngnfs_fs_info *nfi, u64 bnr, struct sockaddr_in *addr);

int ngnfs_map_request_maps(struct ngnfs_fs_info *nfi);
int ngnfs_map_get_maps(struct ngnfs_fs_info *nfi);

int ngnfs_map_setup(struct ngnfs_fs_info *nfi);
void ngnfs_map_destroy(struct ngnfs_fs_info *nfi);

int ngnfs_map_client_setup(struct ngnfs_fs_info *nfi, struct sockaddr_in *mapd_server_addr);
void ngnfs_map_client_destroy(struct ngnfs_fs_info *nfi);

#endif
