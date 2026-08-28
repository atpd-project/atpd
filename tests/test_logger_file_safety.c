#include "logger.h"
#include "utils.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int atp_timezone_init(void) { return 0; }
int get_app_dir(char *buf, size_t size) {
    if (size > 0) buf[0] = '\0';
    return 0;
}
int mkdir_recursive(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    return 0;
}

static int target_is_unchanged(const char *path) {
    char buf[16] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t count = read(fd, buf, sizeof(buf));
    close(fd);
    return count == 5 && memcmp(buf, "safe\n", 5) == 0;
}

int main(void) {
    char dir[] = "/tmp/atpd-log-safety.XXXXXX";
    if (!mkdtemp(dir)) return 1;

    char target[256];
    char symlink_path[256];
    char hardlink_path[256];
    snprintf(target, sizeof(target), "%s/target", dir);
    snprintf(symlink_path, sizeof(symlink_path), "%s/symlink", dir);
    snprintf(hardlink_path, sizeof(hardlink_path), "%s/hardlink", dir);

    int fd = open(target, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || write(fd, "safe\n", 5) != 5 || close(fd) != 0) return 1;
    if (symlink(target, symlink_path) != 0 || link(target, hardlink_path) != 0) return 1;

    log_set_target(LOG_TARGET_FILE);
    log_set_file(symlink_path);
    log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, "must not follow symlink");
    if (!target_is_unchanged(target)) return 1;

    log_set_file(hardlink_path);
    log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, "must not follow hardlink");
    logger_close();
    if (!target_is_unchanged(target)) return 1;

    unlink(symlink_path);
    unlink(hardlink_path);
    unlink(target);
    rmdir(dir);
    puts("logger file safety tests passed");
    return 0;
}
