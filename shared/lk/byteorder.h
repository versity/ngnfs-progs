/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_BYTEORDER_H
#define NGNFS_SHARED_BYTEORDER_H

#include <endian.h>

#include "shared/lk/compiler_attributes.h"
#include "shared/lk/types.h"

static inline u16 ___swab16(u16 x)
{
	return	((x & (u16)0x00ffU) << 8) |
		((x & (u16)0xff00U) >> 8);
}

static inline u32 ___swab32(u32 x)
{
	return	((x & (u32)0x000000ffUL) << 24) |
		((x & (u32)0x0000ff00UL) << 8) |
		((x & (u32)0x00ff0000UL) >> 8) |
		((x & (u32)0xff000000UL) >> 24);
}

static inline u64 ___swab64(u64 x)
{
	return  (u64)((x & (u64)0x00000000000000ffULL) << 56) |
		(u64)((x & (u64)0x000000000000ff00ULL) << 40) |
		(u64)((x & (u64)0x0000000000ff0000ULL) << 24) |
		(u64)((x & (u64)0x00000000ff000000ULL) << 8) |
		(u64)((x & (u64)0x000000ff00000000ULL) >> 8) |
		(u64)((x & (u64)0x0000ff0000000000ULL) >> 24) |
		(u64)((x & (u64)0x00ff000000000000ULL) >> 40) |
		(u64)((x & (u64)0xff00000000000000ULL) >> 56);
}

#define __gen_cast_tofrom(end, size)					\
static inline __##end##size cpu_to_##end##size(u##size x)	\
{									\
	return (__force __##end##size)x;				\
}									\
static inline u##size end##size##_to_cpu(__##end##size x)	\
{									\
	return (__force u##size)x;				\
}

#define __gen_swap_tofrom(end, size)					\
static inline __##end##size cpu_to_##end##size(u##size x)	\
{									\
	return (__force __##end##size)___swab##size(x);		\
}									\
static inline u##size end##size##_to_cpu(__##end##size x)	\
{									\
	return ___swab##size((__force u##size) x);		\
}

#define __gen_functions(which, end)	\
	__gen_##which##_tofrom(end, 16)	\
	__gen_##which##_tofrom(end, 32)	\
	__gen_##which##_tofrom(end, 64)

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define __LITTLE_ENDIAN_BITFIELD
__gen_functions(swap, be)
__gen_functions(cast, le)
#elif __BYTE_ORDER == __BIG_ENDIAN
#define __BIG_ENDIAN_BITFIELD
__gen_functions(swap, le)
__gen_functions(cast, be)
#else
#error "machine is neither BIG_ENDIAN nor LITTLE_ENDIAN"
#endif

#define __gen_add_funcs(end, size)					\
static inline void end##size##_add_cpu(__##end##size *var, u##size val)	\
{									\
	*var = cpu_to_##end##size(end##size##_to_cpu(*var) + val);	\
}

__gen_add_funcs(le, 16)
__gen_add_funcs(le, 32)
__gen_add_funcs(le, 64)
__gen_add_funcs(be, 16)
__gen_add_funcs(be, 32)
__gen_add_funcs(be, 64)

#endif
