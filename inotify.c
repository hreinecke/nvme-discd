/*
 * nvmet_inotify.c
 * inotify watcher for nvmet configfs
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
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/signalfd.h>
#include <fcntl.h>

#include <sys/inotify.h>

#include "list.h"
#include "common.h"

#define INOTIFY_BUFFER_SIZE 8192

LIST_HEAD(dir_watcher_list);

static int debug_inotify;

enum watcher_type {
	TYPE_HOST_DIR,		/* hosts */
	TYPE_HOST,		/* hosts/<host> */
	TYPE_PORT_DIR,		/* ports */
	TYPE_PORT,		/* ports/<port> */
	TYPE_PORT_ATTR,		/* ports/<port>/addr_<attr> */
	TYPE_PORT_SUBSYS_DIR,	/* ports/<port>/subsystems */
	TYPE_PORT_SUBSYS,	/* ports/<port>/subsystems/<subsys> */
	TYPE_SUBSYS_DIR,	/* subsystems */
	TYPE_SUBSYS,		/* subsystems/<subsys> */
	TYPE_SUBSYS_ATTR,	/* subsystems/<subsys>/attr_<attr> */
	TYPE_SUBSYS_HOSTS_DIR,	/* subsystems/<subsys>/allowed_hosts */
	TYPE_SUBSYS_HOST,	/* subsystems/<subsys>/allowed_hosts/<host> */
};

enum db_op {
	OP_ADD,
	OP_DEL,
	OP_MODIFY,
};
struct dir_watcher {
	struct list_head entry;
	enum watcher_type type;
	int wd;
	char dirname[FILENAME_MAX];
};

/* TYPE_HOST */
struct nvmet_host {
	struct dir_watcher watcher;
	char hostnqn[256];
};

/* TYPE_PORT */
struct nvmet_port {
	struct dir_watcher watcher;
	struct list_head subsystems;
	char port_id[256];
	char trtype[256];
	char traddr[256];
	char trsvcid[256];
	char adrfam[256];
	char treq[256];
	char tsas[256];
};

/* TYPE_SUBSYS */
struct nvmet_subsys {
	struct dir_watcher watcher;
	struct list_head hosts;
	char subsysnqn[256];
	int allow_any;
};

/* TYPE_PORT_SUBSYS */
struct nvmet_port_subsys {
	struct list_head entry;
	struct nvmet_port *port;
	struct nvmet_subsys *subsys;
};

/* TYPE_SUBSYS_HOST */
struct nvmet_subsys_host {
	struct list_head entry;
        struct nvmet_subsys *subsys;
	struct nvmet_host *host;
};

static void db_update_subsys_port(struct etcd_cdc_ctx *ctx,
				  struct nvmet_subsys *subsys,
				  struct nvmet_port *port, enum db_op op)
{
	struct nvmet_subsys_host *subsys_host;

	list_for_each_entry(subsys_host, &subsys->hosts, entry) {
		printf("%s %s/%s/%s\n",
		       op == OP_ADD ? "ADD" : "DEL",
		       subsys_host->host->hostnqn,
		       subsys->subsysnqn,
		       port->port_id);
	}
}

static void db_update_host_subsys(struct etcd_cdc_ctx *ctx,
				  struct nvmet_host *host,
				  struct nvmet_subsys *subsys,
				  enum db_op op)
{
	struct dir_watcher *watcher;

	list_for_each_entry(watcher, &dir_watcher_list, entry) {
		struct nvmet_port *port;
		struct nvmet_port_subsys *port_subsys;
		if (watcher->type != TYPE_PORT)
			continue;
		port = container_of(watcher, struct nvmet_port, watcher);
		list_for_each_entry(port_subsys, &port->subsystems, entry) {
			struct nvmet_subsys_host *subsys_host;

			if (port_subsys->subsys != subsys)
				continue;
			if (!host) {
				printf("%s <none>/%s/%s\n",
				       op == OP_ADD ? "ADD" : "DEL",
				       subsys->subsysnqn,
				       port->port_id);
				continue;
			}
			list_for_each_entry(subsys_host, &subsys->hosts, entry) {
				if (subsys_host->host != host)
					continue;
				printf("%s %s/%s/%s\n",
				       op == OP_ADD ? "ADD" : "DEL",
				       host->hostnqn,
				       subsys->subsysnqn,
				       port->port_id);
			}
		}
	}
}

static void db_modify_port(struct etcd_cdc_ctx *ctx,
			   struct nvmet_port *port)
{
	struct nvmet_port_subsys *port_subsys;

