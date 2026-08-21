#include "async_validate.h"
#include "reactor.h"
#include "service.h"

#include <arpa/inet.h>
#include <assert.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct validation_result {
    reactor_t *reactor;
    int calls;
    int success;
};

static void validation_done(int result, const char *output, void *userdata) {
    (void)output;
    struct validation_result *state = userdata;
    state->calls++;
    state->success = result;
    reactor_stop(state->reactor);
}

static void unused_timer(reactor_t *reactor, reactor_timer_t *timer, void *userdata) {
    (void)reactor;
    (void)timer;
    (void)userdata;
    assert(0);
}

static int listen_loopback(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) }
    };
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(fd, 1) == 0);

    socklen_t size = sizeof(address);
    assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    *port = ntohs(address.sin_port);
    return fd;
}

int main(void) {
    reactor_t *reactor = reactor_create();
    assert(reactor);

    reactor_timer_t *cancelled = reactor_add_timer(
        reactor, 60000, 0, unused_timer, NULL);
    assert(cancelled);
    assert(reactor_cancel_timer(reactor, cancelled) == 0);

    int port;
    int listener = listen_loopback(&port);
    service_ctx_t service = {
        .api_port = port,
        .child_pid = -1,
        .state = SERVICE_STOPPED,
        .reactor = reactor
    };
    assert(service_start_async(&service) == -1);
    assert(service.state == SERVICE_FAILED);
    assert(strstr(service.last_error, "already in use"));
    close(listener);

    async_validate_ctx_t validation;
    struct validation_result result = { .reactor = reactor };
    assert(async_validate_config(&validation, reactor, "/bin/true", "/tmp",
                                 "/dev/null", validation_done, &result) == 0);

    pid_t managed = fork();
    assert(managed >= 0);
    if (managed == 0) {
        pause();
        _exit(0);
    }
    service.child_pid = managed;
    usleep(100000);
    service_sigchld_cb(reactor, SIGCHLD, &service);

    reactor_run(reactor);
    assert(result.calls == 1);
    assert(result.success == 1);
    assert(validation.child_pid == -1);

    service.stop_timeout_sec = 1;
    service_cleanup(&service);
    assert(service.child_pid == -1);
    assert(waitpid(managed, NULL, WNOHANG) == -1);
    reactor_destroy(reactor);
    return 0;
}
