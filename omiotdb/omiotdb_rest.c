#include "omiotdb_rest.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IOTDB_MAX_FIELDS 1024
#define IOTDB_MAX_RESPONSE 8192

typedef struct {
	const char *start;
	size_t length;
} field_t;

static void set_error(char *error, size_t capacity, const char *message)
{
	if(error != NULL && capacity > 0)
		snprintf(error, capacity, "%s", message);
}

int iotdb_valid_identifier(const char *value)
{
	const unsigned char *cursor = (const unsigned char *) value;

	if(cursor == NULL || !((*cursor >= 'a' && *cursor <= 'z') ||
		(*cursor >= 'A' && *cursor <= 'Z') || *cursor == '_'))
		return 0;
	for(++cursor; *cursor != '\0'; ++cursor) {
		if(!((*cursor >= 'a' && *cursor <= 'z') ||
			(*cursor >= 'A' && *cursor <= 'Z') ||
			(*cursor >= '0' && *cursor <= '9') || *cursor == '_'))
			return 0;
	}
	return 1;
}

static int valid_server_url(const char *server)
{
	const char *authority;
	const char *authority_end;
	const char *host_end;
	const char *cursor;
	size_t scheme_length;

	if(server == NULL)
		return 0;
	if(strncmp(server, "http://", 7) == 0)
		scheme_length = 7;
	else if(strncmp(server, "https://", 8) == 0)
		scheme_length = 8;
	else
		return 0;
	authority = server + scheme_length;
	authority_end = authority + strcspn(authority, "/?#");
	if(authority == authority_end || memchr(authority, '@', (size_t)(authority_end - authority)) != NULL)
		return 0;

	if(*authority == '[') {
		host_end = memchr(authority + 1, ']', (size_t)(authority_end - authority - 1));
		if(host_end == NULL || host_end == authority + 1)
			return 0;
		for(cursor = authority + 1; cursor < host_end; ++cursor) {
			if(!((*cursor >= '0' && *cursor <= '9') ||
				(*cursor >= 'a' && *cursor <= 'f') ||
				(*cursor >= 'A' && *cursor <= 'F') || *cursor == ':' || *cursor == '.'))
				return 0;
		}
		cursor = host_end + 1;
	} else {
		host_end = authority;
		while(host_end < authority_end && *host_end != ':')
			++host_end;
		if(host_end == authority)
			return 0;
		for(cursor = authority; cursor < host_end; ++cursor) {
			if(!((*cursor >= 'a' && *cursor <= 'z') ||
				(*cursor >= 'A' && *cursor <= 'Z') ||
				(*cursor >= '0' && *cursor <= '9') || *cursor == '.' || *cursor == '-'))
				return 0;
		}
		cursor = host_end;
	}

	if(cursor < authority_end) {
		unsigned long port = 0;
		if(*cursor++ != ':' || cursor == authority_end)
			return 0;
		for(; cursor < authority_end; ++cursor) {
			if(*cursor < '0' || *cursor > '9')
				return 0;
			port = port * 10 + (unsigned long)(*cursor - '0');
			if(port > 65535)
				return 0;
		}
		if(port == 0)
			return 0;
	}

	for(cursor = server; *cursor != '\0'; ++cursor) {
		if(isspace((unsigned char)*cursor) || (unsigned char)*cursor < 0x20 || *cursor == '#')
			return 0;
	}
	return strchr(server, '?') == NULL;
}

static int validate_endpoint_config(const char *server, const char *username,
		const char *password, long timeout_ms, char *error, size_t error_capacity)
{
	if(!valid_server_url(server)) {
		set_error(error, error_capacity, "server must be an absolute http or https URL with a valid host and port");
		return 0;
	}
	if(username == NULL || password == NULL || username[0] == '\0' ||
		strchr(username, ':') != NULL || strchr(username, '\r') != NULL || strchr(username, '\n') != NULL ||
		password[0] == '\0' || strchr(password, '\r') != NULL || strchr(password, '\n') != NULL) {
		set_error(error, error_capacity, "username and password must be set and valid for HTTP Basic authentication");
		return 0;
	}
	if(timeout_ms < 100 || timeout_ms > 600000) {
		set_error(error, error_capacity, "timeout must be between 100 and 600000 milliseconds");
		return 0;
	}
	return 1;
}

