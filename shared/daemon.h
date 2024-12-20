/* SPDX-License-Identifier: GPL-2.0 */

#ifndef NGNFS_SHARED_DAEMON_H
#define NGNFS_SHARED_DAEMON_H

int daemonize(int pipefd[2]);
int daemon_report(int pipefd[2], int status);

#endif
