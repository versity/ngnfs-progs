/* SPDX-License-Identifier: GPL-2.0 */

/*
 * undo buffers store the original contents of a base buffer as it is
 * modified, such that we can copy the original contents back to the
 * base buffer and revert any modifications.  Each undo buffer has a
 * bitmap of the chunks of the base buffer that have been saved and
 * could be restored.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "shared/lk/align.h"
#include "shared/lk/bitops.h"
#include "shared/lk/bits.h"
#include "shared/lk/build_bug.h"
#include "shared/lk/bug.h"
#include "shared/lk/gfp.h"
#include "shared/lk/math.h"
#include "shared/lk/slab.h"
#include "shared/lk/stddef.h"

#include "shared/format-block.h"
#include "shared/unbuf.h"

struct ngnfs_undo_buf {
	void *base;
	void *page;
	void *buf;
	size_t bytes;
	size_t bits;
	unsigned long bitmap[0];
};

/*
 * Trying to choose an l1 cache size.
 */
#define CHUNK_SHIFT	6
#define CHUNK_BYTES	(1 << CHUNK_SHIFT)

int ngnfs_unbuf_alloc(void *base, size_t bytes, struct ngnfs_undo_buf **unbuf_ret)
{
	struct ngnfs_undo_buf *unbuf;
	struct page *page;
	size_t longs;
	size_t bits;
	int ret;

	/* XXX sloppy expedient assumption that caller buffers are single pages */
	if (WARN_ON_ONCE(!IS_ALIGNED(bytes, CHUNK_BYTES)) ||
	    WARN_ON_ONCE((bytes != PAGE_SIZE)))
		return -EINVAL;

	BUG_ON(!IS_ALIGNED(bytes, CHUNK_BYTES));

	bits = DIV_ROUND_UP(bytes, CHUNK_BYTES);
	longs = DIV_ROUND_UP(bits, BITS_PER_LONG);

	unbuf = kmalloc(offsetof(struct ngnfs_undo_buf, bitmap[longs]), GFP_NOFS);
	if (unbuf) {
		page = alloc_page(GFP_NOFS);
		if (!page) {
			kfree(unbuf);
			unbuf = NULL;
		} else {
			unbuf->base = base;
			unbuf->page = page;
			unbuf->buf = page_address(page);
			unbuf->bytes = bytes;
			unbuf->bits = bits;
			memset(unbuf->bitmap, 0, longs * sizeof(long));
		}
		ret = 0;
	} else {
		ret = -ENOMEM;
	}

	*unbuf_ret = unbuf;

	return ret;
}

void ngnfs_unbuf_free(struct ngnfs_undo_buf *unbuf)
{
	if (unbuf) {
		__free_page(unbuf->page);
		kfree(unbuf);
	}
}

void ngnfs_unbuf_save(struct ngnfs_undo_buf *unbuf, void *ptr, size_t size)
{
	off_t off;
	int last;
	int bit;

	if (size == 0)
		return;

	BUG_ON(ptr < unbuf->base);
	BUG_ON(size > unbuf->bytes);
	BUG_ON(ptr + size > unbuf->base + unbuf->bytes);

	bit = (ptr - unbuf->base) >> CHUNK_SHIFT;
	last = ((ptr + size - 1) - unbuf->base) >> CHUNK_SHIFT;

	for (; bit <= last; bit++) {
		if (!test_bit(bit, unbuf->bitmap)) {
			set_bit(bit, unbuf->bitmap);
			off = bit << CHUNK_SHIFT;
			memcpy(unbuf->buf + off, unbuf->base + off, CHUNK_BYTES);
		}
	}
}

void ngnfs_unbuf_restore(struct ngnfs_undo_buf *unbuf)
{
	size_t off;
	int bit;

	for (bit = 0;
	     (bit = find_next_bit(unbuf->bitmap, unbuf->bits, bit)) < unbuf->bits;
	     bit++) {
		clear_bit(bit, unbuf->bitmap);
		off = bit << CHUNK_SHIFT;
		memcpy(unbuf->base + off, unbuf->buf + off, CHUNK_BYTES);
	}
}
