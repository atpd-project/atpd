#ifndef ATP_EPOLL_H
#define ATP_EPOLL_H

int epoll_init(void);
void epoll_cleanup(void);
int epoll_add_fd(int fd, void (*callback)(int fd, void *data), void *data);
int epoll_remove_fd(int fd);
void epoll_run(void);
void epoll_stop(void);
int epoll_run_once(int timeout_ms);
#endif
