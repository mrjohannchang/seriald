#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <unistd.h>

#include "fdio.h"
#include "seriald.h"
#include "strutils.h"
#include "term.h"
#include "ubus.h"
#include "ubus_loop.h"

int efd_notify_tty = -1;
int efd_signal = -1;
int ubus_pipefd[2];
static int fd_tty;

#define STO STDOUT_FILENO
#define STI STDIN_FILENO
#define TTY_WRITE_SZ_DIV 10
#define TTY_WRITE_SZ_MIN 8
int tty_write_sz;
#define set_tty_write_sz(baud) \
	do { \
		tty_write_sz = (baud) / TTY_WRITE_SZ_DIV; \
		if (tty_write_sz < TTY_WRITE_SZ_MIN) tty_write_sz = TTY_WRITE_SZ_MIN; \
	} while (0)
struct tty_q tty_q;
pthread_mutex_t tty_q_mutex;

int sig_exit = 0;

static struct {
	char port[128];
	int baud;
	enum flowcntrl_e flow;
	enum parity_e parity;
	int databits;
	int stopbits;
	int noreset;
	char *socket;
} opts = {
	.port = "",
	.baud = 115200,
	.flow = FC_NONE,
	.parity = P_NONE,
	.databits = 8,
	.stopbits = 1,
	.noreset = 0,
	.socket = NULL, /* the library fall back to default socket when it is NULL */
};

static void show_usage(void);
static void parse_args(int argc, char *argv[]);
static void deadly_handler(int signum);
static void register_signal_handlers(void);
static void loop(void);
static void tty_read_line_parser(const int n, const char *buff_rd);
static void tty_read_line_cb(const char *line);
int main(int argc, char *argv[]);

static void show_usage()
{
	printf("Usage: seriald [options] <TTY device>\n");
	printf("\n");
	printf("Options:\n");
	printf("  -b <baudrate>\n");
	printf("    baudrate should be one of: 0, 50, 75, 110, 134, 150, 200, 300, 600,\n");
	printf("    1200, 1800, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400\n");
	printf("    default to 115200\n");
	printf("  -f s (=soft) | h (=hard) | n (=none)\n");
	printf("    default to n\n");
	printf("  -s <ubus socket>\n");
	printf("    no need to give if you use the default one\n");
	printf("\n");
	printf("Examples:\n");
	printf("  Get data:\n");
	printf("    ubus listen serial\n");
	printf("  Send data (replace <data> with the data you want to send):\n");
	printf("    ubus call serial send '{\"data\": \"<data>\"}'\n");
}

void fatal(const char *format, ...)
{
	char *s, buf[256];
	va_list args;
	int len;

	va_start(args, format);
	len = vsnprintf(buf, sizeof(buf), format, args);
	buf[sizeof(buf) - 1] = '\0';
	va_end(args);
	
	s = "\r\nFATAL: ";
	writen_ni(STO, s, strlen(s));
	writen_ni(STO, buf, len);
	s = "\r\n";
	writen_ni(STO, s, strlen(s));

	/* wait a bit for output to drain */
	sleep(1);

	exit(EXIT_FAILURE);
}

