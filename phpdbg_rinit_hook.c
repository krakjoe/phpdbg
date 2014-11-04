/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2014 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Bob Weinand <bwoebi@php.net>                                |
   +----------------------------------------------------------------------+
*/

#include "phpdbg_rinit_hook.h"
#include "php_ini.h"
#include "ext/standard/file.h"

ZEND_DECLARE_MODULE_GLOBALS(phpdbg_webhelper);

PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("phpdbg.auth", "", PHP_INI_SYSTEM | PHP_INI_PERDIR, OnUpdateString, auth, zend_phpdbg_webhelper_globals, phpdbg_webhelper_globals)
	STD_PHP_INI_ENTRY("phpdbg.path", "", PHP_INI_SYSTEM | PHP_INI_PERDIR, OnUpdateString, path, zend_phpdbg_webhelper_globals, phpdbg_webhelper_globals)
PHP_INI_END()

static inline void php_phpdbg_webhelper_globals_ctor(zend_phpdbg_webhelper_globals *pg) /* {{{ */
{
} /* }}} */

static PHP_MINIT_FUNCTION(phpdbg_webhelper) /* {{{ */
{
	if (!strcmp(sapi_module.name, PHPDBG_NAME)) {
		return SUCCESS;
	}

	ZEND_INIT_MODULE_GLOBALS(phpdbg_webhelper, php_phpdbg_webhelper_globals_ctor, NULL);
	REGISTER_INI_ENTRIES();

	return SUCCESS;
} /* }}} */

static PHP_RINIT_FUNCTION(phpdbg_webhelper) /* {{{ */
{
	zval *cookies = PG(http_globals)[TRACK_VARS_COOKIE];
	zval **auth;

	if (!cookies || zend_hash_find(Z_ARRVAL_P(cookies), PHPDBG_NAME "_AUTH_COOKIE", sizeof(PHPDBG_NAME "_AUTH_COOKIE"), (void **) &auth) == FAILURE || Z_STRLEN_PP(auth) != strlen(PHPDBG_WG(auth)) || strcmp(Z_STRVAL_PP(auth), PHPDBG_WG(auth))) {
		return SUCCESS;
	}

	{
		char *host = PHPDBG_WG(path);
		int host_len = strlen(host);
		struct timeval tv;
		php_stream *stream;
		int err;
		char *errstr = NULL;

		char *msg = NULL;
		char msglen[5] = {0};

		tv.tv_sec = 60;
		tv.tv_usec = 0;

		stream = php_stream_xport_create(host, host_len, REPORT_ERRORS, STREAM_XPORT_CLIENT | STREAM_XPORT_CONNECT, NULL, &tv, NULL, &errstr, &err);

		if (stream == NULL) {
/*			zend_error(E_ERROR, "Unable to connect to UNIX domain socket at %s defined by phpdbg.path ini setting. Reason: %s", PHPDBG_WG(path), strerror(errno)); */
			return SUCCESS;
		}

		phpdbg_webdata_compress(&msg, (int *) msglen TSRMLS_CC);

		php_stream_write(stream, msglen, 4);
		php_stream_write(stream, msg, *(int *) msglen);

		php_stream_passthru(stream);
		php_stream_close(stream);

		php_output_flush_all(TSRMLS_C);
		zend_bailout();
	}

	return SUCCESS;
} /* }}} */

zend_module_entry phpdbg_webhelper_module_entry = {
	STANDARD_MODULE_HEADER,
	"phpdbg_webhelper",
	NULL,
	PHP_MINIT(phpdbg_webhelper),
	NULL,
	PHP_RINIT(phpdbg_webhelper),
	NULL,
	NULL,
	PHPDBG_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PHPDBG_WEBHELPER
ZEND_GET_MODULE(phpdbg_webhelper)
#endif
