#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../omiotdb_rest.h"

typedef struct {
	char url[2048];
	char username[128];
	char password[128];
	char body[OMIOTDB_MAX_REQUEST];
	char response[256];
	long timeout_ms;
	long http_status;
	int fail_transport;
} mock_http;

static int mock_post(void *context, const char *url, const char *username,
		const char *password, long timeout_ms, const char *body,
		long *http_status, char *response, size_t response_capacity,
		char *error, size_t error_capacity)
{
	mock_http *mock = (mock_http *) context;
	snprintf(mock->url, sizeof(mock->url), "%s", url);
	snprintf(mock->username, sizeof(mock->username), "%s", username);
	snprintf(mock->password, sizeof(mock->password), "%s", password);
	snprintf(mock->body, sizeof(mock->body), "%s", body);
	mock->timeout_ms = timeout_ms;
	if(mock->fail_transport) {
		if(error != NULL && error_capacity > 0)
			snprintf(error, error_capacity, "mock transport failure");
		return 0;
	}
	*http_status = mock->http_status;
	if(response_capacity > 0)
		snprintf(response, response_capacity, "%s", mock->response);
	return 1;
}

static void test_config_validation(void)
{
	char error[256] = {0};
	assert(iotdb_valid_identifier("db_7"));
	assert(!iotdb_valid_identifier("7db"));
	assert(!iotdb_valid_identifier("db-name"));
	assert(iotdb_validate_config("https://iotdb.example:18080/api/", "telemetry",
		"root", "root", 2500, error, sizeof(error)));
	assert(iotdb_validate_config("http://[::1]:18080", "telemetry",
		"root", "root", 2500, error, sizeof(error)));
	assert(!iotdb_validate_config("http:///path", "telemetry", "root", "root",
		2500, error, sizeof(error)));
	assert(!iotdb_validate_config("http://:18080", "telemetry", "root", "root",
		2500, error, sizeof(error)));
	assert(!iotdb_validate_config("ftp://iotdb.example", "telemetry", "root", "root",
		2500, error, sizeof(error)));
	assert(!iotdb_validate_config("http://iotdb.example:70000", "telemetry", "root", "root",
		2500, error, sizeof(error)));
	assert(!iotdb_validate_config("http://user@iotdb.example", "telemetry", "root", "root",
		2500, error, sizeof(error)));
	assert(!iotdb_validate_config("http://iotdb.example/?x=1", "telemetry", "root", "root",
		2500, error, sizeof(error)));
	assert(!iotdb_validate_config("http://iotdb.example", "bad-name", "root", "root",
		2500, error, sizeof(error)));
	assert(!iotdb_validate_config("http://iotdb.example", "telemetry", "root:user", "root",
		2500, error, sizeof(error)));
	assert(!iotdb_validate_config("http://iotdb.example", "telemetry", "root", "root",
		99, error, sizeof(error)));
}

static void test_schema_and_template(void)
{
	char error[256] = {0};
	char sql[1024];
	const char expectedPrefix[] = "INSERT INTO readings(time, device_id, label, logged_at) VALUES (1, 'device-1', 'O''Reilly', '2026-11-16 10:00:00.";
	const char *formatted =
		"\"device-1\":\"O'Reilly\":\"16/Nov/2026:10:00:00 +0800\"*:*:"
		"INSERT INTO readings(time, device_id, label, logged_at) VALUES (1, '*1*', '*2*', '*time_local_to_ts_3*')";
	assert(iotdb_build_create_table_sql("readings",
		"time TIMESTAMP TIME,device_id STRING TAG,label STRING FIELD",
		sql, sizeof(sql), error, sizeof(error)));
	assert(strcmp(sql, "CREATE TABLE IF NOT EXISTS readings (time TIMESTAMP TIME,device_id STRING TAG,label STRING FIELD)") == 0);
	assert(!iotdb_build_create_table_sql("readings", "time INT32 FIELD; DROP TABLE readings",
		sql, sizeof(sql), error, sizeof(error)));
	assert(!iotdb_build_create_table_sql("readings", "time INT32 FIELD --comment",
		sql, sizeof(sql), error, sizeof(error)));
	assert(!iotdb_build_create_table_sql("readings", "time INT32 FIELD", NULL, 10,
		error, sizeof(error)));
	assert(iotdb_expand_template(formatted, sql, sizeof(sql), error, sizeof(error)));
	assert(strncmp(sql, expectedPrefix, sizeof(expectedPrefix) - 1) == 0);
	assert(strlen(sql) == sizeof(expectedPrefix) - 1 + 8);
	assert(sql[sizeof(expectedPrefix) - 1 + 6] == '\'');
	assert(sql[sizeof(expectedPrefix) - 1 + 7] == ')');
	assert(!iotdb_expand_template("\"device\"*:*:SELECT *0*", sql, sizeof(sql), error, sizeof(error)));
	assert(!iotdb_expand_template("\"device\"*:*:SELECT *+1*", sql, sizeof(sql), error, sizeof(error)));
	assert(!iotdb_expand_template("\"device\"*:*:SELECT *2*", sql, sizeof(sql), error, sizeof(error)));
	assert(!iotdb_expand_template("\"device\"*:*:SELECT *1", sql, sizeof(sql), error, sizeof(error)));
	assert(!iotdb_expand_template("\"device\"*:*:SELECT *1*", sql, 4, error, sizeof(error)));
	assert(!iotdb_expand_template("\"not a timestamp\"*:*:SELECT '*time_local_to_ts_1*'", sql, sizeof(sql), error, sizeof(error)));
}

