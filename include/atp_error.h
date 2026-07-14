#ifndef ATP_ERROR_H
#define ATP_ERROR_H

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
    ATP_ERR_EBPF = -10,
    ATP_ERR_SERVICE = -11,
    ATP_ERR_NETLINK = -12,
} atp_error_t;

static inline const char* atp_strerror(int err) {
    switch (err) {
        case ATP_OK:           return "Success";
        case ATP_ERR_GENERAL:  return "General error";
        case ATP_ERR_NOMEM:    return "Out of memory";
        case ATP_ERR_NOENT:    return "File not found";
        case ATP_ERR_PERM:     return "Permission denied";
        case ATP_ERR_TIMEOUT:  return "Operation timed out";
        case ATP_ERR_BUSY:     return "Resource busy";
        case ATP_ERR_INVAL:    return "Invalid argument";
        case ATP_ERR_IO:       return "I/O error";
        case ATP_ERR_CONFIG:   return "Configuration error";
        case ATP_ERR_EBPF:     return "eBPF error";
        case ATP_ERR_SERVICE:  return "Service error";
        case ATP_ERR_NETLINK:  return "Netlink error";
        default:               return "Unknown error";
    }
}

#endif
