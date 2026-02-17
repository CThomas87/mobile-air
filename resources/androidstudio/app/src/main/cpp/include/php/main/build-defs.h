/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Stig Sæther Bakken <ssb@php.net>                             |
   +----------------------------------------------------------------------+
*/

#define CONFIGURE_COMMAND " '/home/chris/build/php-android/php-src/configure'  '--host=aarch64-linux-android' '--build=x86_64-pc-linux-gnu' '--prefix=/home/chris/build/php-android/build-aarch64-linux-android/out' '--enable-zts' '--enable-embed=shared' '--enable-opcache=shared' '--disable-cli' '--disable-cgi' '--disable-phpdbg' '--without-pear' '--with-layout=GNU' '--with-iconv=/home/chris/php-deps/build-arm64-v8a' '--with-sqlite3=/home/chris/php-deps/build-arm64-v8a' '--with-libxml' '--with-sodium=/home/chris/php-deps/build-arm64-v8a' '--with-curl=/home/chris/php-deps/build-arm64-v8a' '--with-openssl=/home/chris/php-deps/build-arm64-v8a' '--with-zlib' '--with-zip' '--enable-mbstring' 'build_alias=x86_64-pc-linux-gnu' 'host_alias=aarch64-linux-android' 'PKG_CONFIG_PATH=/home/chris/php-deps/build-arm64-v8a/lib/pkgconfig'"
#define PHP_ODBC_CFLAGS	""
#define PHP_ODBC_LFLAGS		""
#define PHP_ODBC_LIBS		""
#define PHP_ODBC_TYPE		""
#define PHP_PROG_SENDMAIL	"/usr/sbin/sendmail"
#define PEAR_INSTALLDIR         ""
#define PHP_INCLUDE_PATH	".:"
#define PHP_EXTENSION_DIR       "/home/chris/build/php-android/build-aarch64-linux-android/out/lib/php/20240924-zts"
#define PHP_PREFIX              "/home/chris/build/php-android/build-aarch64-linux-android/out"
#define PHP_BINDIR              "/home/chris/build/php-android/build-aarch64-linux-android/out/bin"
#define PHP_SBINDIR             "/home/chris/build/php-android/build-aarch64-linux-android/out/sbin"
#define PHP_MANDIR              "/home/chris/build/php-android/build-aarch64-linux-android/out/share/man"
#define PHP_LIBDIR              "/home/chris/build/php-android/build-aarch64-linux-android/out/lib/php"
#define PHP_DATADIR             "/home/chris/build/php-android/build-aarch64-linux-android/out/share/php"
#define PHP_SYSCONFDIR          "/home/chris/build/php-android/build-aarch64-linux-android/out/etc"
#define PHP_LOCALSTATEDIR       "/home/chris/build/php-android/build-aarch64-linux-android/out/var"
#define PHP_CONFIG_FILE_PATH    "/home/chris/build/php-android/build-aarch64-linux-android/out/etc"
#define PHP_CONFIG_FILE_SCAN_DIR    ""
#define PHP_SHLIB_SUFFIX        "so"
#define PHP_SHLIB_EXT_PREFIX    ""
