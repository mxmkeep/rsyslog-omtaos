pkglib_LTLIBRARIES = omtaos.la

# under RHEL 7, we have an issue when gnutls is linked. For some reason
# (autoconf magic?) we link ommysql against gnutls. I have not found the
# root cause, but setting LIBS= below makes the problem go away. A better
# solution would be appreciated, but for the time being this seems to
# solve the issue.
# more info: https://github.com/rsyslog/rsyslog/issues/408
# rgerhards, 2015-07-08
LIBS=
omtaos_la_SOURCES = omtaos.c ../omelasticsearch/cJSON/cjson.c
omtaos_la_CPPFLAGS =  $(RSRT_CFLAGS) $(PTHREADS_CFLAGS)
omtaos_la_LDFLAGS = -module -avoid-version
omtaos_la_LIBADD = -ltaos

EXTRA_DIST = 
