#ifndef CSVDB_TEST_UTIL_H
#define CSVDB_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_failures = 0;

#define TEST_CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            test_failures++; \
        } \
    } while (0)

#define TEST_CHECK_STR_EQ(actual, expected) \
    do { \
        const char *_a = (actual); \
        const char *_e = (expected); \
        if (_a == NULL || _e == NULL || strcmp(_a, _e) != 0) { \
            fprintf(stderr, "FAIL %s:%d: expected \"%s\", got \"%s\"\n", \
                    __FILE__, __LINE__, _e ? _e : "(null)", _a ? _a : "(null)"); \
            test_failures++; \
        } \
    } while (0)

#define TEST_MAIN_RETURN() (test_failures != 0 ? 1 : 0)

/* Reads a fixture file whole. Caller frees. Returns NULL on any I/O error. */
static inline char *read_file_to_string(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

#endif /* CSVDB_TEST_UTIL_H */