	list_for_each_entry(port_subsys, &port->subsystems, entry) {
		struct nvmet_subsys_host *subsys_host;
		struct nvmet_subsys *subsys = port_subsys->subsys;

		if (subsys->allow_any) {
			printf("MODIFY <none>/%s/%s\n",
			       subsys->subsysnqn,
			       port->port_id);
			continue;
		}
		list_for_each_entry(subsys_host, &subsys->hosts, entry) {
			struct nvmet_host *host = subsys_host->host;

			printf("MODIFY %s/%s/%s\n",
			       host->hostnqn,
			       subsys->subsysnqn,
			       port->port_id);
		}
	}
}

static struct dir_watcher *find_watcher(enum watcher_type type, char *path)
{
	struct dir_watcher *watcher;

	list_for_each_entry(watcher, &dir_watcher_list, entry) {
		if (watcher->type != type)
			continue;
		if (strcmp(watcher->dirname, path))
			continue;
		return watcher;
	}
	fprintf(stderr, "%s: no watcher found for type %d dir %s\n",
		__func__, type, path);
	return NULL;
}

static struct nvmet_port *port_from_port_subsys_dir(char *dir)
{
	char port_dir[PATH_MAX + 1], *p;
	struct dir_watcher *watcher;

	strcpy(port_dir, dir);
	p = strrchr(port_dir, '/');
	if (!p) {
		if (debug_inotify)
			fprintf(stderr, "%s: invalid directory %s\n",
				__func__, dir);
		return NULL;
	}
	*p = '\0';
	watcher = find_watcher(TYPE_PORT, port_dir);
	if (watcher)
		return container_of(watcher, struct nvmet_port, watcher);
	return NULL;
}

static struct nvmet_subsys *subsys_from_subsys_host_dir(char *dir)
{
	char subsys_dir[PATH_MAX + 1], *p;
	struct dir_watcher *watcher;

	strcpy(subsys_dir, dir);
	p = strrchr(subsys_dir, '/');
	if (!p) {
		fprintf(stderr, "%s: invalid directory %s\n", __func__, dir);
		return NULL;
	}
	*p = '\0';
	watcher = find_watcher(TYPE_SUBSYS, subsys_dir);
	if (watcher)
		return container_of(watcher, struct nvmet_subsys, watcher);
	return NULL;
}

static int attr_read_int(char *path, char *attr, int *v)
{
	char attr_path[PATH_MAX + 1];
	char attr_buf[256], *p;
	int fd, len, val;

	sprintf(attr_path, "%s/%s", path, attr);
	fd = open(attr_path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: failed to open '%s', error %d\n",
			__func__, attr_path, errno);
		return -1;
	}
	len = read(fd, attr_buf, 256);
	if (len < 0) {
		fprintf(stderr, "%s: failed to read '%s', error %d\n",
			__func__, attr_path, errno);
		memset(attr_buf, 0, 256);
	} else {
		val = strtoul(attr_buf, &p, 10);
		if (attr_buf == p)
			len = -1;
		else
			*v = val;
	}
	close(fd);
	return len;
}

static struct dir_watcher *add_watch(int fd, struct dir_watcher *watcher,
				     int flags)
{
	struct dir_watcher *tmp;

	INIT_LIST_HEAD(&watcher->entry);
	list_for_each_entry(tmp, &dir_watcher_list, entry) {
		if (tmp->type != watcher->type)
			continue;
		if (strcmp(tmp->dirname, watcher->dirname))
			continue;
		if (tmp->wd < 0)
			continue;
		if (debug_inotify)
			printf("re-use inotify watch %d type %d for %s\n",
			       watcher->wd, watcher->type, watcher->dirname);
		return tmp;
	}
	watcher->wd = inotify_add_watch(fd, watcher->dirname, flags);
	if (watcher->wd < 0) {
		fprintf(stderr,
			"%s: failed to add inotify watch to '%s', error %d\n",
			__func__, watcher->dirname, errno);
		return watcher;
	}
	if (debug_inotify)
		printf("add inotify watch %d type %d to %s\n",
		       watcher->wd, watcher->type, watcher->dirname);
	list_add(&watcher->entry, &dir_watcher_list);
	return 0;
}

static int remove_watch(int fd, struct etcd_cdc_ctx *ctx,
			struct dir_watcher *watcher)
{
	int ret;
	struct nvmet_host *host;
	struct nvmet_port *port;
	struct nvmet_subsys *subsys;
	struct nvmet_port_subsys *port_subsys, *tmp_p;
	struct nvmet_subsys_host *subsys_host, *tmp_s;

	ret = inotify_rm_watch(fd, watcher->wd);
	if (ret < 0)
		fprintf(stderr, "%s: failed to remove inotify watch on '%s'\n",
			__func__, watcher->dirname);
	if (debug_inotify)
		printf("remove inotify watch %d type %d from '%s'\n",
		       watcher->wd, watcher->type, watcher->dirname);
	list_del_init(&watcher->entry);

