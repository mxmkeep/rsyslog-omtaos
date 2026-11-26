#ifndef OMIOTDB_REST_H
#define OMIOTDB_REST_H

#include <stddef.h>

#define OMIOTDB_MAX_REQUEST 32768

typedef int (*iotdb_post_fn)(void *context,
		const char *url,
		const char *username,
		const char *password,
		long timeout_ms,
		const char *body,
		long *http_status,
		char *response,
		size_t response_capacity,
		char *error,
		size_t error_capacity);

int iotdb_valid_identifier(const char *value);
int iotdb_validate_config(const char *server, const char *database,
		const char *username, const char *password, long timeout_ms,
		char *error, size_t error_capacity);
int iotdb_build_create_table_sql(const char *table, const char *columns,
		char *output, size_t output_capacity,
		char *error, size_t error_capacity);
int iotdb_expand_template(const char *formatted, char *sql, size_t sql_capacity,
		char *error, size_t error_capacity);
int iotdb_build_request_json(const char *database, const char *sql,
		char *output, size_t output_capacity);
int iotdb_response_ok(long http_status, const char *response,
		char *error, size_t error_capacity);
int iotdb_execute(const char *server, const char *database,
		const char *username, const char *password, long timeout_ms,
		const char *sql, iotdb_post_fn post, void *context,
		char *error, size_t error_capacity);

#endif
