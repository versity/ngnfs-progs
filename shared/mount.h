/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_MOUNT_H
#define NGNFS_SHARED_MOUNT_H

#include "shared/fs_info.h"

int ngnfs_mount(struct ngnfs_fs_info *nfi, int argc, char **argv);
void ngnfs_unmount(struct ngnfs_fs_info *nfi);

#endif
