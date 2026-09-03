#include "utils.h"

#include <assert.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "--child") == 0) {
        /* Linux comm permits spaces and ')' inside the /proc stat wrapper. */
        (void)prctl(PR_SET_NAME, "worker ) odd", 0, 0, 0);
        char ch;
        (void)read(STDIN_FILENO, &ch, 1);
        _exit(0);
    }

    unsigned long long self_start = 0;
    assert(get_process_starttime(getpid(), &self_start) == 0);
    assert(self_start != 0);

    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sync_pipe[1]);
        if (dup2(sync_pipe[0], STDIN_FILENO) < 0) _exit(126);
        close(sync_pipe[0]);
        execl(argv[0], argv[0], "--child", (char *)NULL);
        _exit(127);
    }
    close(sync_pipe[0]);

    unsigned long long child_start = 0;
    for (int i = 0; i < 100 && child_start == 0; i++) {
        (void)get_process_starttime(child, &child_start);
        if (child_start == 0) usleep(1000);
    }
    assert(child_start != 0);

    unsigned long long child_start_again = 0;
    assert(get_process_starttime(child, &child_start_again) == 0);
    assert(child_start_again == child_start);

    close(sync_pipe[1]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    (void)self_start;
    (void)child_start_again;
    (void)status;
    return 0;
}
