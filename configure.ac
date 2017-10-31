AC_INIT([libkuhli], [0.1], [mattfrey@cisco.com], [])
AM_INIT_AUTOMAKE([foreign])
LT_INIT
AC_CONFIG_MACRO_DIR([m4])
AC_PROG_CC
LIBCURL_CHECK_CONFIG([yes], [7.18.0])
LIBCURL_CHECK_CONFIG([yes], [7.32.0], [AC_DEFINE(USE_CURL_MULTISOCKET_ALL,1,[Define as 1 if curl must call curl_multisocket_all to unpause])])
AC_SEARCH_LIBS([uv_loop_init], [uv], [], [AC_MSG_ERROR([unable to find libuv])])
AC_CHECK_HEADER(uv.h, , [AC_MSG_ERROR([Unable to find uv.h; Install libuv dev package])])
CFLAGS="$CFLAGS -std=gnu99 -Wall"
AC_OUTPUT([Makefile])
