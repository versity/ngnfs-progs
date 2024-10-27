/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_FORMAT_MSG_H
#define NGNFS_SHARED_FORMAT_MSG_H

#include <linux/types.h>

#include "shared/lk/compiler_attributes.h"
#include "shared/lk/types.h"

enum {
	NGNFS_MSG_GET_BLOCK = 0,
	NGNFS_MSG_GET_BLOCK_RESULT,
	NGNFS_MSG_WRITE_BLOCK,
	NGNFS_MSG_WRITE_BLOCK_RESULT,
	NGNFS_MSG_GET_MAPS,
	NGNFS_MSG_GET_MAPS_RESULT,
	NGNFS_MSG__NR,
};

enum {
	NGNFS_MSG_ERR_OK = 0,
	NGNFS_MSG_ERR_UNKNOWN,
	NGNFS_MSG_ERR_EIO,
	NGNFS_MSG_ERR_ENOMEM,
	NGNFS_MSG_ERR__INVALID,
};

enum {
	NGNFS_MSG_BLOCK_ACCESS_READ = 0,
	NGNFS_MSG_BLOCK_ACCESS_WRITE,
	NGNFS_MSG_BLOCK_ACCESS__UNKNOWN,
};

struct ngnfs_msg_header {
	__le32 crc;
	__le16 data_size;
	__u8 ctl_size;
	__u8 type;
};

#define NGNFS_MSG_MAX_CTL_SIZE	255
#define NGNFS_MSG_MAX_DATA_SIZE 4096

struct ngnfs_msg_get_block {
	__le64 bnr;
	__u8 access;
	__u8 _pad[7];
};

struct ngnfs_msg_get_block_result {
	__le64 bnr;
	__u8 access;
	__u8 err;
	__u8 _pad[6];
};

struct ngnfs_msg_write_block {
	__le64 bnr;
};

struct ngnfs_msg_write_block_result {
	__le64 bnr;
	__u8 err;
	__u8 _pad[7];
};

struct ngnfs_devd_map {
	__le64 version;
	__le64 nr_addrs;
	struct ngnfs_ipv4_addr {
		__le64 device_uuid; /* XXX future :) */
		__le32 addr;
		__le16 port;
		__le16 _pad;
	} addrs[];
};

/* Eventually this will have more than one map. */
struct ngnfs_maps {
	struct ngnfs_devd_map devd_map;
};

/*
 * TODO: currently a message can't have both ctl_size and buf_size of 0.
 * Either this will expand to have something in it or we will allow
 * that.
 */
struct ngnfs_msg_get_maps {
	__u8 _pad[8];
};

struct ngnfs_msg_get_maps_result {
	__u8 err;
	__u8 _pad[7];
	struct ngnfs_devd_map devd_map;
};

#endif
