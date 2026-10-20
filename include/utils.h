#ifndef ATP_UTILS_H
#define ATP_UTILS_H

#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

#define PIDOF_BUF_SIZE  256
#define VERSION_BUF_SIZE 128
#define UPTIME_BUF_SIZE 64
#define MAX_CMD_LEN     512
#define MAX_OUTPUT_LEN  4096
int file_exists(const char *path);
int mkdir_recursive(const char *path, mode_t mode);
void trim(char *str);

int process_exists(pid_t pid);
int get_process_starttime(pid_t pid, unsigned long long *out_ticks);
int get_process_threads(pid_t pid);
int get_process_fd_count(pid_t pid);
double get_process_cpu_percent(pid_t pid);
int get_process_uptime_sec(pid_t pid);

int get_app_dir(char *buf, size_t size);
int atp_timezone_init(void);
int atp_timezone_get_name(char *buf, size_t size);
long atp_timezone_get_offset_sec(void);
#endif