static void test_request_execution(void)
{
	mock_http mock = {0};
	char error[256] = {0};
	char json[512];
	assert(iotdb_build_request_json("db", "INSERT INTO t VALUES ('a\"b')", json, sizeof(json)));
	assert(strcmp(json, "{\"database\":\"db\",\"sql\":\"INSERT INTO t VALUES ('a\\\"b')\"}") == 0);
	assert(iotdb_build_request_json("", "CREATE DATABASE IF NOT EXISTS db", json, sizeof(json)));
	assert(strcmp(json, "{\"database\":\"\",\"sql\":\"CREATE DATABASE IF NOT EXISTS db\"}") == 0);
	assert(!iotdb_build_request_json("database-too-long", "sql", json, 8));

	mock.http_status = 200;
	snprintf(mock.response, sizeof(mock.response), "{\"message\":\"ok\",\"details\":{\"x\":[1,2]},\"code\":200}");
	assert(iotdb_execute("https://iotdb.example/prefix/", "db", "root", "root",
		2500, "INSERT INTO t VALUES (1)", mock_post, &mock, error, sizeof(error)));
	assert(strcmp(mock.url, "https://iotdb.example/prefix/rest/table/v1/nonQuery") == 0);
	assert(strcmp(mock.username, "root") == 0);
	assert(strcmp(mock.password, "root") == 0);
	assert(mock.timeout_ms == 2500);
	assert(strcmp(mock.body, "{\"database\":\"db\",\"sql\":\"INSERT INTO t VALUES (1)\"}") == 0);

	assert(iotdb_execute("https://iotdb.example", "", "root", "root", 2500,
		"CREATE DATABASE IF NOT EXISTS db", mock_post, &mock, error, sizeof(error)));
	assert(strcmp(mock.body, "{\"database\":\"\",\"sql\":\"CREATE DATABASE IF NOT EXISTS db\"}") == 0);

	mock.http_status = 503;
	assert(!iotdb_execute("https://iotdb.example", "db", "root", "root", 2500,
		"SELECT 1", mock_post, &mock, error, sizeof(error)));
	assert(strstr(error, "HTTP status 503") != NULL);
	mock.http_status = 200;
	snprintf(mock.response, sizeof(mock.response), "{\"code\":801,\"message\":\"unauthorized\"}");
	assert(!iotdb_execute("https://iotdb.example", "db", "root", "root", 2500,
		"SELECT 1", mock_post, &mock, error, sizeof(error)));
	assert(strstr(error, "code 801") != NULL);
	snprintf(mock.response, sizeof(mock.response), "{}");
	assert(!iotdb_execute("https://iotdb.example", "db", "root", "root", 2500,
		"SELECT 1", mock_post, &mock, error, sizeof(error)));
	assert(strstr(error, "without a valid code") != NULL);
	snprintf(mock.response, sizeof(mock.response), "{\"code\":200} trailing");
	assert(!iotdb_execute("https://iotdb.example", "db", "root", "root", 2500,
		"SELECT 1", mock_post, &mock, error, sizeof(error)));
	snprintf(mock.response, sizeof(mock.response), "{\"code\":200,\"x\":truish}");
	assert(!iotdb_execute("https://iotdb.example", "db", "root", "root", 2500,
		"SELECT 1", mock_post, &mock, error, sizeof(error)));
	snprintf(mock.response, sizeof(mock.response), "{\"code\":200,\"code\":200}");
	assert(!iotdb_execute("https://iotdb.example", "db", "root", "root", 2500,
		"SELECT 1", mock_post, &mock, error, sizeof(error)));
	mock.fail_transport = 1;
	assert(!iotdb_execute("https://iotdb.example", "db", "root", "root", 2500,
		"SELECT 1", mock_post, &mock, error, sizeof(error)));
	assert(strcmp(error, "mock transport failure") == 0);
}

int main(void)
{
	test_config_validation();
	test_schema_and_template();
	test_request_execution();
	puts("IoTDB REST helper tests passed");
	return 0;
}
