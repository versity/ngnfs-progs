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
	NGNFS_MSG_GET_MANIFEST,
	NGNFS_MSG_GET_MANIFEST_RESULT,
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

struct ngnfs_msg_get_manifest {
	__le64 seq_nr;
};

struct ngnfs_msg_get_manifest_result {
	__le64 seq_nr;
	__u8 err;
	__u8 _pad[7];
};

#endif