	switch (watcher->type) {
	case TYPE_HOST:
		host = container_of(watcher,
				    struct nvmet_host,
				    watcher);
		free(host);
		break;
	case TYPE_PORT:
		port = container_of(watcher,
				    struct nvmet_port,
				    watcher);
		free(port);
		break;
	case TYPE_PORT_SUBSYS_DIR:
		port = port_from_port_subsys_dir(watcher->dirname);
		if (!port) {
			free(watcher);
			break;
		}
		list_for_each_entry_safe(port_subsys, tmp_p,
					 &port->subsystems, entry) {
			if (debug_inotify)
				printf("unlink subsys %s from port %s\n",
				       port_subsys->subsys->subsysnqn,
				       port->port_id);
			list_del_init(&port_subsys->entry);
			db_update_subsys_port(ctx, port_subsys->subsys,
					      port, OP_DEL);
			free(port_subsys);
		}
		free(watcher);
		break;
	case TYPE_SUBSYS:
		subsys = container_of(watcher,
				      struct nvmet_subsys,
				      watcher);
		free(subsys);
		break;
	case TYPE_SUBSYS_HOSTS_DIR:
		subsys = subsys_from_subsys_host_dir(watcher->dirname);
		if (!subsys) {
			fprintf(stderr, "%s: invalid subsys host dir %s\n",
				__func__, watcher->dirname);
			free(watcher);
			break;
		}
		list_for_each_entry_safe(subsys_host, tmp_s,
					 &subsys->hosts, entry) {
			if (debug_inotify)
				printf("unlink host %s from subsys %s\n",
				       subsys_host->host->hostnqn,
				       subsys->subsysnqn);
			list_del_init(&subsys_host->entry);
			db_update_host_subsys(ctx, subsys_host->host,
					      subsys, OP_DEL);
			free(subsys_host);
		}
		free(watcher);
		break;
	default:
		if (debug_inotify)
			printf("free inotify type %d from %s\n",
			       watcher->type, watcher->dirname);
		free(watcher);
		break;
	}
	return ret;
}

static int watch_directory(int fd, char *dirname,
			   enum watcher_type type, int flags)
{
	struct dir_watcher *watcher, *tmp;

	watcher = malloc(sizeof(struct dir_watcher));
	if (!watcher) {
		fprintf(stderr, "%s: failed to allocate dirwatch\n", __func__);
		return -1;
	}
	strcpy(watcher->dirname, dirname);
	watcher->type = type;
	tmp = add_watch(fd, watcher, flags);
	if (tmp) {
		if (tmp == watcher)
			free(watcher);
		return -1;
	}
 	return 0;
}

static void watch_host(int fd, struct etcd_cdc_ctx *ctx,
		       char *hosts_dir, char *hostnqn)
{
	struct nvmet_host *host;
	struct dir_watcher *watcher;

	host = malloc(sizeof(struct nvmet_host));
	if (!host)
		return;

	strcpy(host->hostnqn, hostnqn);
	sprintf(host->watcher.dirname, "%s/%s",
		hosts_dir, hostnqn);
	host->watcher.type = TYPE_HOST;
	watcher = add_watch(fd, &host->watcher, IN_DELETE_SELF);
	if (watcher) {
		if (watcher == &host->watcher)
			free(host);
		return;
	}
}

static int port_read_attr(struct nvmet_port *port, char *attr)
{
	char attr_path[PATH_MAX + 1];
	char *attr_buf, *p;
	int fd, len;

	if (!strcmp(attr, "trtype"))
		attr_buf = port->trtype;
	else if (!strcmp(attr, "traddr"))
		attr_buf = port->traddr;
	else if (!strcmp(attr, "trsvcid"))
		attr_buf = port->trsvcid;
	else if (!strcmp(attr, "adrfam"))
		attr_buf = port->adrfam;
	else if (!strcmp(attr, "tsas"))
		attr_buf = port->tsas;
	else if (!strcmp(attr, "treq"))
		attr_buf = port->treq;
	else {
		fprintf(stderr, "%s: port %s invalid attribute '%s'\n",
			__func__, port->port_id, attr);
		return -1;
	}

	strncpy(attr_path, port->watcher.dirname, PATH_MAX);
	strcat(attr_path, "/addr_");
	strcat(attr_path, attr);
	fd = open(attr_path, O_RDONLY);
	if (fd < 0) {
		if (!strcmp(attr, "tsas")) {
			strcpy(attr_buf, "none");
			return 4;
		}
		fprintf(stderr, "%s: port %s failed to open '%s', error %d\n",
			__func__, port->port_id, attr_path, errno);
		return -1;
	}
	len = read(fd, attr_buf, 256);
	if (len < 0)
		memset(attr_buf, 0, 256);
	else {
		p = &attr_buf[len - 1];
		if (*p == '\n')
			*p = '\0';
	}
	close(fd);

	return len;
}

