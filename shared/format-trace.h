/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_FORMAT_TRACE_H
#define NGNFS_SHARED_FORMAT_TRACE_H

/*
 * Just the binary trace format produced by userspace trace events.
 */

#include "shared/lk/byteorder.h"

struct ngnfs_trace_event_header {
	__le16 id;
	__le16 size;
	__le32 _pad;
};

#endif
