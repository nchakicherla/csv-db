#ifndef CSVDB_EXECUTOR_H
#define CSVDB_EXECUTOR_H

#include <stdbool.h>
#include <stddef.h>

#include "ast.h"
#include "catalog.h"
#include "result.h"

/* Executes one parsed statement against `catalog`. For SELECT, *out_result
 * is set to a newly allocated Result (caller result_free's it); for every
 * other statement kind *out_result is set to NULL. If out_affected_rows is
 * non-NULL, it's set to the number of rows inserted/updated/deleted (0 for
 * CREATE/DROP/SELECT). Returns false and fills errbuf on any error. */
bool executor_exec(Catalog *catalog, const Statement *stmt, Result **out_result,
                    size_t *out_affected_rows, char *errbuf, size_t errlen);

#endif /* CSVDB_EXECUTOR_H */
