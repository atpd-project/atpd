#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

int main(void) {
    unsigned long long self_start = 0;
    if (get_process_starttime(getpid(), &self_start) != 0 || self_start == 0) abort();

    /* Save original process name */
    char orig_name[16] = {0};
    if (prctl(PR_GET_NAME, orig_name, 0, 0, 0) != 0) abort();

    /* Linux comm permits spaces and ')' inside the /proc stat wrapper.
     * Setting the comm directly on self tests /proc/<pid>/stat parsing
     * without subprocess fork/exec, ensuring ThreadSanitizer safety. */
    if (prctl(PR_SET_NAME, "worker ) odd", 0, 0, 0) != 0) abort();

    unsigned long long odd_start = 0;
    if (get_process_starttime(getpid(), &odd_start) != 0 || odd_start != self_start) abort();

    /* Restore original name */
    if (prctl(PR_SET_NAME, orig_name, 0, 0, 0) != 0) abort();

    unsigned long long restored_start = 0;
    if (get_process_starttime(getpid(), &restored_start) != 0 || restored_start != self_start) abort();

    return 0;
}
