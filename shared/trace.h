/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_TRACE_H
#define NGNFS_SHARED_TRACE_H

#include <stdio.h>
#include <pthread.h>

#include "shared/lk/types.h"

#include "shared/format-trace.h"
#include "shared/urcu.h"

extern pthread_key_t trace_thread_private_key;

int trace_register_thread(void);
void trace_unregister_thread(void);

int trace_setup(char *trace_path);
void trace_destroy(void);

struct trace_thread_private {
	void *addr;
	void *buf;
	size_t offset;
	size_t mask;
};

static inline struct trace_thread_private *trace_get_tpriv(void)
{
	return pthread_getspecific(trace_thread_private_key);
}

/*
 * This returns an offset delta to advance the caller's event into the
 * next chunk if its current position would overflow the chunk.  It's
 * done without branches.  Perhaps not the most clever, but it seemed
 * clear enough and quick to throw together.
 *
 * (The key here is turning crossed into a mask that maintains or clears
 * the offset to return depending on if we crossed the end of the chunk
 * of not).
 */
static inline u32 trace_chunk_round(u32 off, u32 size)
{
	u32 within = off & TRACE_CHUNK_MASK;
	u32 crossed = (within + size) & TRACE_CHUNK_SIZE;

	return (TRACE_CHUNK_SIZE - within) & (crossed - (crossed >> (TRACE_CHUNK_SHIFT - 1)));
}

/*
 * Give the generated calling event trace inline a pointer at which to
 * store its id struct.
 *
 * Since size is constant in the caller we have it include the size of
 * the header as well.
 *
 * We always store a terminating size = 0 in the next event.  This is
 * always possible because the event will be stored in the next chunk if
 * it fully consumes this chunk.  Sizes are aligned so if it didn't
 * consume this chunk then there's room for the initial aligned word in
 * the next event in this chunk.
 */
static inline void *trace_entry_ptr(u16 id, size_t size)
{
	struct trace_thread_private *tpriv = trace_get_tpriv();
	struct ngnfs_trace_event *tev;
	size_t off;

	off = (tpriv->offset + trace_chunk_round(tpriv->offset, size)) & tpriv->mask;
	tpriv->offset = off + size;

	tev = tpriv->buf + off;
	tev->id = id;
	tev->size = size;
	tev->_pad = 0;
	tev->tick = caa_get_cycles();

	((struct ngnfs_trace_event *)((void *)tev + size))->size = 0;

	return tev + 1;
}

#include "generated-trace-inlines.h"

#endif