int iotdb_validate_config(const char *server, const char *database,
		const char *username, const char *password, long timeout_ms,
		char *error, size_t error_capacity)
{
	if(database == NULL || !validate_endpoint_config(server, username, password,
		timeout_ms, error, error_capacity))
		return 0;
	if(!iotdb_valid_identifier(database)) {
		set_error(error, error_capacity, "database must be a plain IoTDB table-model identifier");
		return 0;
	}
	return 1;
}

int iotdb_build_create_table_sql(const char *table, const char *columns,
		char *output, size_t output_capacity,
		char *error, size_t error_capacity)
{
	const char *cursor;
	int written;

	if(output == NULL || output_capacity == 0) {
		set_error(error, error_capacity, "table SQL output buffer is required");
		return 0;
	}
	output[0] = '\0';
	if(!iotdb_valid_identifier(table)) {
		set_error(error, error_capacity, "table must be a plain IoTDB table identifier");
		return 0;
	}
	if(columns == NULL || columns[0] == '\0') {
		set_error(error, error_capacity, "columns schema is required");
		return 0;
	}
	for(cursor = columns; *cursor != '\0'; ++cursor) {
		if(*cursor == ';' || *cursor == '\n' || *cursor == '\r' ||
			(unsigned char)*cursor < 0x20) {
			set_error(error, error_capacity, "columns schema must be a single statement without control characters");
			return 0;
		}
	}
	if(strstr(columns, "--") != NULL || strstr(columns, "/*") != NULL || strstr(columns, "*/") != NULL) {
		set_error(error, error_capacity, "columns schema cannot contain SQL comments");
		return 0;
	}
	written = snprintf(output, output_capacity, "CREATE TABLE IF NOT EXISTS %s (%s)", table, columns);
	if(written < 0 || (size_t) written >= output_capacity) {
		set_error(error, error_capacity, "table schema exceeds the SQL buffer limit");
		return 0;
	}
	return 1;
}

static int append_char(char *output, size_t capacity, size_t *length, char value)
{
	if(*length + 1 >= capacity)
		return 0;
	output[(*length)++] = value;
	output[*length] = '\0';
	return 1;
}

static int append_sql_value(char *output, size_t capacity, size_t *length,
		const char *value, size_t value_length)
{
	size_t i;
	for(i = 0; i < value_length; ++i) {
		if(value[i] == '\'') {
			if(!append_char(output, capacity, length, '\'') ||
				!append_char(output, capacity, length, '\''))
				return 0;
		} else if(!append_char(output, capacity, length, value[i])) {
			return 0;
		}
	}
	return 1;
}

