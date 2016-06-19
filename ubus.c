#define _GNU_SOURCE
#include <libubus.h>
#include <libubox/blobmsg_json.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "seriald.h"
#include "strutils.h"
#include "ubus.h"

static void seriald_ubus_send_event(const char *json);
static void ubus_send_event_line_parser(const int n, const char *buff_rd);

static struct ubus_context *ubus_ctx = NULL;
static struct blob_buf b;
static const char *ubus_sock;

static void seriald_ubus_send_event(const char *json)
{
	blob_buf_init(&b, 0);

	if (!blobmsg_add_json_from_string(&b, json)) {
		DPRINTF("cannot parse data for ubus send event\n");
		return;
	}

	if (ubus_send_event(ubus_ctx, "serial", b.head)) {
		ubus_free(ubus_ctx);
		ubus_ctx = ubus_connect(ubus_sock);
		ubus_send_event(ubus_ctx, "serial", b.head);
	}
}

static void ubus_send_event_line_parser(const int n, const char *buff_rd)
{
	static char line_buff[TTY_RD_SZ+1] = "";
	const char *p;
	const char *pp;
	char buff[TTY_RD_SZ+1] = "";

	/* buff_rd isn't null-terminated */
	strncat(buff, buff_rd, n);

	pp = buff;

	while ((p = strchr(pp, '\n'))) {
		if (strlen(line_buff) + p - pp > TTY_RD_SZ) {
			seriald_ubus_send_event(line_buff);
			*line_buff = '\0';
		}

		strncat(line_buff, pp, p - pp);
		strchrdel(line_buff, '\r');
		seriald_ubus_send_event(line_buff);
		*line_buff = '\0';

		pp = p + 1;
	}

	if (pp) {
		if (strlen(line_buff) + n - (pp - buff) > TTY_RD_SZ) {
			seriald_ubus_send_event(line_buff);
			*line_buff = '\0';
		}
		strncat(line_buff, pp, n - (pp - buff));
		strchrdel(line_buff, '\r');
	}
}

void seriald_ubus_run(const char *path)
{
	fd_set rdset;
	int r;
	int n;
	char buff_rd[TTY_RD_SZ];
	int max_fd;
	eventfd_t efd_value;

	ubus_sock = path;
	max_fd = (efd_signal > ubus_pipefd[0]) ? efd_signal : ubus_pipefd[0];

	ubus_ctx = ubus_connect(ubus_sock);
	if (!ubus_ctx) {
		fatal("cannot connect to ubus");
	}

	while (!sig_exit) {
		FD_ZERO(&rdset);
		FD_SET(efd_signal, &rdset);
		FD_SET(ubus_pipefd[0], &rdset);

		r = select(max_fd + 1, &rdset, NULL, NULL, NULL);
		if (r < 0)  {
			if (errno == EINTR) continue;
			else fatal("select failed: %d : %s", errno, strerror(errno));
		}

		if (FD_ISSET(ubus_pipefd[0], &rdset)) {
			/* read from pipe */
			do {
				n = read(ubus_pipefd[0], &buff_rd, sizeof(buff_rd));
			} while (n < 0 && errno == EINTR);
			if (n == 0) {
				fatal("pipe closed");
			} else if (n < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK)
					fatal("read from pipe failed: %s", strerror(errno));
			} else {
				ubus_send_event_line_parser(n, buff_rd);
			}
		}

		if (FD_ISSET(efd_signal, &rdset)) {
			/* Being self-piped */
			if (eventfd_read(efd_signal, &efd_value)) {
				fatal("failed to read efd_signal");
			}
		}
	}
}
