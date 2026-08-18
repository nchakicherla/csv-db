#include <stdio.h>

#include "csvdb/csvdb.h"

int main(void) {
    printf("csvdb %s\n", csvdb_version());
    return 0;
}
