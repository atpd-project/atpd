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
int exec_cmd(const char *cmd, char *output, size_t output_size, int timeout_sec);
int exec_cmd_simple(const char *cmd, int timeout_sec);
int exec_cmd_argv(const char *cmd_path, char *const argv[], char *output, size_t output_size, int timeout_sec);
int read_file(const char *path, char *buf, size_t buf_size);
int write_file(const char *path, const char *content);
int is_number(const char *str);
void trim(char *str);
int starts_with(const char *str, const char *prefix);
int ends_with(const char *str, const char *suffix);
char *str_replace(const char *str, const char *old, const char *new_str);
int find_command_path(const char *name, char *out_path, size_t out_size);

int get_pid_by_name(const char *name);
int process_exists(pid_t pid);
long get_process_memory_kb(pid_t pid);
int get_process_threads(pid_t pid);
int get_process_fd_count(pid_t pid);
double get_process_cpu_percent(pid_t pid);
int get_process_uptime_sec(pid_t pid);
int get_process_user_group(pid_t pid, char *user, char *group, size_t size);
int get_binary_version(const char *bin_path, char *version, size_t size);
void format_uptime(int seconds, char *buf, size_t size);

int kill_process(pid_t pid, int signal);
int kill_all_by_name(const char *name, int signal);
int wait_for_pid_exit(pid_t pid, int timeout_sec);
int check_ip6tables_available(void);
#endif
