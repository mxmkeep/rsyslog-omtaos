pkglib_LTLIBRARIES = omiotdb.la

omiotdb_la_SOURCES = omiotdb.c omiotdb_rest.c omiotdb_rest.h
omiotdb_la_CPPFLAGS = $(RSRT_CFLAGS) $(PTHREADS_CFLAGS)
omiotdb_la_LDFLAGS = -module -avoid-version
omiotdb_la_LIBADD = -lcurl

check_PROGRAMS = test_omiotdb_rest
TESTS = $(check_PROGRAMS)
test_omiotdb_rest_SOURCES = tests/test_omiotdb_rest.c omiotdb_rest.c omiotdb_rest.h
