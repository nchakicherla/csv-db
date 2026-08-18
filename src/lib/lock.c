#include "lock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

bool lock_acquire(const char *lock_path, LockMode mode, TableLock *out,
                   char *errbuf, size_t errlen) {
    int fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        snprintf(errbuf, errlen, "cannot open lock file \"%s\": %s", lock_path, strerror(errno));
        return false;
    }

    int op = (mode == LOCK_SHARED) ? LOCK_SH : LOCK_EX;
    if (flock(fd, op) != 0) {
        snprintf(errbuf, errlen, "cannot lock \"%s\": %s", lock_path, strerror(errno));
        close(fd);
        return false;
    }

    out->fd = fd;
    return true;
}

void lock_release(TableLock *lock) {
    if (lock == NULL || lock->fd < 0) {
        return;
    }
    flock(lock->fd, LOCK_UN);
    close(lock->fd);
    lock->fd = -1;
}