static struct nvmet_port *update_port(char *ports_dir, char *port_id)
{
	struct nvmet_port *port;

	port = malloc(sizeof(struct nvmet_port));
	if (!port) {
		fprintf(stderr, "%s: port %s failed to allocate port\n",
			__func__, port_id);
		return NULL;
	}
	INIT_LIST_HEAD(&port->subsystems);
	strcpy(port->port_id, port_id);
	sprintf(port->watcher.dirname, "%s/%s",
		ports_dir, port_id);
	port_read_attr(port, "trtype");
	port_read_attr(port, "traddr");
	port_read_attr(port, "trsvcid");
	port_read_attr(port, "adrfam");
	port_read_attr(port, "treq");
	port_read_attr(port, "tsas");
	return port;
}

static void add_port_subsys(struct etcd_cdc_ctx *ctx,
			    struct nvmet_port *port, char *subsysnqn)
{
	struct nvmet_port_subsys *port_subsys;
	struct dir_watcher *watcher;
	char subsys_dir[PATH_MAX + 1];
	struct nvmet_subsys *subsys;

	port_subsys = malloc(sizeof(struct nvmet_port_subsys));
	if (!port_subsys)
		return;
	INIT_LIST_HEAD(&port_subsys->entry);
	port_subsys->port = port;
	strcpy(subsys_dir, ctx->configfs);
	strcat(subsys_dir, "/subsystems/");
	strcat(subsys_dir, subsysnqn);
	watcher = find_watcher(TYPE_SUBSYS, subsys_dir);
	if (!watcher) {
		free(port_subsys);
		return;
	}
	subsys = container_of(watcher, struct nvmet_subsys, watcher);
	port_subsys->subsys = subsys;
	list_add(&port_subsys->entry, &port->subsystems);
	if (debug_inotify)
		printf("link port %s to subsys %s\n",
		       port->port_id, subsys->subsysnqn);
	db_update_subsys_port(ctx, subsys, port, OP_ADD);
}

static void link_port_subsys(struct etcd_cdc_ctx *ctx,
			     char *port_subsys_dir, char *subsysnqn)
{
	struct nvmet_port_subsys *port_subsys;
	struct nvmet_port *port;

	port = port_from_port_subsys_dir(port_subsys_dir);
	if (!port)
		return;

	list_for_each_entry(port_subsys, &port->subsystems, entry) {
		if (!strcmp(port_subsys->subsys->subsysnqn,
			    subsysnqn)) {
			fprintf(stderr, "%s: duplicate subsys %s for %s\n",
				__func__, subsysnqn, port->port_id);
			return;
		}
	}
	add_port_subsys(ctx, port, subsysnqn);
}
	
static void watch_port(int fd, struct etcd_cdc_ctx *ctx,
		       char *ports_dir, char *port_id)
{
	struct nvmet_port *port;
	struct dir_watcher *watcher;
	char subsys_dir[PATH_MAX + 1];
	DIR *sd;
	struct dirent *se;

	port = update_port(ports_dir, port_id);
	if (!port)
		return;

	port->watcher.type = TYPE_PORT;
	watcher = add_watch(fd, &port->watcher, IN_MODIFY | IN_DELETE_SELF);
	if (watcher) {
		if (watcher == &port->watcher)
			free(port);
		return;
	}

	strcpy(subsys_dir, port->watcher.dirname);
	strcat(subsys_dir, "/subsystems");
	watch_directory(fd, subsys_dir, TYPE_PORT_SUBSYS_DIR,
			IN_CREATE | IN_DELETE);

	sd = opendir(subsys_dir);
	if (!sd) {
		fprintf(stderr, "%s: cannot open %s\n", __func__, subsys_dir);
		return;
	}
	while ((se = readdir(sd))) {
		if (!strcmp(se->d_name, ".") ||
		    !strcmp(se->d_name, ".."))
			continue;
		add_port_subsys(ctx, port, se->d_name);
	}
	closedir(sd);
}

static void add_subsys_host(struct etcd_cdc_ctx *ctx,
			    struct nvmet_subsys *subsys, char *hostnqn)
{
	struct nvmet_subsys_host *subsys_host;
	struct dir_watcher *watcher;
	char host_dir[PATH_MAX + 1];
	struct nvmet_host *host;

