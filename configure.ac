AC_INIT([libkuhli], [0.1], [mattfrey@cisco.com], [])
AM_INIT_AUTOMAKE([foreign])
LT_INIT
AC_CONFIG_MACRO_DIR([m4])
AC_PROG_CC
AC_SEARCH_LIBS([curl_global_init, curl_easy_getinfo], [curl], [], [AC_MSG_ERROR([Unable to find libcurl])])
AC_CHECK_HEADER(curl/curl.h, , [AC_MSG_ERROR([Unable to find curl.h; Install libcurl dev package])])
AC_SEARCH_LIBS([uv_loop_init], [uv], [], [AC_MSG_ERROR([unable to find libuv])])
AC_CHECK_HEADER(uv.h, , [AC_MSG_ERROR([Unable to find uv.h; Install libuv dev package])])
CFLAGS="$CFLAGS -std=gnu99 -Wall"
AC_OUTPUT([Makefile])
