#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <libubox/uloop.h>
#include <libubox/ustream.h>
#include <libubox/utils.h>
#include <libubus.h>

#include "seriald.h"
#include "ubus_loop.h"

static struct ubus_context *ubus_ctx = NULL;
static const char *ubus_path;

static void seriald_ubus_add_fd(void);
static void seriald_ubus_connection_lost_cb(struct ubus_context *ctx);
static void seriald_ubus_reconnect_timer(struct uloop_timeout *timeout);
static int seriald_send_data(struct ubus_context *ctx, struct ubus_object *obj,
		struct ubus_request_data *req, const char *method, struct blob_attr *msg);

enum {
	DATA_DATA,
	__DATA_MAX
};

static const struct blobmsg_policy data_policy[__DATA_MAX] = {
	[DATA_DATA] = { .name = "data", .type = BLOBMSG_TYPE_STRING },
};

static struct ubus_method seriald_object_methods[] = {
	UBUS_METHOD("send", seriald_send_data, data_policy),
};

static struct ubus_object_type seriald_object_type =
	UBUS_OBJECT_TYPE("serial", seriald_object_methods);

static struct ubus_object seriald_object = {
	.name = "serial",
	.type = &seriald_object_type,
	.methods = seriald_object_methods,
	.n_methods = ARRAY_SIZE(seriald_object_methods),
};

static int seriald_send_data(
		struct ubus_context *ctx, struct ubus_object *obj,
		struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__DATA_MAX];
	int len;

	blobmsg_parse(data_policy, __DATA_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[DATA_DATA]) return UBUS_STATUS_INVALID_ARGUMENT;

	const char *data = blobmsg_get_string(tb[DATA_DATA]);
	len = strlen(data);

	pthread_mutex_lock(&tty_q_mutex);
	if (tty_q.len + len < TTY_Q_SZ) {
		memmove(tty_q.buff + tty_q.len, data, len);
		tty_q.len += len;
		tty_q.buff[tty_q.len] = '\n';
		++tty_q.len;
		eventfd_write(efd_notify_tty, 1);
	}
	pthread_mutex_unlock(&tty_q_mutex);

	return UBUS_STATUS_OK;
}

static void seriald_ubus_reconnect_timer(struct uloop_timeout *timeout)
{
	static struct uloop_timeout retry = {
		.cb = seriald_ubus_reconnect_timer,
	};
	int t = 2;

	if (ubus_reconnect(ubus_ctx, ubus_path)) {
		DPRINTF("failed to reconnect, trying again in %d seconds\n", t);
		uloop_timeout_set(&retry, t * 1000);
		return;
	}

	DPRINTF("reconnected to ubus, new id: %08x\n", ubus_ctx->local_id);
	seriald_ubus_add_fd();
}

static void seriald_ubus_connection_lost_cb(struct ubus_context *ctx)
{
	seriald_ubus_reconnect_timer(NULL);
}

static void seriald_ubus_add_fd(void)
{
	ubus_add_uloop(ubus_ctx);
	fcntl(ubus_ctx->sock.fd, F_SETFD,
			fcntl(ubus_ctx->sock.fd, F_GETFD) | FD_CLOEXEC);
}

int seriald_ubus_loop_init(const char *path)
{
	int r;

	uloop_init();
	ubus_path = path;

	ubus_ctx = ubus_connect(path);
	if (!ubus_ctx) {
		DPRINTF("cannot connect to ubus\n");
		return -EIO;
	}

	DPRINTF("connected as %08x\n", ubus_ctx->local_id);
	ubus_ctx->connection_lost = seriald_ubus_connection_lost_cb;
	seriald_ubus_add_fd();

	r = ubus_add_object(ubus_ctx, &seriald_object);
	if (r) {
		DPRINTF("Failed to add object: %s\n", ubus_strerror(r));
		return r;
	}

	return 0;
}

void *seriald_ubus_loop(void *unused)
{
	uloop_run();
	return 0;
}

void seriald_ubus_loop_done(void)
{
	ubus_free(ubus_ctx);
}

void seriald_ubus_loop_stop(void)
{
	uloop_end();
}