  	subsys_host = malloc(sizeof(struct nvmet_subsys_host));
	if (!subsys_host) {
		fprintf(stderr, "%s: cannot allocate %s\n",
			__func__, hostnqn);
		return;
	}
	INIT_LIST_HEAD(&subsys_host->entry);
	subsys_host->subsys = subsys;
	strcpy(host_dir, ctx->configfs);
	strcat(host_dir, "/hosts/");
	strcat(host_dir, hostnqn);
	watcher = find_watcher(TYPE_HOST, host_dir);
	if (!watcher) {
		free(subsys_host);
		return;
	}
	host = container_of(watcher, struct nvmet_host, watcher);
	subsys_host->host = host;
	list_add(&subsys_host->entry, &subsys->hosts);
	if (debug_inotify)
		printf("link host %s to subsys %s\n",
		       host->hostnqn, subsys->subsysnqn);
	db_update_host_subsys(ctx, host, subsys, OP_ADD);
}

static void link_subsys_host(struct etcd_cdc_ctx *ctx,
			     char *subsys_hosts_dir, char *hostnqn)
{
	struct dir_watcher *watcher;
	struct nvmet_subsys_host *subsys_host;
	struct nvmet_subsys *subsys;
	char subsys_dir[PATH_MAX + 1], *p;

	strcpy(subsys_dir, subsys_hosts_dir);
	p = strrchr(subsys_dir, '/');
	if (!p) {
		fprintf(stderr, "%s: invalid directory %s\n",
			__func__, subsys_hosts_dir);
		return;
	}
	*p = '\0';
	watcher = find_watcher(TYPE_SUBSYS, subsys_dir);
	if (!watcher)
		return;

	subsys = container_of(watcher, struct nvmet_subsys, watcher);
	list_for_each_entry(subsys_host, &subsys->hosts, entry) {
		if (!strcmp(subsys_host->host->hostnqn, hostnqn)) {
			fprintf(stderr, "%s: duplicate host %s for %s\n",
				__func__, hostnqn, subsys->subsysnqn);
			return;
		}
	}
	add_subsys_host(ctx, subsys, hostnqn);
}

static void watch_subsys(int fd, struct etcd_cdc_ctx *ctx,
			 char *subsys_dir, char *subnqn)
{
	struct nvmet_subsys *subsys;
	struct dir_watcher *watcher;
	char ah_dir[PATH_MAX + 1];
	DIR *ad;
	struct dirent *ae;

	subsys = malloc(sizeof(struct nvmet_subsys));
	if (!subsys)
		return;
	INIT_LIST_HEAD(&subsys->hosts);
	strcpy(subsys->subsysnqn, subnqn);

	sprintf(subsys->watcher.dirname, "%s/%s",
		subsys_dir, subnqn);
	subsys->watcher.type = TYPE_SUBSYS;
	watcher = add_watch(fd, &subsys->watcher, IN_MODIFY | IN_DELETE_SELF);
	if (watcher) {
		if (watcher == &subsys->watcher) {
			free(subsys);
			return;
		}
	}

	attr_read_int(subsys->watcher.dirname,
		      "attr_allow_any_host",
		      &subsys->allow_any);
	if (subsys->allow_any)
		db_update_host_subsys(ctx, NULL, subsys, OP_ADD);

	sprintf(ah_dir, "%s/%s/allowed_hosts",
		subsys_dir, subnqn);
	watch_directory(fd, ah_dir, TYPE_SUBSYS_HOSTS_DIR,
			IN_CREATE | IN_DELETE);
	ad = opendir(ah_dir);
	if (!ad) {
		fprintf(stderr, "%s: cannot open %s\n", __func__, ah_dir);
		return;
	}
	while ((ae = readdir(ad))) {
		if (!strcmp(ae->d_name, ".") ||
		    !strcmp(ae->d_name, ".."))
			continue;
		add_subsys_host(ctx, subsys, ae->d_name);
	}
	closedir(ad);
}

static void
display_inotify_event(struct inotify_event *ev)
{
	if (!debug_inotify)
		return;
	printf("inotify wd = %d; ", ev->wd);
	if (ev->cookie > 0)
		printf("cookie = %4d; ", ev->cookie);

	printf("mask = ");

	if (ev->mask & IN_ISDIR)
		printf("IN_ISDIR ");

	if (ev->mask & IN_CREATE)
		printf("IN_CREATE ");

	if (ev->mask & IN_DELETE)
		printf("IN_DELETE ");

	if (ev->mask & IN_DELETE_SELF)
		printf("IN_DELETE_SELF ");

	if (ev->mask & IN_MODIFY)
		printf("IN_MODIFY ");

	if (ev->mask & IN_MOVE_SELF)
		printf("IN_MOVE_SELF ");
	if (ev->mask & IN_MOVED_FROM)
		printf("IN_MOVED_FROM ");
	if (ev->mask & IN_MOVED_TO)
		printf("IN_MOVED_TO ");

	if (ev->mask & IN_IGNORED)
		printf("IN_IGNORED ");
	if (ev->mask & IN_Q_OVERFLOW)
		printf("IN_Q_OVERFLOW ");
	if (ev->mask & IN_UNMOUNT)
		printf("IN_UNMOUNT ");

	if (ev->len > 0)
		printf("name = %s", ev->name);
	printf("\n");
}

