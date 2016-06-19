#ifndef __SERIALD_UBUS_LOOP_H
#define __SERIALD_UBUS_LOOP_H

int seriald_ubus_loop_init(const char *path);
void seriald_ubus_loop_done(void);
void *seriald_ubus_loop(void *unused);
void seriald_ubus_loop_stop(void);

#endif /* __SERIALD_UBUS_LOOP_H */
