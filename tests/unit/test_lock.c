#include "test_util.h"

#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lock.h"

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    memcpy(copy, s, n);
    return copy;
}

static char *make_temp_lock_path(void) {
    char template_buf[] = "/tmp/csvdb_test_lock_XXXXXX";
    int fd = mkstemp(template_buf);
    if (fd < 0) {
        return NULL;
    }
    close(fd);
    return dup_str(template_buf);
}

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

/* Forks a child that acquires `child_mode` on `path`, signals the parent
 * once it holds the lock, then holds it for ~300ms before releasing. The
 * parent waits for that signal, then times how long its own `path`
 * acquisition under `parent_mode` takes. Returns the elapsed seconds. */
static double time_contended_acquire(const char *path, LockMode child_mode, LockMode parent_mode) {
    int fds[2];
    if (pipe(fds) != 0) {
        return -1.0;
    }

    pid_t pid = fork();
    TEST_CHECK(pid >= 0);
    if (pid < 0) {
        return -1.0;
    }

    if (pid == 0) {
        close(fds[0]);
        char err[128];
        TableLock lock;
        if (!lock_acquire(path, child_mode, &lock, err, sizeof(err))) {
            _exit(1);
        }
        char sig = 'x';
        if (write(fds[1], &sig, 1) != 1) {
            _exit(1);
        }
        struct timespec hold = {0, 300L * 1000 * 1000};
        nanosleep(&hold, NULL);
        lock_release(&lock);
        close(fds[1]);
        _exit(0);
    }

    close(fds[1]);
    char sig = 0;
    TEST_CHECK(read(fds[0], &sig, 1) == 1);
    close(fds[0]);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    char err[128];
    TableLock lock;
    bool ok = lock_acquire(path, parent_mode, &lock, err, sizeof(err));
    clock_gettime(CLOCK_MONOTONIC, &end);
    TEST_CHECK(ok);
    if (ok) {
        lock_release(&lock);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    return elapsed_seconds(start, end);
}

static void test_exclusive_excludes_writer(void) {
    char *path = make_temp_lock_path();
    TEST_CHECK(path != NULL);

    double elapsed = time_contended_acquire(path, LOCK_EXCLUSIVE, LOCK_EXCLUSIVE);
    /* the child held its exclusive lock for ~300ms; the parent's own
     * exclusive acquire must have blocked for a comparable stretch */
    TEST_CHECK(elapsed >= 0.2);

    remove(path);
    free(path);
}

static void test_exclusive_excludes_reader(void) {
    char *path = make_temp_lock_path();
    TEST_CHECK(path != NULL);

    double elapsed = time_contended_acquire(path, LOCK_EXCLUSIVE, LOCK_SHARED);
    TEST_CHECK(elapsed >= 0.2);

    remove(path);
    free(path);
}

static void test_shared_allows_concurrent_readers(void) {
    char *path = make_temp_lock_path();
    TEST_CHECK(path != NULL);

    double elapsed = time_contended_acquire(path, LOCK_SHARED, LOCK_SHARED);
    /* SH+SH must not exclude: the parent should return almost
     * immediately, not wait out the child's ~300ms hold */
    TEST_CHECK(elapsed >= 0.0 && elapsed < 0.2);

    remove(path);
    free(path);
}

int main(void) {
    test_exclusive_excludes_writer();
    test_exclusive_excludes_reader();
    test_shared_allows_concurrent_readers();

    if (test_failures == 0) {
        printf("all lock tests passed\n");
    }
    return TEST_MAIN_RETURN();
}