int process_inotify_event(int fd, struct etcd_cdc_ctx *ctx,
			  char *iev_buf, int iev_len)
{
	struct inotify_event *ev;
	struct dir_watcher *tmp_watcher, *watcher = NULL;
	int ev_len;

	ev = (struct inotify_event *)iev_buf;
	display_inotify_event(ev);
	ev_len = sizeof(struct inotify_event) + ev->len;
	if (ev->mask & IN_IGNORED)
		return ev_len;

	list_for_each_entry(tmp_watcher, &dir_watcher_list, entry) {
		if (tmp_watcher->wd == ev->wd) {
			watcher = tmp_watcher;
			break;
		}
	}
	if (!watcher) {
		if (debug_inotify)
			printf("No watcher for wd %d\n", ev->wd);
		return ev_len;
	}
	if (ev->mask & IN_CREATE) {
		char subdir[FILENAME_MAX + 1];

		sprintf(subdir, "%s/%s", watcher->dirname, ev->name);
		if (debug_inotify) {
			if (ev->mask & IN_ISDIR)
				printf("mkdir %s\n", subdir);
			else
				printf("link %s\n", subdir);
		}
		switch (watcher->type) {
		case TYPE_HOST_DIR:
			watch_host(fd, ctx, watcher->dirname, ev->name);
			break;
		case TYPE_PORT_DIR:
			watch_port(fd, ctx, watcher->dirname, ev->name);
			break;
		case TYPE_PORT_SUBSYS_DIR:
			link_port_subsys(ctx, watcher->dirname, ev->name);
			break;
		case TYPE_SUBSYS_DIR:
			watch_subsys(fd, ctx, watcher->dirname, ev->name);
			break;
		case TYPE_SUBSYS_HOSTS_DIR:
			link_subsys_host(ctx, watcher->dirname, ev->name);
			break;
		default:
			fprintf(stderr, "%s: unhandled create type %d\n",
				__func__, watcher->type);
			break;
		}
	} else if (ev->mask & IN_DELETE_SELF) {
		struct nvmet_port *port;
		struct nvmet_subsys *subsys;
		struct nvmet_host *host;
		char path[PATH_MAX + 1];

		if (debug_inotify)
			printf("rmdir %s type %d\n",
			       watcher->dirname, watcher->type);

		/* Watcher is already removed */
		list_del_init(&watcher->entry);
		switch (watcher->type) {
		case TYPE_PORT:
			port = container_of(watcher,
					    struct nvmet_port, watcher);
			strcpy(path, port->watcher.dirname);
			strcat(path, "/subsystems");
			watcher = find_watcher(TYPE_PORT_SUBSYS_DIR, path);
			if (watcher) {
				if (debug_inotify)
					printf("free %s\n",
					       watcher->dirname);
				list_del_init(&watcher->entry);
				free(watcher);
			}
			free(port);
			break;
		case TYPE_SUBSYS:
			subsys = container_of(watcher,
					      struct nvmet_subsys, watcher);

			strcpy(path, subsys->watcher.dirname);
			strcat(path, "/allowed_hosts");
			watcher = find_watcher(TYPE_SUBSYS_HOSTS_DIR, path);
			if (watcher) {
				if (debug_inotify)
					printf("free %s\n",
					       watcher->dirname);
				list_del_init(&watcher->entry);
				free(watcher);
			}
			free(subsys);
			break;
		case TYPE_HOST:
			host = container_of(watcher,
					    struct nvmet_host, watcher);
			free(host);
			break;
		default:
			free(watcher);
			break;
		}
	} else if (ev->mask & IN_DELETE) {
		struct nvmet_port *port;
		struct nvmet_port_subsys *port_subsys, *tmp_p;
		struct nvmet_subsys *subsys;
		struct nvmet_subsys_host *subsys_host, *tmp_s;
		char subdir[FILENAME_MAX + 1];

		sprintf(subdir, "%s", watcher->dirname);
		if (debug_inotify) {
			if (ev->mask & IN_ISDIR)
				printf("rmdir %s %s\n", subdir, ev->name);
			else
				printf("unlink %s %s\n", subdir, ev->name);
		}
		switch (watcher->type) {
		case TYPE_PORT_SUBSYS_DIR:
			port = port_from_port_subsys_dir(watcher->dirname);
			if (!port) {
				free(watcher);
				break;
			}
			list_for_each_entry(tmp_p, &port->subsystems, entry) {
				if (!strcmp(tmp_p->subsys->subsysnqn,
					    ev->name)) {
					port_subsys = tmp_p;
					break;
				}
			}
			if (!port_subsys) {
				fprintf(stderr, "%s: port_subsys %s not found\n",
					__func__, ev->name);
				free(watcher);
			} else {
				if (debug_inotify)
					printf("unlink subsys %s from port %s\n",
					       ev->name, port->port_id);
				list_del_init(&port_subsys->entry);
				db_update_subsys_port(ctx,
						      port_subsys->subsys,
						      port, OP_DEL);
				free(port_subsys);
			}
			break;
		case TYPE_SUBSYS_HOSTS_DIR:
			subsys = subsys_from_subsys_host_dir(watcher->dirname);
			if (!subsys) {
				fprintf(stderr, "%s: subsys not found for dir %s\n",
					__func__, watcher->dirname);
				free(watcher);
				break;
			}
			list_for_each_entry(tmp_s, &subsys->hosts, entry) {
				if (!strcmp(tmp_s->host->hostnqn,
					    ev->name)) {
					subsys_host = tmp_s;
					break;
				}
			}
			if (!subsys_host) {
				fprintf(stderr, "%s: subsys_host %s not found\n",
					__func__, ev->name);
				free(watcher);
			} else {
				if (debug_inotify)
					printf("unlink host %s from subsys %s\n",
					       ev->name, subsys->subsysnqn);
				list_del_init(&subsys_host->entry);
				db_update_host_subsys(ctx, subsys_host->host,
						      subsys, OP_DEL);
				free(subsys_host);
			}
			break;
		default:
			remove_watch(fd, ctx, watcher);
			break;
		}
	} else if (ev->mask & IN_MODIFY) {
		struct nvmet_port *port;
		struct nvmet_subsys *subsys;

		if (debug_inotify)
			printf("write %s %s\n", watcher->dirname, ev->name);

		switch (watcher->type) {
		case TYPE_PORT:
			port = container_of(watcher,
					    struct nvmet_port,
					    watcher);
			if (!strncmp(ev->name, "addr_", 5)) {
				port_read_attr(port, ev->name + 5);
				db_modify_port(ctx, port);
			} else {
				fprintf(stderr,
					"%s: invalid port attribute %s\n",
					__func__, ev->name + 5);
			}
			break;
		case TYPE_SUBSYS:
			subsys = container_of(watcher,
					      struct nvmet_subsys,
					      watcher);
			if (!strncmp(ev->name, "attr_allow_any_host", 19)) {
				attr_read_int(subsys->watcher.dirname,
					      "attr_allow_any_host",
					      &subsys->allow_any);
				if (subsys->allow_any)
					db_update_host_subsys(ctx, NULL,
							      subsys, OP_ADD);
				else
					db_update_host_subsys(ctx, NULL,
							      subsys, OP_DEL);
			} else {
				if (debug_inotify)
					printf("unknown attribute %s/%s\n",
					       subsys->subsysnqn,
					       ev->name);
			}
			break;
		default:
			fprintf(stderr, "%s: unhandled modify type %d\n",
				__func__, watcher->type);
			break;
		}
	}
	return ev_len;
}

