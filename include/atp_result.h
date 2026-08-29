#ifndef ATP_RESULT_H
#define ATP_RESULT_H

typedef enum {
    ATP_OK = 0,
    ATP_ERR_GENERAL = -1,
    ATP_ERR_NOMEM = -2,
    ATP_ERR_NOENT = -3,
    ATP_ERR_PERM = -4,
    ATP_ERR_TIMEOUT = -5,
    ATP_ERR_BUSY = -6,
    ATP_ERR_INVAL = -7,
    ATP_ERR_IO = -8,
    ATP_ERR_CONFIG = -9,
    ATP_ERR_NOTSUP = -10,
} atp_result_t;

static inline const char *atp_result_string(atp_result_t result) {
    switch (result) {
        case ATP_OK:           return "Success";
        case ATP_ERR_GENERAL:  return "General error";
        case ATP_ERR_NOMEM:    return "Out of memory";
        case ATP_ERR_NOENT:    return "Not found";
        case ATP_ERR_PERM:     return "Permission denied";
        case ATP_ERR_TIMEOUT:  return "Operation timed out";
        case ATP_ERR_BUSY:     return "Resource busy";
        case ATP_ERR_INVAL:    return "Invalid argument";
        case ATP_ERR_IO:       return "I/O error";
        case ATP_ERR_CONFIG:   return "Configuration error";
        case ATP_ERR_NOTSUP:   return "Operation not supported";
        default:               return "Unknown error";
    }
}

#endif
