#include "utils.h"

#include <stdlib.h>
#include <unistd.h>

int main(void) {
    /* 1. NULL pointer parameter check */
    if (get_process_starttime(getpid(), NULL) == 0) abort();

    /* 2. Negative PID check */
    unsigned long long dummy = 0;
    if (get_process_starttime(-1, &dummy) == 0) abort();

    /* 3. Non-existent large PID check */
    if (get_process_starttime(99999999, &dummy) == 0) abort();

    /* 4. Valid self PID check */
    unsigned long long self_start1 = 0;
    if (get_process_starttime(getpid(), &self_start1) != 0 || self_start1 == 0) abort();

    /* 5. Repeatability: consecutive reads must be identical */
    unsigned long long self_start2 = 0;
    if (get_process_starttime(getpid(), &self_start2) != 0 || self_start2 != self_start1) abort();

    /* 6. System root / init PID 1 check */
    unsigned long long init_start = 0;
    (void)get_process_starttime(1, &init_start);

    return 0;
}
