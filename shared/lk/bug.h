/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_LK_BUG_H
#define NGNFS_SHARED_LK_BUG_H

#include <assert.h>

#include "shared/log.h"

#define BUG_ON(cond) \
        assert(!(cond))

#define WARN_ON_ONCE(cond)				\
({							\
	__typeof__(cond) _cond = (cond);		\
	static bool _warned = false;			\
							\
	if (_cond && !_warned) {			\
		log("warning condition: "#cond);	\
		_warned = true;				\
	}						\
	_cond;						\
})

#endif
