/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_DEVD_RECV_H
#define NGNFS_DEVD_RECV_H

int devd_recv_setup(struct ngnfs_fs_info *nfi);
void devd_recv_destroy(struct ngnfs_fs_info *nfi);

#endif
