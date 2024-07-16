/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_RWONCE_H
#define NGNFS_SHARED_LK_RWONCE_H

#define READ_ONCE(x)	(*(const volatile typeof(x) *)&(x))

#define WRITE_ONCE(x, val)						\
do {									\
	*(volatile typeof(x) *)&(x) = (val);				\
} while (0)

#endif
