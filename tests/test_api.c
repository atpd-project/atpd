#include "api.h"
#include "reactor.h"

#include <arpa/inet.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct callback_state {
    api_ctx_t *api;
    reactor_t *reactor;
    int calls;
    int timeout_calls;
};

static void stalled_response_cb(int http_code, const char *body, void *userdata) {
    struct callback_state *state = userdata;
    assert(http_code == 0);
    assert(body == NULL);
    state->timeout_calls++;
    reactor_stop(state->reactor);
}

static void response_cb(int http_code, const char *body, void *userdata) {
    struct callback_state *state = userdata;
    assert(http_code == 200);
    assert(body && strcmp(body, "{\"version\":\"test\"}") == 0);
    state->calls++;
    if (state->calls == 1) {
        assert(api_get_version_async(state->api, response_cb, state) == 0);
    } else {
        assert(api_get_version_async(state->api, stalled_response_cb, state) == 0);
    }
}

static void timeout_cb(reactor_t *reactor, reactor_timer_t *timer, void *userdata) {
    (void)timer;
    (void)userdata;
    reactor_stop(reactor);
}

static int listen_loopback(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) }
    };
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(fd, 2) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    *port = ntohs(address.sin_port);
    return fd;
}

static void serve_responses(int listener) {
    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 18\r\n"
        "Connection: close\r\n\r\n"
        "{\"version\":\"test\"}";
    for (int i = 0; i < 3; i++) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) _exit(1);
        char request[1024];
        if (read(client, request, sizeof(request)) <= 0) {
            _exit(1);
        }
        if (i == 2) pause();
        if (write(client, response, sizeof(response) - 1) != sizeof(response) - 1) _exit(1);
        close(client);
    }
    close(listener);
    _exit(0);
}

int main(void) {
    int port;
    int listener = listen_loopback(&port);
    pid_t server = fork();
    assert(server >= 0);
    if (server == 0) serve_responses(listener);
    close(listener);

    atp_config_t config = {0};
    snprintf(config.api.host, sizeof(config.api.host), "127.0.0.1");
    config.api.port = port;

    reactor_t *reactor = reactor_create();
    assert(reactor);
    api_ctx_t api;
    api_ctx_t temporary;
    assert(api_init(&api, &config) == 0);
    assert(api_init(&temporary, &config) == 0);
    assert(api_start_with_reactor(&api, reactor) == 0);
    assert(api_start_with_reactor(&temporary, reactor) == 0);
    api_cleanup(&temporary);
    assert(api.reactor == reactor);
    api.timeout_sec = 1;

    struct callback_state state = { .api = &api, .reactor = reactor };
    assert(api_get_version_async(&api, response_cb, &state) == 0);
    assert(reactor_add_timer(reactor, 5000, 0, timeout_cb, NULL));
    assert(reactor_run(reactor) == 0);
    assert(state.calls == 2);
    assert(state.timeout_calls == 1);

    api_cleanup(&api);
    reactor_destroy(reactor);
    kill(server, SIGTERM);
    int status;
    assert(waitpid(server, &status, 0) == server);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    return 0;
}
