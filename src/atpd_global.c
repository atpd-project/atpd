#include "atpd_global.h"

atpd_global_t g_atpd = {
    .running = 1,
    .reload = 0,
    .show_status = 0,
    .reactor = NULL,
    .svc = NULL
};	
