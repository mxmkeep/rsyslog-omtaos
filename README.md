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
```
yum install git valgrind autoconf automake flex bison python-docutils python-sphinx json-c-devel libuuid-devel libgcrypt-devel zlib-devel openssl-devel libcurl-devel gnutls-devel mysql-devel postgresql-devel libdbi-dbd-mysql libdbi-devel net-snmp-devel libestr-devel.x86_64 autoconf automake libtool -y
```
再安装   pkg-config-0.29.tar.gz

6. 重新生成configure文件
使用autogen.sh重新生成configure文件

7. configure
```
./configure --prefix=/usr/local/rsyslog-824 --enable-imptcp --enable-omuxsock --enable-omstdout --enable-taos
make install -j 4
```

# rsyslog configure
1. 在消息模板里，消息数据和SQL的分隔符为 *:*: 4个字符 (随意规定的，不喜欢可以自己修改源码)
2. 每一个消息值都需要用"" 双引号括起来，类似json格式，然后采用两个星号获取消息值，例如```*2*```获取第二个消息值
3. 需要传入http的time_local字段，然后代码自动在时间后面添加6位数字，将时间扩充为微妙级别，防止时间戳重复数据被TDengine丢弃
   关键字为time_local_to_ts_n，最后n代码time_local所在消息值位置，例如```*time_local_to_ts_10*```表示第10个消息值为time_local
4. 其他syslog配置请查看rsyslog文档https://www.rsyslog.com/doc/v8-stable/index.html
   TDengine相关请查看TDengine文档https://www.taosdata.com/cn/documentation/
```
$ActionFileDefaultTemplate RSYSLOG_TraditionalFileFormat
module(load="imudp")
input(type="imudp" port="1524" ruleset="TDengine")
#$ModLoad omtaos
module(load="omtaos") 
# msg *:*: sql
# msg "aaaa":"bbbbb","cccc""dddd"
# *1* = msg[1]=aaaa      *3* = msg[3]="cccc"
template(name="tpl_http" type="string" option.sql="on" string="\"mc\":\"%hostname:1:2%\",\"proxy\":\"%hostname:4:10%\",%msg%*:*:insert into http_test_'*2*'_'*4*' using http tags('*2*','*4*') values('*time_local_to_ts_10*','*6*','*8*')")
ruleset(name="TDengine"){
    if ( $programname == 'nginx' ) then {
        action(type="omtaos" server="yz-td01" serverport="6030"
            db="http_log" uid="root" pwd="taosdata" template="tpl_http"
            queue.filename="taos_q_http"
            queue.spoolDirectory="/data/taos/syslog"
            queue.size="5000000"
            queue.type="linkedList"
            queue.maxdiskspace="20g"
            queue.maxfilesize="100m"
            queue.workerthreads="100"
        )
    }
}
```
# performance
在4核8线程+8G环境下压测，入库速率为4万QPS左右
目前在生产环境1万QPS入库速率下基本正常
本代码为单条消息insert入库，还想再提升性能，可以修改为批量入库
