/* SPDX-License-Identifier: GPL-2.0 */

#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "shared/log.h"
#include "shared/nerr.h"
#include "shared/parse.h"

int parse_ull(unsigned long long *ull, char *str, unsigned long long least,
	      unsigned long long most)
{
	int ret;

	ret = strtoull_nerr(ull, str, NULL, 0);
	if (ret < 0)
		goto out;

	if (*ull < least || *ull > most) {
		log("parsed value %llu out of bounds, must be >= %llu and <= %llu",
		    *ull, least, most);
		ret = -EINVAL;
	}

out:
	return ret;
}

int parse_ll(long long *ll, char *str, long long least, long long most)
{
	int ret;

	ret = strtoll_nerr(ll, str, NULL, 0);
	if (ret < 0)
		goto out;

	if (*ll < least || *ll > most) {
		log("parsed value %lld out of bounds, must be >= %lld and <= %lld",
		    *ll, least, most);
		ret = -EINVAL;
	}

out:
	return ret;
}

/*
 * Simple dotted_quad:port ipv4.  No name resolution.  If the ':'
 * separator is missing the string is assumed to be the dotted quad
 * address.  When missing, addr defaults to INADDR_ANY and port to 0.
 */
int parse_ipv4_addr_port(struct sockaddr_in *sin, char *str)
{
	struct in_addr in = { .s_addr = htonl(INADDR_ANY), };
	long long port = 0;
	char *addr = NULL;
	char *dup = NULL;
	char *sep;
	int ret;

	sep = index(str, ':');

	if (sep > str) {
		ret = strndup_nerr(&dup, str, sep - str);
		if (ret < 0)
			goto out;

		addr = dup;

	} else if (sep == NULL) {
		addr = str;
	}

	if (addr) {
		if (inet_aton(addr, &in) != 1) {
			log("inet_aton failed to parse %s", addr);
			ret = -EINVAL;
			goto out;
		}
	}

	if (sep && *(sep + 1) != '\0') {
		ret = parse_ll(&port, sep + 1, 0, USHRT_MAX);
		if (ret) {
			log("error parsing port '%s' in '%s'", sep + 1, str);
			goto out;
		}
	}

	sin->sin_family = AF_INET;
	sin->sin_addr = in;
	sin->sin_port = htons(port);
	ret = 0;
out:
	free(dup);
	return ret;
}