static void parse_args(int argc, char *argv[])
{
	int c;
	int r = 0;

	while ((c = getopt(argc, argv, "hf:b:s:")) != -1) {
		switch (c) {
			case 'f':
				switch (optarg[0]) {
					case 'X':
					case 'x':
						opts.flow = FC_XONXOFF;
						break;
					case 'H':
					case 'h':
						opts.flow = FC_RTSCTS;
						break;
					case 'N':
					case 'n':
						opts.flow = FC_NONE;
						break;
					default:
						DPRINTF("Invalid flow control: %c\n", optarg[0]);
						r = -1;
						break;
				}
				break;
			case 'b':
				opts.baud = atoi(optarg);
				if (opts.baud == 0 || !term_baud_ok(opts.baud)) {
					DPRINTF("Invalid baud rate: %d\n", opts.baud);
					r = -1;
				}
				break;
			case 's':
				opts.socket = optarg;
				break;
			case 'h':
				r = 1;
				break;
			default:
				r = -1;
				break;
		}
	}

	if (r) {
		show_usage();
		exit((r > 0) ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	if ((argc - optind) < 1) {
		DPRINTF("No port given\n");
		show_usage();
		exit(EXIT_FAILURE);
	}

	strncpy(opts.port, argv[optind], sizeof(opts.port) - 1);
	opts.port[sizeof(opts.port) - 1] = '\0';
}

static void deadly_handler(int signum)
{
	DPRINTF("seriald is signaled with TERM\n");
	if (!sig_exit) {
		seriald_ubus_loop_stop();
		sig_exit = 1;
		if (efd_notify_tty >= 0) eventfd_write(efd_notify_tty, 1);
		if (efd_signal >= 0) eventfd_write(efd_signal, 1);
	}
}

static void register_signal_handlers(void)
{
	struct sigaction exit_action, ign_action;

	/* Set up the structure to specify the exit action. */
	exit_action.sa_handler = deadly_handler;
	sigemptyset (&exit_action.sa_mask);
	exit_action.sa_flags = 0;

	/* Set up the structure to specify the ignore action. */
	ign_action.sa_handler = SIG_IGN;
	sigemptyset (&ign_action.sa_mask);
	ign_action.sa_flags = 0;

	sigaction(SIGTERM, &exit_action, NULL);

	sigaction(SIGALRM, &ign_action, NULL);
	sigaction(SIGHUP, &ign_action, NULL);
	sigaction(SIGINT, &ign_action, NULL);
	sigaction(SIGPIPE, &ign_action, NULL);
	sigaction(SIGQUIT, &ign_action, NULL);
	sigaction(SIGUSR1, &ign_action, NULL);
	sigaction(SIGUSR2, &ign_action, NULL);
}

static void loop(void)
{
	fd_set rdset, wrset;
	int r;
	int n;
	char buff_rd[TTY_RD_SZ];
	int write_sz;
	int max_fd;
	eventfd_t efd_value;

	max_fd = (fd_tty > efd_notify_tty) ? fd_tty : efd_notify_tty;
	tty_q.len = 0;

	while (!sig_exit) {
		FD_ZERO(&rdset);
		FD_ZERO(&wrset);
		FD_SET(fd_tty, &rdset);
		FD_SET(efd_notify_tty, &rdset);

		pthread_mutex_lock(&tty_q_mutex);
		if (tty_q.len) FD_SET(fd_tty, &wrset);
		pthread_mutex_unlock(&tty_q_mutex);

		r = select(max_fd + 1, &rdset, &wrset, NULL, NULL);
		if (r < 0)  {
			if (errno == EINTR) continue;
			else fatal("select failed: %d : %s", errno, strerror(errno));
		}

		if (FD_ISSET(fd_tty, &rdset)) {
			/* read from port */
			do {
				n = read(fd_tty, &buff_rd, sizeof(buff_rd));
			} while (n < 0 && errno == EINTR);
			if (n == 0) {
				fatal("term closed");
			} else if (n < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK)
					fatal("read from term failed: %s", strerror(errno));
			} else {
				tty_read_line_parser(n, buff_rd);
			}
		}

		if (FD_ISSET(efd_notify_tty, &rdset)) {
			/* Being notified we have something to write to TTY */
			if (eventfd_read(efd_notify_tty, &efd_value)) {
				fatal("failed to read efd_notify_tty");
			}
		}

		if (FD_ISSET(fd_tty, &wrset)) {
			/* write to port */
			pthread_mutex_lock(&tty_q_mutex);
			write_sz = (tty_q.len < tty_write_sz) ? tty_q.len : tty_write_sz;
			do {
				n = write(fd_tty, tty_q.buff, write_sz);
			} while (n < 0 && errno == EINTR);
			if (n <= 0) fatal("write to term failed: %s", strerror(errno));
			memmove(tty_q.buff, tty_q.buff + n, tty_q.len - n);
			tty_q.len -= n;
			pthread_mutex_unlock(&tty_q_mutex);
		}
	}
}

static void tty_read_line_parser(const int n, const char *buff_rd)
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
			tty_read_line_cb(line_buff);
			*line_buff = '\0';
		}

		strncat(line_buff, pp, p - pp);
		strchrdel(line_buff, '\r');
		tty_read_line_cb(line_buff);
		*line_buff = '\0';

		pp = p + 1;
	}

	if (pp) {
		if (strlen(line_buff) + n - (pp - buff) > TTY_RD_SZ) {
			tty_read_line_cb(line_buff);
			*line_buff = '\0';
		}
		strncat(line_buff, pp, n - (pp - buff));
		strchrdel(line_buff, '\r');
	}
}

