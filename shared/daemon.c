/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Basic routines to daemonize a server and return success on
 * initialization.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "shared/log.h"

/*
 * Spawn a background process to do long-lived work. The child must
 * report the status of its initialization via pipefd before the parent
 * will exit.
 *
 * The parent does not return from this function.
 *
 * In the child, the return value is 0 on success, -errno on failure.
 */
int daemonize(int pipefd[2])
{
	int status = 0;
	int count;
	int ret;

	ret = pipe(pipefd);
	if (ret < 0)
		return -errno;

	ret = fork();
	if (ret < 0)
		return -errno;

	if (ret > 0) {
		/* parent reads initialization status from child and exits */
		close(pipefd[1]);
		count = read(pipefd[0], &status, sizeof(status));
		if (count < 0)
			fprintf(stderr, "error reading child status: "ENOF"\n",	ENOA(status));
		if (count == 0)
			fprintf(stderr, "child exited before reporting status\n");
		if (status < 0)
			fprintf(stderr, "error starting server: "ENOF"\n", ENOA(-status));

		exit((count <= 0) || status);
	}

	/* child does daemon things and returns */
	close(pipefd[0]);
	ret = setsid();
	if (ret < 0 )
		return -errno;

	ret = daemon(0, 0);
	if (ret < 0)
		return -errno;

	return 0;
}

/*
 * When initialization is finished, send the return code to the
 * foreground process to report before it exits.
 */
int daemon_report(int pipefd[2], int status)
{
	int ret;

	ret = write(pipefd[1], &status, sizeof(status));
	close(pipefd[1]);

	return ret;
}
