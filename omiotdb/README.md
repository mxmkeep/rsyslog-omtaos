# rsyslog omiotdb output module

`omiotdb` is a standalone rsyslog output module for Apache IoTDB's Table model. It sends one SQL statement per message to `POST /rest/table/v1/nonQuery`, uses HTTP Basic authentication, and rejects both non-2xx HTTP responses and IoTDB application response codes other than 200.

The action accepts these RainerScript parameters:

| Parameter | Required | Meaning |
| --- | --- | --- |
| `server` | yes | Absolute `http://` or `https://` base URL, optionally including a path prefix |
| `db` | yes | IoTDB Table-model database identifier |
| `table` | yes | Table identifier created at action start |
| `columns` | yes | Table column definitions, such as `time TIMESTAMP TIME,device_id STRING TAG,value DOUBLE FIELD` |
| `uid` / `pwd` | yes | HTTP Basic username and password |
| `template` | yes | SQL-enabled rsyslog template; its rendered text must follow the format below |
| `timeout` | no | Connect and total timeout in milliseconds; defaults to 5000, valid range is 100 to 600000 |

At the first transaction for each worker, the module sends `CREATE DATABASE IF NOT EXISTS` and `CREATE TABLE IF NOT EXISTS`. Each rendered message must contain zero or more double-quoted fields followed by `*:*:` and a SQL template. `*1*`, `*2*`, and later placeholders insert those fields in order; apostrophes in values are escaped for SQL string literals. The existing `*time_local_to_ts_N*` placeholder is supported for Apache access-log timestamps (`dd/Mon/yyyy:HH:mm:ss +zzzz`) and expands to a timestamp with six unique fractional digits.

## Build

The source targets the rsyslog 8.x output-module API used by the reference `omtaos` module.

1. Copy `omiotdb` under `rsyslog/plugins/`.
2. Add `plugins/omiotdb` to the top-level `SUBDIRS` in `Makefile.am`, guarded by a new `ENABLE_IOTDB` conditional.
3. In `configure.ac`, add an `--enable-iotdb` option and `AM_CONDITIONAL(ENABLE_IOTDB, test x$enable_iotdb = xyes)`, add `plugins/omiotdb/Makefile` to `AC_CONFIG_FILES`, and report the option in configure output.
4. Regenerate the build system with `./autogen.sh`, then configure with `--enable-iotdb`. Install the libcurl development headers and library before configuring.
5. Build and run the standalone helper tests with `make -C plugins/omiotdb check`.

## RainerScript example

The template below illustrates the exact wire format with a fixed sample. Replace the quoted fields with properties from the input template and keep their order aligned with the placeholders.

```rainerscript
module(load="omiotdb")

template(name="iotdbSample" type="string" option.sql="on"
    string="\"2026-11-19 10:00:00.000\":\"device-01\":23.4*:*:INSERT INTO telemetry(time, device_id, value) VALUES ('*1*', '*2*', *3*)")

action(type="omiotdb"
    server="http://127.0.0.1:18080"
    db="sensors"
    table="telemetry"
    columns="time TIMESTAMP TIME,device_id STRING TAG,value DOUBLE FIELD"
    uid="<service-user>"
    pwd="<service-password>"
    timeout="5000"
    template="iotdbSample")
```

Enable the IoTDB REST service (`enable_rest_service=true`) on the target DataNode. The implementation currently submits one HTTP request per message; it does not batch writes.

The REST formatter and response handling can be tested without an IoTDB server using `make -C plugins/omiotdb check` or by compiling `omiotdb_rest.c` with `tests/test_omiotdb_rest.c`.
