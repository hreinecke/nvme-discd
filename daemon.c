/*
 * nvmet_etcd.c
 * decentralized NVMe discovery controller
 *
 * Copyright (c) 2021 Hannes Reinecke <hare@suse.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <getopt.h>
#include <sys/types.h>
#include <fcntl.h>

#include <sys/inotify.h>

#include "common.h"
#include "discdb.h"

static char *default_configfs = "/sys/kernel/config/nvmet";
static char *default_dbfile = "nvme_discdb.sqlite";

pthread_mutex_t signal_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t signal_cond = PTHREAD_COND_INITIALIZER;
sigset_t sigmask;

void *inotify_loop(void *);

int stopped = 0;
int tcp_debug = 0;
int cmd_debug = 0;

void *signal_loop(void *arg)
{
	int ret, signo;

	while (!stopped) {
		ret = sigwait(&sigmask, &signo);
		if (ret != 0) {
			fprintf(stderr, "sigwait failed with %d\n", errno);
			break;
		}
		switch (signo) {
		case SIGINT:
		case SIGTERM:
			printf("interrupted\n");
			pthread_mutex_lock(&signal_lock);
			stopped = 1;
			pthread_mutex_unlock(&signal_lock);
			pthread_cond_signal(&signal_cond);
			break;
		default:
			printf("unhandled signal %d\n", signo);
			break;
		}
	}
	pthread_exit(NULL);
	return NULL;
}

int parse_opts(struct etcd_cdc_ctx *ctx, int argc, char *argv[])
{
	struct option getopt_arg[] = {
		{"configfs", required_argument, 0, 'c'},
		{"port", required_argument, 0, 'p'},
		{"tls", no_argument, 0, 't'},
		{"nqn", required_argument, 0, 'n'},
		{"verbose", no_argument, 0, 'v'},
	};
	char c;
	int getopt_ind;

	while ((c = getopt_long(argc, argv, "c:e:n:p:st:v",
				getopt_arg, &getopt_ind)) != -1) {
		switch (c) {
		case 'c':
			ctx->configfs = optarg;
			break;
		case 'n':
			strcpy(ctx->subsys.subsysnqn, optarg);
			break;
		case 'p':
			ctx->port = atoi(optarg);
			break;
		case 't':
			ctx->tls++;
			break;
		case 'v':
			ctx->debug++;
			break;
		}
	}
	return 0;
}

int main (int argc, char *argv[])
{
	struct etcd_cdc_ctx *ctx;
	int ret = 0;
	pthread_t signal_thread, inotify_thread;
	pthread_attr_t pthread_attr;

	ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		fprintf(stderr, "cannot allocate context\n");
		exit(1);
	}
	memset(ctx, 0, sizeof(*ctx));
	ctx->configfs = default_configfs;
	ctx->ttl = 10;
	ctx->dbfile = default_dbfile;
	ctx->port = 8009;
	strcpy(ctx->host.hostnqn, NVME_DISC_SUBSYS_NAME);
	strcpy(ctx->subsys.subsysnqn, NVME_DISC_SUBSYS_NAME);

	parse_opts(ctx, argc, argv);

	if (ctx->debug)
		cmd_debug = 1;
	if (ctx->debug > 1)
		tcp_debug = 1;

	if (discdb_open(ctx->dbfile)) {
		ret = 1;
		goto out_free_ctx;
	}

	if (discdb_add_host(&ctx->host) < 0) {
		fprintf(stderr, "failed to insert default host %s\n",
			ctx->host.hostnqn);
		goto out_close_db;
	}
	if (discdb_add_subsys(&ctx->subsys) < 0) {
		fprintf(stderr, "failed to insert default subsys %s\n",
			ctx->subsys.subsysnqn);
		goto out_del_host;
	}
	discdb_add_host_subsys(&ctx->host, &ctx->subsys);

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);

	if (pthread_sigmask(SIG_BLOCK, &sigmask, NULL) < 0) {
		fprintf(stderr, "Couldn't block signals, error %d\n", errno);
		ret = 1;
		goto out_del_subsys;
	}
	ret = pthread_create(&signal_thread, NULL, signal_loop, NULL);
	if (ret) {
		signal_thread = 0;
		fprintf(stderr, "Could not start signal thread\n");
		ret = 1;
		goto out_del_subsys;
	}

	pthread_attr_init(&pthread_attr);
	ret = pthread_create(&inotify_thread, &pthread_attr,
			     inotify_loop, ctx);
	pthread_attr_destroy(&pthread_attr);
	if (ret) {
		inotify_thread = 0;
		fprintf(stderr, "failed to create inotify pthread: %d\n", ret);
		ret = 1;
		pthread_kill(signal_thread, SIGTERM);
		goto out_join;
	}

	pthread_mutex_lock(&signal_lock);
	while (!stopped)
		pthread_cond_wait(&signal_cond, &signal_lock);
	pthread_mutex_unlock(&signal_lock);

	interface_stop();

	pthread_kill(inotify_thread, SIGTERM);
	pthread_join(inotify_thread, NULL);
out_join:
	pthread_join(signal_thread, NULL);
out_del_subsys:
	discdb_del_host_subsys(&ctx->host, &ctx->subsys);
	discdb_del_subsys(&ctx->subsys);
out_del_host:
	discdb_del_host(&ctx->host);
out_close_db:
	discdb_close(ctx->dbfile);
out_free_ctx:
	free(ctx);
	return ret;
}
