/* SPDX-License-Identifier: GPL-2.0 */

#include <assert.h>
#include <time.h>

#include "shared/lk/ktime.h"

ktime_t ktime_get_real(void)
{
	struct timespec ts;
	int ret;

	ret = clock_gettime(CLOCK_REALTIME, &ts);
	assert(ret == 0);

	return timespec_to_ktime(ts);
}
