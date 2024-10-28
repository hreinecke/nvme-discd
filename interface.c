#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "common.h"
#include "tcp.h"
#include "endpoint.h"
#include "discdb.h"

LIST_HEAD(interface_list);
pthread_mutex_t interface_lock = PTHREAD_MUTEX_INITIALIZER;

static void *interface_thread(void *arg)
{
	struct interface *iface = arg;
	struct endpoint *ep, *_ep;
	sigset_t set;
	int id;
	pthread_attr_t pthread_attr;
	int ret;

	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	ret = tcp_init_listener(iface);
	if (ret < 0) {
		fprintf(stderr,
			"iface %d: listener start error %d\n",
			iface->portid, ret);
		pthread_exit(NULL);
		return NULL;
	}

	while (!stopped) {
		id = tcp_wait_for_connection(iface, KATO_INTERVAL);

		if (stopped)
			break;

		if (id < 0) {
			if (id == -EAGAIN) {
				fprintf(stderr,
					"iface %d: listener interrupted\n",
					iface->portid);
				continue;
			}
			fprintf(stderr,
				"iface %d: listener error %d\n",
				iface->portid, id);
			break;
		}
		ep = enqueue_endpoint(id, iface);
		if (!ep)
			continue;

		pthread_attr_init(&pthread_attr);

		ret = pthread_create(&ep->pthread, &pthread_attr,
				     endpoint_thread, ep);
		if (ret) {
			ep->pthread = 0;
			fprintf(stderr,
				"iface %d: endpoint start error %d\n",
				iface->portid, ret);
		}
		pthread_attr_destroy(&pthread_attr);
	}

	printf("iface %d: destroy listener\n", iface->portid);

	tcp_destroy_listener(iface);
	pthread_mutex_lock(&iface->ep_mutex);
	list_for_each_entry_safe(ep, _ep, &iface->ep_list, node)
		dequeue_endpoint(ep);
	pthread_mutex_unlock(&iface->ep_mutex);
	pthread_exit(NULL);
	return NULL;
}

int interface_create(struct etcd_cdc_ctx *ctx, struct nvmet_port *port)
{
	struct interface *iface;
	pthread_attr_t pthread_attr;
	int ret = 0;

	if (strcmp(port->trtype, "tcp")) {
		printf("skip interface with transport type '%s'\n",
		       port->trtype);
		return 0;
	}
	pthread_mutex_lock(&interface_lock);
	list_for_each_entry(iface, &interface_list, node) {
		if (strcmp(iface->port.traddr, port->traddr))
			continue;
		if (strcmp(iface->port.adrfam, port->adrfam))
			continue;
		fprintf(stderr, "iface %d: duplicate interface requested\n",
			iface->portid);
		ret = -EBUSY;
		break;
	}
	if (ret < 0) {
		iface = NULL;
		goto out_unlock;
	}

	iface = malloc(sizeof(struct interface));
	if (!iface) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	memset(iface, 0, sizeof(struct interface));
	INIT_LIST_HEAD(&iface->node);
	INIT_LIST_HEAD(&iface->ep_list);
	pthread_mutex_init(&iface->ep_mutex, NULL);
	iface->listenfd = -1;
	iface->ctx = ctx;
	strcpy(iface->port.trtype, port->trtype);
	strcpy(iface->port.traddr, port->traddr);
	strcpy(iface->port.adrfam, port->adrfam);
	sprintf(iface->port.trsvcid, "%d", ctx->port);
	if (!strcmp(port->adrfam, "ipv6"))
		iface->adrfam = AF_INET6;
	else
		iface->adrfam = AF_INET;
	pthread_mutex_init(&iface->ep_mutex, NULL);
	ret = discdb_add_port(&iface->port, NVME_NQN_DISC);
	if (ret < 0) {
		fprintf(stderr, "failed to create interface for %s:%s:%s\n",
			iface->port.trtype, iface->port.traddr,
			iface->port.trsvcid);
		free(iface);
		iface = NULL;
		goto out_unlock;
	}
	iface->portid = iface->port.port_id;
	printf("iface %d: created %s addr %s:%s\n", iface->portid,
	       iface->port.adrfam, iface->port.traddr, iface->port.trsvcid);
	list_add(&iface->node, &interface_list);

	pthread_attr_init(&pthread_attr);
	ret = pthread_create(&iface->pthread, &pthread_attr,
			     interface_thread, iface);
	pthread_attr_destroy(&pthread_attr);
	if (ret) {
		iface->pthread = 0;
		fprintf(stderr, "iface %d: failed to start iface, error %d\n",
			iface->portid, ret);
		list_del_init(&iface->node);
		discdb_del_port(&iface->port);
		free(iface);
		iface = NULL;
	}
out_unlock:
	pthread_mutex_unlock(&interface_lock);
	if (iface)
		discdb_add_subsys_port(&ctx->subsys, &iface->port);
	return ret;
}

static void interface_free(struct interface *iface)
{
	printf("%s: free interface %d\n", __func__, iface->portid);

	if (iface->pthread)
		pthread_join(iface->pthread, NULL);
	pthread_mutex_destroy(&iface->ep_mutex);
	list_del_init(&iface->node);
	discdb_del_port(&iface->port);
	free(iface);
}

void interface_delete(struct etcd_cdc_ctx *ctx, struct nvmet_port *port)
{
	struct interface *iface = NULL, *tmp;
	int num_ports;

	num_ports = discdb_count_subsys_port(port, ctx->port);
	if (num_ports > 0) {
		fprintf(stderr, "iface %d: ports still pending (%d)\n",
			iface->portid, num_ports);
		return;
	}
	pthread_mutex_lock(&interface_lock);
	list_for_each_entry(tmp, &interface_list, node) {
		if (!strcmp(tmp->port.trtype, port->trtype) &&
		    !strcmp(tmp->port.traddr, port->traddr) &&
		    !strcmp(tmp->port.adrfam, port->adrfam)) {
			iface = tmp;
			break;
		}
	}
	if (iface)
		list_del_init(&iface->node);
	pthread_mutex_unlock(&interface_lock);
	if (!iface)
		return;

	fprintf(stderr, "iface %d: terminating\n",
		iface->portid);
	discdb_del_subsys_port(&iface->ctx->subsys, &iface->port);
	if (iface->pthread)
		pthread_kill(iface->pthread, SIGTERM);
	printf("%s: %s addr %s:%s\n", __func__,
	       iface->port.adrfam, iface->port.traddr, iface->port.trsvcid);
	interface_free(iface);
}