static int append_time_local(char *output, size_t capacity, size_t *length,
		const char *value, size_t value_length)
{
	static const char *months[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	static volatile unsigned int counter;
	char timestamp[27];
	const char *month;
	unsigned int sequence;
	int month_number;

	if(value_length != 26 || value[2] != '/' || value[6] != '/' ||
		value[11] != ':' || value[14] != ':' || value[17] != ':' ||
		value[20] != ' ' || (value[21] != '+' && value[21] != '-') ||
		!isdigit((unsigned char)value[0]) || !isdigit((unsigned char)value[1]) ||
		!isdigit((unsigned char)value[7]) || !isdigit((unsigned char)value[8]) ||
		!isdigit((unsigned char)value[9]) || !isdigit((unsigned char)value[10]) ||
		!isdigit((unsigned char)value[12]) || !isdigit((unsigned char)value[13]) ||
		!isdigit((unsigned char)value[15]) || !isdigit((unsigned char)value[16]) ||
		!isdigit((unsigned char)value[18]) || !isdigit((unsigned char)value[19]) ||
		!isdigit((unsigned char)value[22]) || !isdigit((unsigned char)value[23]) ||
		!isdigit((unsigned char)value[24]) || !isdigit((unsigned char)value[25]))
		return 0;
	month = value + 3;
	for(month_number = 0; month_number < 12; ++month_number) {
		if(memcmp(month, months[month_number], 3) == 0)
			break;
	}
	if(month_number == 12)
		return 0;
	timestamp[0] = value[7];
	timestamp[1] = value[8];
	timestamp[2] = value[9];
	timestamp[3] = value[10];
	timestamp[4] = '-';
	timestamp[5] = (char)('0' + (month_number + 1) / 10);
	timestamp[6] = (char)('0' + (month_number + 1) % 10);
	timestamp[7] = '-';
	timestamp[8] = value[0];
	timestamp[9] = value[1];
	timestamp[10] = ' ';
	memcpy(timestamp + 11, value + 12, 8);
	timestamp[19] = '.';
	sequence = __sync_add_and_fetch(&counter, 1) % 1000000;
	snprintf(timestamp + 20, sizeof(timestamp) - 20, "%06u", sequence);
	return append_sql_value(output, capacity, length, timestamp, 26);
}

int iotdb_expand_template(const char *formatted, char *sql, size_t sql_capacity,
		char *error, size_t error_capacity)
{
	static const char delimiter[] = "*:*:";
	field_t fields[IOTDB_MAX_FIELDS];
	const char *cursor;
	const char *sql_template = NULL;
	size_t field_count = 0;
	size_t output_length = 0;

	if(formatted == NULL || sql == NULL || sql_capacity == 0) {
		set_error(error, error_capacity, "formatted message and output buffer are required");
		return 0;
	}
	sql[0] = '\0';
	cursor = formatted;
	while(*cursor != '\0') {
		if(strncmp(cursor, delimiter, sizeof(delimiter) - 1) == 0) {
			sql_template = cursor + sizeof(delimiter) - 1;
			break;
		}
		if(*cursor == '"') {
			const char *start = ++cursor;
			int escaped = 0;
			while(*cursor != '\0') {
				if(escaped) {
					escaped = 0;
				} else if(*cursor == '\\') {
					escaped = 1;
				} else if(*cursor == '"') {
					break;
				}
				++cursor;
			}
			if(*cursor != '"' || field_count >= IOTDB_MAX_FIELDS) {
				set_error(error, error_capacity, "message contains an unterminated or excessive quoted field");
				return 0;
			}
			fields[field_count].start = start;
			fields[field_count].length = (size_t) (cursor - start);
			++field_count;
		}
		++cursor;
	}
	if(sql_template == NULL || sql_template[0] == '\0') {
		set_error(error, error_capacity, "formatted message must contain *:*: followed by an IoTDB SQL template");
		return 0;
	}

	for(cursor = sql_template; *cursor != '\0'; ++cursor) {
		if(*cursor != '*') {
			if(!append_char(sql, sql_capacity, &output_length, *cursor))
				goto too_long;
			continue;
		}
		{
			const char *end = strchr(cursor + 1, '*');
			char *number_end;
			char number[48];
			const char *field_number;
			long index;
			int time_local = 0;
			size_t number_length;
			if(end == NULL) {
				set_error(error, error_capacity, "SQL template contains an unmatched '*' placeholder");
				return 0;
			}
			number_length = (size_t) (end - cursor - 1);
			if(number_length == 0 || number_length >= sizeof(number)) {
				set_error(error, error_capacity, "SQL template placeholders must be numeric message indexes");
				return 0;
			}
			memcpy(number, cursor + 1, number_length);
			number[number_length] = '\0';
			if(strncmp(number, "time_local_to_ts_", sizeof("time_local_to_ts_") - 1) == 0) {
				time_local = 1;
				field_number = number + sizeof("time_local_to_ts_") - 1;
			} else {
				field_number = number;
			}
			for(number_end = (char *)field_number; *number_end != '\0'; ++number_end) {
				if(*number_end < '0' || *number_end > '9') {
					set_error(error, error_capacity, "SQL template placeholders must be positive numeric message indexes");
					return 0;
				}
			}
			errno = 0;
			index = strtol(field_number, &number_end, 10);
			if(errno == ERANGE || number_end == field_number || *number_end != '\0' ||
				index < 1 || (size_t) index > field_count) {
				set_error(error, error_capacity, "SQL template references a missing or invalid message field");
				return 0;
			}
			if(time_local) {
				if(!append_time_local(sql, sql_capacity, &output_length,
					fields[index - 1].start, fields[index - 1].length)) {
					set_error(error, error_capacity, "time_local_to_ts placeholder requires an Apache access-log timestamp");
					return 0;
				}
			} else if(!append_sql_value(sql, sql_capacity, &output_length,
				fields[index - 1].start, fields[index - 1].length)) {
				goto too_long;
			}
			cursor = end;
		}
	}
	if(output_length == 0) {
		set_error(error, error_capacity, "SQL template expanded to an empty statement");
		return 0;
	}
	return 1;

too_long:
	set_error(error, error_capacity, "expanded SQL exceeds the configured buffer limit");
	return 0;
}

static int append_json_string(char *output, size_t capacity, size_t *length, const char *value)
{
	const unsigned char *cursor = (const unsigned char *) value;
	if(!append_char(output, capacity, length, '"'))
		return 0;
	for(; *cursor != '\0'; ++cursor) {
		char escaped[7];
		switch(*cursor) {
		case '"':
		case '\\':
			if(!append_char(output, capacity, length, '\\') ||
				!append_char(output, capacity, length, (char) *cursor))
				return 0;
			break;
		case '\b': if(!append_char(output, capacity, length, '\\') || !append_char(output, capacity, length, 'b')) return 0; break;
		case '\f': if(!append_char(output, capacity, length, '\\') || !append_char(output, capacity, length, 'f')) return 0; break;
		case '\n': if(!append_char(output, capacity, length, '\\') || !append_char(output, capacity, length, 'n')) return 0; break;
		case '\r': if(!append_char(output, capacity, length, '\\') || !append_char(output, capacity, length, 'r')) return 0; break;
		case '\t': if(!append_char(output, capacity, length, '\\') || !append_char(output, capacity, length, 't')) return 0; break;
		default:
			if(*cursor < 0x20) {
				snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned int) *cursor);
				if(strlen(escaped) >= capacity - *length)
					return 0;
				memcpy(output + *length, escaped, strlen(escaped));
				*length += strlen(escaped);
				output[*length] = '\0';
			} else if(!append_char(output, capacity, length, (char) *cursor)) {
				return 0;
			}
		}
	}
	return append_char(output, capacity, length, '"');
}