int watch_hosts_dir(int fd, struct etcd_cdc_ctx *ctx)
{
	char hosts_dir[PATH_MAX + 1];
	DIR *hd;
	struct dirent *he;

	strcpy(hosts_dir, ctx->configfs);
	strcat(hosts_dir, "/hosts");
	watch_directory(fd, hosts_dir, TYPE_HOST_DIR, IN_CREATE);

	hd = opendir(hosts_dir);
	if (!hd) {
		fprintf(stderr, "%s: cannot open %s\n", __func__, hosts_dir);
		return -1;
	}
	while ((he = readdir(hd))) {
		if (!strcmp(he->d_name, ".") ||
		    !strcmp(he->d_name, ".."))
			continue;
		watch_host(fd, ctx, hosts_dir, he->d_name);
	}
	closedir(hd);
	return 0;
}

int watch_ports_dir(int fd, struct etcd_cdc_ctx *ctx)
{
	char ports_dir[PATH_MAX + 1];
	DIR *pd;
	struct dirent *pe;

	strcpy(ports_dir, ctx->configfs);
	strcat(ports_dir, "/ports");
	watch_directory(fd, ports_dir, TYPE_PORT_DIR, IN_CREATE);

	pd = opendir(ports_dir);
	if (!pd) {
		fprintf(stderr, "%s: cannot open %s\n",
			__func__, ports_dir);
		return -1;
	}
	while ((pe = readdir(pd))) {
		if (!strcmp(pe->d_name, ".") ||
		    !strcmp(pe->d_name, ".."))
			continue;
		watch_port(fd, ctx, ports_dir, pe->d_name);
	}
	closedir(pd);
	return 0;
}

