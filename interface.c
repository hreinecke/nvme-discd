#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "common.h"
#include "discdb.h"
#include "tcp.h"
#include "endpoint.h"

LIST_HEAD(interface_list);

static int portid;

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
			if (id != -EAGAIN)
				fprintf(stderr,
					"iface %d: listener error %d\n",
					iface->portid, id);
			continue;
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

int interface_create(struct etcd_cdc_ctx *ctx,
		     struct nvmet_port *port)
{
	struct interface *iface;
	pthread_attr_t pthread_attr;
	int ret;

	iface = malloc(sizeof(struct interface));
	if (!iface)
		return -1;
	memset(iface, 0, sizeof(struct interface));
	INIT_LIST_HEAD(&iface->node);
	iface->ctx = ctx;
	iface->port = port;
	iface->portid = portid++;
	pthread_mutex_init(&iface->ep_mutex, NULL);
	printf("%s: %s addr %s:%d\n", __func__,
	       port->adrfam, port->traddr, ctx->port);

	if (!strcmp(port->adrfam, "ipv4")) {
		struct sockaddr_in *addr = (struct sockaddr_in *)&iface->addr;

		memset(addr, 0, sizeof(*addr));
		addr->sin_family = AF_INET;
		addr->sin_port = htons(ctx->port);
		inet_pton(AF_INET, port->traddr, &addr->sin_addr);
	} else {
		struct sockaddr_in6 *addr6 = &iface->addr;

		memset(addr6, 0, sizeof(*addr6));
		addr6->sin6_family = AF_INET6;
		addr6->sin6_port = htons(ctx->port);
		inet_pton(AF_INET6, port->traddr, &addr6->sin6_addr);
	}
	list_add(&iface->node, &interface_list);

	pthread_attr_init(&pthread_attr);
	ret = pthread_create(&iface->pthread, &pthread_attr,
			     interface_thread, iface);
	if (ret) {
		iface->pthread = 0;
		fprintf(stderr, "iface %d: failed to start iface, error %d\n",
			iface->portid, ret);
		list_del_init(&iface->node);
		free(iface);
	}
	pthread_attr_destroy(&pthread_attr);
	return ret;
}

static void interface_free(struct interface *iface)
{
	printf("%s: free interface %d\n", __func__, iface->portid);

	if (iface->pthread)
		pthread_join(iface->pthread, NULL);
	pthread_mutex_destroy(&iface->ep_mutex);
	list_del_init(&iface->node);
	free(iface);
}

int interface_delete(struct etcd_cdc_ctx *ctx, struct nvmet_port *port)
{
	struct interface *iface, *tmp;

	list_for_each_entry(tmp, &interface_list, node) {
		if (tmp->port == port) {
			iface = tmp;
			break;
		}
	}
	if (iface) {
		printf("%s: %s addr %s:%d\n", __func__,
		       port->adrfam, port->traddr, ctx->port);
		interface_free(iface);
	}
	return 0;
}

void terminate_interfaces(struct interface *iface, int signo)
{
	struct interface *_iface;

	stopped = true;
	list_for_each_entry(_iface, &interface_list, node) {
		if (_iface != iface)
			continue;
		fprintf(stderr, "iface %d: terminating\n",
			_iface->portid);
		pthread_kill(iface->pthread, signo);
	}
}