int iotdb_build_request_json(const char *database, const char *sql,
		char *output, size_t output_capacity)
{
	size_t length = 0;
	static const char prefix[] = "{\"database\":";
	if(database == NULL || sql == NULL || output == NULL || output_capacity == 0)
		return 0;
	output[0] = '\0';
	if(sizeof(prefix) - 1 >= output_capacity)
		return 0;
	memcpy(output, prefix, sizeof(prefix) - 1);
	length = sizeof(prefix) - 1;
	if(!append_json_string(output, output_capacity, &length, database) ||
		!append_char(output, output_capacity, &length, ',') ||
		!append_char(output, output_capacity, &length, '"') ||
		!append_char(output, output_capacity, &length, 's') ||
		!append_char(output, output_capacity, &length, 'q') ||
		!append_char(output, output_capacity, &length, 'l') ||
		!append_char(output, output_capacity, &length, '"') ||
		!append_char(output, output_capacity, &length, ':') ||
		!append_json_string(output, output_capacity, &length, sql) ||
		!append_char(output, output_capacity, &length, '}'))
		return 0;
	return 1;
}

static const char *skip_ws(const char *cursor)
{
	while(*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
		++cursor;
	return cursor;
}

static int parse_json_string(const char **cursor, char *output, size_t output_capacity)
{
	size_t length = 0;
	const char *p = *cursor;
	if(*p != '"')
		return 0;
	++p;
	while(*p != '\0' && *p != '"') {
		char value = *p++;
		if((unsigned char)value < 0x20)
			return 0;
		if(value == '\\') {
			if(*p == '\0')
				return 0;
			value = *p++;
			if(value == 'u') {
				int i;
				for(i = 0; i < 4; ++i) {
					if(*p == '\0' || !isxdigit((unsigned char)*p))
						return 0;
					++p;
				}
				value = '?';
			} else if(value == 'n') value = '\n';
			else if(value == 'r') value = '\r';
			else if(value == 't') value = '\t';
			else if(value != '"' && value != '\\' && value != '/') return 0;
		}
		if(output != NULL) {
			if(length + 1 >= output_capacity)
				return 0;
			output[length] = value;
		}
		++length;
	}
	if(*p != '"')
		return 0;
	++p;
	if(output != NULL)
		output[length] = '\0';
	*cursor = p;
	return 1;
}

static int skip_json_value(const char **cursor, int depth)
{
	const char *p = skip_ws(*cursor);
	if(depth > 32)
		return 0;
	if(*p == '"') {
		if(!parse_json_string(&p, NULL, 0))
			return 0;
	} else if(*p == '{' || *p == '[') {
		char close = *p++ == '{' ? '}' : ']';
		p = skip_ws(p);
		if(*p == close) {
			++p;
			*cursor = p;
			return 1;
		}
		while(*p != close) {
			if(*p == '\0') return 0;
			if(close == '}') {
				if(!parse_json_string(&p, NULL, 0)) return 0;
				p = skip_ws(p);
				if(*p++ != ':') return 0;
			}
			if(!skip_json_value(&p, depth + 1)) return 0;
			p = skip_ws(p);
			if(*p == close) break;
			if(*p++ != ',') return 0;
			p = skip_ws(p);
		}
		++p;
	} else {
		const char *start = p;
		if(strncmp(p, "true", 4) == 0)
			p += 4;
		else if(strncmp(p, "false", 5) == 0)
			p += 5;
		else if(strncmp(p, "null", 4) == 0)
			p += 4;
		else {
			if(*p == '-')
				++p;
			if(*p == '0') {
				++p;
				if(*p >= '0' && *p <= '9')
					return 0;
			} else {
				if(*p < '1' || *p > '9')
					return 0;
				while(*p >= '0' && *p <= '9')
					++p;
			}
			if(*p == '.') {
				++p;
				if(*p < '0' || *p > '9')
					return 0;
				while(*p >= '0' && *p <= '9')
					++p;
			}
			if(*p == 'e' || *p == 'E') {
				++p;
				if(*p == '+' || *p == '-')
					++p;
				if(*p < '0' || *p > '9')
					return 0;
				while(*p >= '0' && *p <= '9')
					++p;
			}
		}
		if(p == start || (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
			!isspace((unsigned char)*p)))
			return 0;
	}
	*cursor = p;
	return 1;
}

static int parse_status_code(const char *response, long *code)
{
	const char *p = skip_ws(response);
	int found_code = 0;
	if(*p++ != '{')
		return 0;
	p = skip_ws(p);
	if(*p == '}')
		return 0;
	for(;;) {
		char key[32];
		if(!parse_json_string(&p, key, sizeof(key)))
			return 0;
		p = skip_ws(p);
		if(*p++ != ':')
			return 0;
		p = skip_ws(p);
		if(strcmp(key, "code") == 0) {
			char text[32];
			char *parseEnd;
			if(found_code)
				return 0;
			if(*p == '"') {
				if(!parse_json_string(&p, text, sizeof(text))) return 0;
				errno = 0;
				*code = strtol(text, &parseEnd, 10);
				if(errno == ERANGE || parseEnd == text || *parseEnd != '\0') return 0;
			} else {
				const char *numberStart = p;
				if(*p == '-')
					++p;
				if(*p < '0' || *p > '9')
					return 0;
				if(*p == '0' && p[1] >= '0' && p[1] <= '9')
					return 0;
				while(*p >= '0' && *p <= '9')
					++p;
				if(*p != ',' && *p != '}' && !isspace((unsigned char)*p))
					return 0;
				errno = 0;
				*code = strtol(numberStart, &parseEnd, 10);
				if(errno == ERANGE || parseEnd != p)
					return 0;
			}
			found_code = 1;
		} else if(!skip_json_value(&p, 0)) {
			return 0;
		}
		p = skip_ws(p);
		if(*p == '}') {
			++p;
			p = skip_ws(p);
			return found_code && *p == '\0';
		}
		if(*p++ != ',') return 0;
		p = skip_ws(p);
	}
}

int iotdb_response_ok(long http_status, const char *response,
		char *error, size_t error_capacity)
{
	long code;
	if(http_status < 200 || http_status >= 300) {
		if(error != NULL && error_capacity > 0)
			snprintf(error, error_capacity, "IoTDB REST returned HTTP status %ld", http_status);
		return 0;
	}
	if(response == NULL || !parse_status_code(response, &code)) {
		set_error(error, error_capacity, "IoTDB REST returned a response without a valid code field");
		return 0;
	}
	if(code != 200) {
		if(error != NULL && error_capacity > 0)
			snprintf(error, error_capacity, "IoTDB REST rejected the request with code %ld", code);
		return 0;
	}
	return 1;
}

int iotdb_execute(const char *server, const char *database,
		const char *username, const char *password, long timeout_ms,
		const char *sql, iotdb_post_fn post, void *context,
		char *error, size_t error_capacity)
{
	char url[2048];
	char body[OMIOTDB_MAX_REQUEST];
	char response[IOTDB_MAX_RESPONSE];
	long http_status = 0;
	int written;
	if(!validate_endpoint_config(server, username, password, timeout_ms, error, error_capacity))
		return 0;
	if(database == NULL || (database[0] != '\0' && !iotdb_valid_identifier(database))) {
		set_error(error, error_capacity, "database must be empty only for CREATE DATABASE or a plain IoTDB identifier");
		return 0;
	}
	if(post == NULL || sql == NULL || sql[0] == '\0') {
		set_error(error, error_capacity, "SQL statement and HTTP transport are required");
		return 0;
	}
	written = snprintf(url, sizeof(url), "%s%srest/table/v1/nonQuery", server,
		server[strlen(server) - 1] == '/' ? "" : "/");
	if(written < 0 || (size_t) written >= sizeof(url) ||
		!iotdb_build_request_json(database, sql, body, sizeof(body))) {
		set_error(error, error_capacity, "IoTDB REST request exceeds the configured buffer limit");
		return 0;
	}
	response[0] = '\0';
	if(!post(context, url, username, password, timeout_ms, body, &http_status,
		response, sizeof(response), error, error_capacity)) {
		if(error == NULL || error_capacity == 0 || error[0] == '\0')
			set_error(error, error_capacity, "IoTDB REST transport failed");
		return 0;
	}
	response[sizeof(response) - 1] = '\0';
	return iotdb_response_ok(http_status, response, error, error_capacity);
}
