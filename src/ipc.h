#ifndef BSPWM_IPC_H
#define BSPWM_IPC_H

#include <stdbool.h>

void ipc_accept(int sock_fd);
bool ipc_handle(int fd);    /* false: fd is not a control socket client */
int ipc_timeout(void);      /* epoll_wait timeout in ms, -1 for none */
void ipc_expire(void);
void ipc_close_all(void);

#endif
