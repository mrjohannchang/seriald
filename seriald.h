#ifndef __SERIALD_H
#define __SERIALD_H

#include <pthread.h>

#define DPRINTF(format, ...) fprintf(stderr, "%s(%d): " format, __func__, __LINE__, ## __VA_ARGS__)

#ifndef TTY_Q_SZ
#define TTY_Q_SZ 1024
#endif

#define TTY_RD_SZ 256

struct tty_q {
	int len;
	char buff[TTY_Q_SZ];
} tty_q;

extern struct tty_q tty_q;
extern pthread_mutex_t tty_q_mutex;
void fatal(const char *format, ...);

extern int sig_exit;

extern int efd_notify_tty;
extern int efd_signal;
extern int ubus_pipefd[];

#endif /* __SERIALD_H */
