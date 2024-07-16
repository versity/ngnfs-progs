/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NGNFS_SHARED_NERR_H
#define NGNFS_SHARED_NERR_H

#include <stdlib.h>
#include <string.h>

static inline int strdup_nerr(char **ret, const char *str)
{
	*ret = strdup(str);
	if (*ret == NULL)
		return -errno;

	return 0;
}

static inline int strndup_nerr(char **ret, const char *str, size_t n)
{
	*ret = strndup(str, n);
	if (*ret == NULL)
		return -errno;

	return 0;
}

static inline char **prepare_strtoX(char **endptr, char **ourend)
{
	errno = 0;
	if (endptr == NULL)
		return ourend;
	else
		return endptr;
}

static inline int finish_strtoX(char **endptr)
{
	if (errno)
		return -errno;
	else if (**endptr != '\0')
		return -EINVAL;
	else
		return 0;
}

static inline int strtoll_nerr(long long *ret, const char *restrict nptr,
			       char **restrict endptr, int base)
{
	char *ourend;

	endptr = prepare_strtoX(endptr, &ourend);
	*ret = strtoll(nptr, endptr, base);
	return finish_strtoX(endptr);
}

static inline int strtoull_nerr(unsigned long long *ret, const char *restrict nptr,
				char **restrict endptr, int base)
{
	char *ourend;

	endptr = prepare_strtoX(endptr, &ourend);
	*ret = strtoull(nptr, endptr, base);
	return finish_strtoX(endptr);
}

#endif
