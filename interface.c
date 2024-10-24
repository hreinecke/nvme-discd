#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "common.h"
#include "discdb.h"
#include "tcp.h"
#include "endpoint.h"

LIST_HEAD(interface_list);
pthread_mutex_t interface_lock = PTHREAD_MUTEX_INITIALIZER;

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
	int ret = 0;

	if (strcmp(port->trtype, "tcp")) {
		printf("skip interface with transport type '%s'\n",
		       port->trtype);
		return 0;
	}
	pthread_mutex_lock(&interface_lock);
	list_for_each_entry(iface, &interface_list, node) {
		printf("iface %d: checking %s %s %s\n",
		       iface->portid, iface->port->trtype,
		       iface->port->traddr, iface->port->trsvcid);
		if (iface->port == port) {
			fprintf(stderr, "iface %d: skip duplicate interface\n",
				iface->portid);
			ret = -EAGAIN;
			break;
		}
		if (strcmp(iface->port->trtype, port->trtype))
			continue;
		if (strcmp(iface->port->traddr, port->traddr))
			continue;
		fprintf(stderr, "iface %d: duplicate interface requested\n",
			iface->portid);
		ret = -EBUSY;
		break;
	}
	if (ret < 0) {
		if (ret == -EAGAIN)
			ret = 0;
		goto out_unlock;
	}

	iface = malloc(sizeof(struct interface));
	if (!iface) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	memset(iface, 0, sizeof(struct interface));
	INIT_LIST_HEAD(&iface->node);
	iface->listenfd = -1;
	iface->ctx = ctx;
	iface->port = port;
	iface->portid = portid++;
	if (!strcmp(port->adrfam, "ipv6"))
		iface->adrfam = AF_INET6;
	else
		iface->adrfam = AF_INET;
	pthread_mutex_init(&iface->ep_mutex, NULL);
	printf("iface %d: create %s addr %s:%d\n", iface->portid,
	       port->adrfam, port->traddr, ctx->port);

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
out_unlock:
	pthread_mutex_unlock(&interface_lock);
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
	struct interface *iface = NULL, *tmp;

	pthread_mutex_lock(&interface_lock);
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
	pthread_mutex_unlock(&interface_lock);
	return 0;
}

void terminate_interfaces(struct interface *iface, int signo)
{
	struct interface *_iface;

	stopped = true;
	pthread_mutex_lock(&interface_lock);
	list_for_each_entry(_iface, &interface_list, node) {
		if (_iface != iface)
			continue;
		fprintf(stderr, "iface %d: terminating\n",
			_iface->portid);
		pthread_kill(iface->pthread, signo);
	}
	pthread_mutex_unlock(&interface_lock);
}
