/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_BITOPTS_H
#define NGNFS_SHARED_LK_BITOPTS_H

#include <stdbool.h>

#include "shared/lk/bits.h"
#include "shared/lk/compiler_attributes.h"
#include "shared/lk/types.h"

/*
 * We're being very conservative with the full memory ordering.
 *
 * We're using that the good old !!() in case bool is an int type
 * smaller than the long so we don't accidentally truncate high returned
 * bits to zero.
 */

static __always_inline unsigned long nr_bit(long nr)
{
	return 1UL << (nr & (BITS_PER_LONG - 1));
}

static __always_inline volatile unsigned long *nr_addr(long nr, volatile unsigned long *addr)
{
	return addr + (nr / BITS_PER_LONG);
}

static __always_inline bool test_bit(long nr, volatile unsigned long *addr)
{
	return !!(__atomic_load_n(nr_addr(nr, addr), __ATOMIC_SEQ_CST) & nr_bit(nr));
}

static __always_inline void set_bit(long nr, volatile unsigned long *addr)
{
	__atomic_or_fetch(nr_addr(nr, addr), nr_bit(nr), __ATOMIC_SEQ_CST);
}

static __always_inline void clear_bit(long nr, volatile unsigned long *addr)
{
	__atomic_and_fetch(nr_addr(nr, addr), ~nr_bit(nr), __ATOMIC_SEQ_CST);
}

static __always_inline bool test_and_set_bit(long nr, volatile unsigned long *addr)
{
	unsigned long bit = nr_bit(nr);

	return !!(__atomic_fetch_or(addr, bit, __ATOMIC_SEQ_CST) & bit);
}

static __always_inline bool test_and_clear_bit(long nr, volatile unsigned long *addr)
{
	unsigned long bit = nr_bit(nr);

	return !!(__atomic_fetch_and(addr, ~bit, __ATOMIC_SEQ_CST) & bit);
}

static __always_inline unsigned long hweight_long(unsigned long w)
{
	return __builtin_popcountl(w);
}

/**
 * rol64 - rotate a 64-bit value left
 * @word: value to rotate
 * @shift: bits to roll
 */
static inline __u64 rol64(__u64 word, unsigned int shift)
{
	return (word << (shift & 63)) | (word >> ((-shift) & 63));
}

/**
 * ror64 - rotate a 64-bit value right
 * @word: value to rotate
 * @shift: bits to roll
 */
static inline __u64 ror64(__u64 word, unsigned int shift)
{
	return (word >> (shift & 63)) | (word << ((-shift) & 63));
}

/**
 * rol32 - rotate a 32-bit value left
 * @word: value to rotate
 * @shift: bits to roll
 */
static inline __u32 rol32(__u32 word, unsigned int shift)
{
	return (word << (shift & 31)) | (word >> ((-shift) & 31));
}

/**
 * ror32 - rotate a 32-bit value right
 * @word: value to rotate
 * @shift: bits to roll
 */
static inline __u32 ror32(__u32 word, unsigned int shift)
{
	return (word >> (shift & 31)) | (word << ((-shift) & 31));
}

/**
 * rol16 - rotate a 16-bit value left
 * @word: value to rotate
 * @shift: bits to roll
 */
static inline __u16 rol16(__u16 word, unsigned int shift)
{
	return (word << (shift & 15)) | (word >> ((-shift) & 15));
}

/**
 * ror16 - rotate a 16-bit value right
 * @word: value to rotate
 * @shift: bits to roll
 */
static inline __u16 ror16(__u16 word, unsigned int shift)
{
	return (word >> (shift & 15)) | (word << ((-shift) & 15));
}

/**
 * rol8 - rotate an 8-bit value left
 * @word: value to rotate
 * @shift: bits to roll
 */
static inline __u8 rol8(__u8 word, unsigned int shift)
{
	return (word << (shift & 7)) | (word >> ((-shift) & 7));
}

/**
 * ror8 - rotate an 8-bit value right
 * @word: value to rotate
 * @shift: bits to roll
 */
static inline __u8 ror8(__u8 word, unsigned int shift)
{
	return (word >> (shift & 7)) | (word << ((-shift) & 7));
}

/**
 * __ffs - find first bit in word.
 * @word: The word to search
 *
 * Undefined if no bit exists, so code should check against 0 first.
 */
static __always_inline unsigned long __ffs(unsigned long word)
{
        return __builtin_ctzl(word);
}

#endif
