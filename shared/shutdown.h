/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_SHUTDOWN_H
#define NGNFS_SHARED_SHUTDOWN_H

#include <stdbool.h>

struct ngnfs_fs_info;

bool ngnfs_should_shutdown(struct ngnfs_fs_info *nfi);
void ngnfs_shutdown(struct ngnfs_fs_info *nfi, int err);

#endif