static void tty_read_line_cb(const char *line)
{
	char format[] = "{\"data\": \"%s\"}\n";
	char json[sizeof(format)+TTY_RD_SZ];
	char *p;
	int n;
	int sz;

	sprintf(json, format, line);
	p = json;
	sz = strlen(json);

	while (sz > 0) {
		do {
			n = write(ubus_pipefd[1], p, sz);
		} while (n < 0 && errno == EINTR);
		if (n <= 0) fatal("write to pipe failed: %s", strerror(errno));
		p += n;
		sz -= n;
	}
}

int main(int argc, char *argv[])
{
	int r;
	pthread_t uloop_tid;

	parse_args(argc, argv);
	register_signal_handlers();

	r = pipe2(ubus_pipefd, O_CLOEXEC);
	if (r < 0) fatal("cannot create pipe to ubus: %s", strerror(errno));

	/* Seems like you cannot have multiple ubus connections in single process. */
	/* So we fork. */
	switch(fork()) {
		case 0:
			efd_signal = eventfd(0, EFD_CLOEXEC);
			if (efd_signal < 0) {
				fatal("cannot create efd_signal: %s", strerror(errno));
			}
			close(ubus_pipefd[1]);
			seriald_ubus_run(opts.socket);
			return EXIT_SUCCESS;
		case -1:
			fatal("cannot fork ubus_event_loop");
	}

	close(ubus_pipefd[0]);

	efd_notify_tty = eventfd(0, EFD_CLOEXEC);
	if (efd_notify_tty < 0) {
		fatal("cannot create efd_notify_tty: %s", strerror(errno));
	}

	r = term_lib_init();
	if (r < 0) fatal("term_init failed: %s", term_strerror(term_errno, errno));

	fd_tty = open(opts.port, O_RDWR | O_NONBLOCK | O_NOCTTY);
	if (fd_tty < 0) fatal("cannot open %s: %s", opts.port, strerror(errno));

	r = term_set(fd_tty,
			1,              /* raw mode. */
			opts.baud,      /* baud rate. */
			opts.parity,    /* parity. */
			opts.databits,  /* data bits. */
			opts.stopbits,  /* stop bits. */
			opts.flow,      /* flow control. */
			1,              /* local or modem */
			!opts.noreset); /* hup-on-close. */
	if (r < 0) {
		fatal("failed to add device %s: %s",
				opts.port, term_strerror(term_errno, errno));
	}

	r = term_apply(fd_tty, 0);
	if (r < 0) {
		fatal("failed to config device %s: %s",
				opts.port, term_strerror(term_errno, errno));
	}

	set_tty_write_sz(term_get_baudrate(fd_tty, NULL));

	r = seriald_ubus_loop_init(opts.socket);
	if (r) fatal("failed to connect to ubus");

	r = pthread_create(&uloop_tid, NULL, &seriald_ubus_loop, NULL);
	if (r) fatal("can't create thread for uloop: %s", strerror(r));

	loop();

	seriald_ubus_loop_done();
	return EXIT_SUCCESS;
}
