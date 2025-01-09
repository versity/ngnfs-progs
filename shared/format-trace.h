/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_FORMAT_TRACE_H
#define NGNFS_SHARED_FORMAT_TRACE_H

/*
 * The userspace trace file is organized into chunks.  The first chunk
 * contains a header that describes the overall file.  Following that
 * are fixed size regions for each thread.  Each thread's region
 * contains an initial chunk with an info struct followed by chunks of
 * events written as a ring.
 *
 * Each trace event is an initial struct defining the event id then a
 * variable size id struct containing that event's arguments.  Tracing
 * chunks are a stream of contiguous events that ends by either fully
 * consuming the chunk or ends with a terminating event header with a
 * size of 0.  Chunks are initialized to 0 so unwritten chunks look as
 * though they start with a terminating event.
 */

#include "shared/lk/byteorder.h"

#define TRACE_BUF_SIZE		(8 * 1024 * 1024)

#define TRACE_CHUNK_SHIFT	12
#define TRACE_CHUNK_SIZE	(1 << TRACE_CHUNK_SHIFT)
#define TRACE_CHUNK_MASK	(TRACE_CHUNK_SIZE - 1)

#define TRACE_THREAD_SIZE	(TRACE_CHUNK_SIZE + TRACE_BUF_SIZE)

struct ngnfs_trace_file {
	__u64 endian;
	__u64 synchro_tick;
	__u64 synchro_nsec;
	__u64 nsec_per_tick_num;
	__u64 nsec_per_tick_denom;
};

#define NGNFS_TRACE_NATIVE_ENDIAN 0xfedcba9876543210

struct ngnfs_trace_thread {
	__u64 tid;
};

/*
 * The size is redundant with the id but makes readers a bit easier to
 * implement and we have room in the first 64bits that are all
 * initialized with one constant store.
 *
 * A size of 0 indicates that there's no more events in the chunk.  It's
 * in the first 64bit word so that we can always have room to store that
 * (potentially partial) terminating header after an event if its
 * aligned size doesn't fill the chunk.
 */
struct ngnfs_trace_event {
	__u16 id;
	__u16 size;
	__u32 _pad;
	__u64 tick;
};

#endif
