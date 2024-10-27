/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_MAPD_RECV_H
#define NGNFS_MAPD_RECV_H

#include "shared/lk/in.h"
#include "shared/lk/list.h"
#include "shared/lk/types.h"
#include "shared/fs_info.h"

struct ngnfs_map_addr_head {
	struct list_head head;
	struct sockaddr_in addr;
};

int mapd_setup(struct ngnfs_fs_info *nfi, struct list_head *list, u8 nr);
void mapd_destroy(struct ngnfs_fs_info *nfi);

#endif