int watch_subsys_dir(int fd, struct etcd_cdc_ctx *ctx)
{
	char subsys_dir[PATH_MAX + 1];
	DIR *sd;
	struct dirent *se;

	strcpy(subsys_dir, ctx->configfs);
	strcat(subsys_dir, "/subsystems");
	watch_directory(fd, subsys_dir, TYPE_SUBSYS_DIR, IN_CREATE);

	sd = opendir(subsys_dir);
	if (!sd) {
		fprintf(stderr, "%s: cannot open %s\n", __func__, subsys_dir);
		return -1;
	}
	while ((se = readdir(sd))) {
		if (!strcmp(se->d_name, ".") ||
		    !strcmp(se->d_name, ".."))
			continue;
		watch_subsys(fd, ctx, subsys_dir, se->d_name);
	}
	closedir(sd);
	return 0;
}

void cleanup_watcher(int fd, struct etcd_cdc_ctx *ctx)
{
	struct dir_watcher *watcher, *tmp_watch;

    	list_for_each_entry_safe(watcher, tmp_watch, &dir_watcher_list, entry) {
		remove_watch(fd, ctx, watcher);
	}
}

void inotify_loop(struct etcd_cdc_ctx *ctx)
{
	sigset_t sigmask;
	int inotify_fd, signal_fd;
	fd_set rfd;
	struct timeval tmo;
	char event_buffer[INOTIFY_BUFFER_SIZE]
		__attribute__ ((aligned(__alignof__(struct inotify_event))));

	if (ctx->debug > 1)
		debug_inotify = 1;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);

	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0) {
		fprintf(stderr, "Couldn't block signals, error %d\n", errno);
		return;
	}
	signal_fd = signalfd(-1, &sigmask, 0);
	if (signal_fd < 0) {
		fprintf(stderr, "Couldn't setup signal fd, error %d\n", errno);
		return;
	}
	inotify_fd = inotify_init();
	if (inotify_fd < 0) {
		fprintf(stderr, "Could not setup inotify, error %d\n", errno);
		close(signal_fd);
		return;
	}

	watch_hosts_dir(inotify_fd, ctx);
	watch_subsys_dir(inotify_fd, ctx);
	watch_ports_dir(inotify_fd, ctx);

	for (;;) {
		int rlen, ret;
		char *iev_buf;

		FD_ZERO(&rfd);
		FD_SET(signal_fd, &rfd);
		FD_SET(inotify_fd, &rfd);
		tmo.tv_sec = ctx->ttl / 5;
		tmo.tv_usec = 0;
		ret = select(inotify_fd + 1, &rfd, NULL, NULL, &tmo);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "select returned %d", errno);
			break;
		}
		if (ret == 0) {
			/* Select timeout*/
			continue;
		}
		if (!FD_ISSET(inotify_fd, &rfd)) {
			struct signalfd_siginfo fdsi;

			if (!FD_ISSET(signal_fd, &rfd)) {
				fprintf(stderr,
					"select returned for invalid fd");
				continue;
			}
			rlen = read(signal_fd, &fdsi, sizeof(fdsi));
			if (rlen != sizeof(fdsi)) {
				fprintf(stderr,
					"Couldn't read siginfo\n");
				exit(1);
			}
			if (fdsi.ssi_signo == SIGINT ||
			    fdsi.ssi_signo == SIGTERM) {
				fprintf(stderr,
					"signal %d received, terminating\n",
					fdsi.ssi_signo);
				break;
			}
		}
		rlen = read(inotify_fd, event_buffer, INOTIFY_BUFFER_SIZE);
		if (rlen < 0) {
			fprintf(stderr, "error %d on reading inotify event",
				errno);
			continue;
		}
		for (iev_buf = event_buffer;
		     iev_buf < event_buffer + rlen; ) {
			int iev_len;

			iev_len = process_inotify_event(inotify_fd, ctx,
							iev_buf,
							event_buffer + rlen - iev_buf);
			if (iev_len < 0) {
				fprintf(stderr, "Failed to process inotify\n");
				break;
			}
			iev_buf += iev_len;
		}
	}
	cleanup_watcher(inotify_fd, ctx);

	close(inotify_fd);
	close(signal_fd);
}

