/* EasyRadio core platform for the Linux host tests and the dry run (libc). */
#define _GNU_SOURCE
#include "../src/er_core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int er_test_quiet;

void *er_alloc(size_t size) {
    return calloc(1, size ? size : 1);
}
void er_free(void *memory) {
    free(memory);
}
void er_log_line(const char *line) {
    if (!er_test_quiet) printf("  log: %s\n", line);
}
struct ErFile {
    int fd;
};
ErFile *er_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || S_ISDIR(st.st_mode)) {
        close(fd);
        return NULL;
    }
    ErFile *f = malloc(sizeof(*f));
    f->fd = fd;
    return f;
}
uint64_t er_file_size(ErFile *f) {
    struct stat st;
    return fstat(f->fd, &st) ? 0 : (uint64_t)st.st_size;
}
int er_read_at(ErFile *f, uint64_t offset, void *buffer, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(f->fd, (char *)buffer + done, size - done, (off_t)(offset + done));
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return 1;
}
void er_close(ErFile *f) {
    if (f) {
        close(f->fd);
        free(f);
    }
}
int er_write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}
int er_make_dirs(const char *path) {
    char copy[4096];
    size_t n = strlen(path);
    if (n >= sizeof(copy)) return 0;
    memcpy(copy, path, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (copy[i] == '/' || copy[i] == 0) {
            char c = copy[i];
            copy[i] = 0;
            if (mkdir(copy, 0755) && errno != EEXIST) return 0;
            copy[i] = c;
        }
    }
    return 1;
}
int er_delete_file(const char *path) {
    return unlink(path) == 0 || errno == ENOENT;
}
int er_list_dir(const char *path, ErVisit visit, void *context) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        visit(e->d_name, S_ISDIR(st.st_mode), (uint64_t)st.st_size, (uint64_t)st.st_mtime, context);
    }
    closedir(d);
    return 1;
}
uint64_t er_random_seed(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec ^ (uint64_t)getpid() << 32;
}
