#ifndef CSVDB_LOCK_H
#define CSVDB_LOCK_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LOCK_SHARED,
    LOCK_EXCLUSIVE
} LockMode;

typedef struct {
    int fd;
} TableLock;

/* Acquires a blocking flock() on `lock_path` (the file is created if it
 * doesn't exist yet). Advisory only: this protects cooperating csvdb
 * processes on the same host, not arbitrary writers, and is not reliable
 * over NFS or other network filesystems -- this design assumes a local
 * (or otherwise POSIX-flock-correct) filesystem. */
bool lock_acquire(const char *lock_path, LockMode mode, TableLock *out,
                   char *errbuf, size_t errlen);

void lock_release(TableLock *lock);

#endif /* CSVDB_LOCK_H */
