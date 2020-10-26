# rsyslog-omtaos
rsyslog plugin for tdengine
编写本插件的目的是将nginx产生的巨量http请求日志写入时序数据库TDengine
当然，本插件也支持将所有传给rsyslog的消息写入TDengine

# build
1. 下载rsyslog源码，本人采用的是8.24
https://github.com/rsyslog/rsyslog
http://www.rsyslog.com
2. 将本项目omtaos目录下载放到rsyslog源码的plugins目录下
3. 修改源码最外层的Makefile.am文件，增加omtaos配置
可以搜索参照mysql位置写入
```
if ENABLE_TAOS
SUBDIRS += plugins/omtaos
endif

THIS_IS_TEMPORARILY_DISABLED = \
    --enable-distcheck-workaround \
    --enable-testbench \
    --enable-imdiag \
    --enable-testbench \
    --enable-imfile \
    --enable-snmp \
    --enable-libdbi \
    --enable-mysql \
    --enable-taos \
    --enable-relp \
```
4. 修改源码的configure.ac文件，同样可以参照mysql位置填写
```
# TDengine(taos) support
AC_ARG_ENABLE(taos,
        [AS_HELP_STRING([--enable-taos],[Enable TDengine(taos) database support @<:@default=no@:>@])],
        [case "${enableval}" in
         yes) enable_taos="yes" ;;
          no) enable_taos="no" ;;
           *) AC_MSG_ERROR(bad value ${enableval} for --enable-taos) ;;
         esac],
        [enable_taos=no]
)
AM_CONDITIONAL(ENABLE_TAOS, test x$enable_taos = xyes)


AC_CONFIG_FILES([Makefile \
        runtime/Makefile \
        compat/Makefile \
        grammar/Makefile \
        tools/Makefile \
        plugins/omtesting/Makefile \
        plugins/omgssapi/Makefile \
        plugins/ommysql/Makefile \
        plugins/omtaos/Makefile \

echo "---{ database support }---"
echo "    MySql support enabled:                    $enable_mysql"
echo "    TDengine support enabled:                 $enable_taos"
echo "    libdbi support enabled:                   $enable_libdbi"
echo "    PostgreSQL support enabled:               $enable_pgsql"
echo "    mongodb support enabled:                  $enable_ommongodb"
echo "    hiredis support enabled:                  $enable_omhiredis"
```

5. 安装编译环境
yum install git valgrind autoconf automake flex bison python-docutils python-sphinx json-c-devel libuuid-devel libgcrypt-devel zlib-devel openssl-devel libcurl-devel gnutls-devel mysql-devel postgresql-devel libdbi-dbd-mysql libdbi-devel net-snmp-devel libestr-devel.x86_64 autoconf automake libtool -y
再安装   pkg-config-0.29.tar.gz

6. 重新生成configure文件
使用autogen.sh重新生成configure文件

7. configure
./configure --prefix=/usr/local/rsyslog-824 --enable-imptcp --enable-omuxsock --enable-omstdout --enable-taos
make install -j 4
