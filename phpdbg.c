/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2013 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Felipe Pena <felipe@php.net>                                |
   | Authors: Joe Watkins <joe.watkins@live.co.uk>                        |
   +----------------------------------------------------------------------+
*/

#ifndef ZEND_SIGNALS
# include <signal.h>
#endif
#include "phpdbg.h"
#include "phpdbg_prompt.h"
#include "phpdbg_bp.h"
#include "phpdbg_break.h"
#include "phpdbg_utils.h"

ZEND_DECLARE_MODULE_GLOBALS(phpdbg);

#if PHP_VERSION_ID >= 50500
void (*zend_execute_old)(zend_execute_data *execute_data TSRMLS_DC);
#else
void (*zend_execute_old)(zend_op_array *op_array TSRMLS_DC);
#endif

static inline void php_phpdbg_globals_ctor(zend_phpdbg_globals *pg) /* {{{ */
{
    pg->exec = NULL;
    pg->exec_len = 0;
    pg->ops = NULL;
    pg->vmret = 0;
    pg->bp_count = 0;
    pg->lcmd = NULL;
    pg->flags = PHPDBG_DEFAULT_FLAGS;
    pg->oplog = NULL;
    pg->io[PHPDBG_STDIN] = NULL;
    pg->io[PHPDBG_STDOUT] = NULL;
    pg->io[PHPDBG_STDERR] = NULL;
    memset(&pg->lparam, 0, sizeof(phpdbg_param_t));
    pg->frame.num = 0;
} /* }}} */

static PHP_MINIT_FUNCTION(phpdbg) /* {{{ */
{
    ZEND_INIT_MODULE_GLOBALS(phpdbg, php_phpdbg_globals_ctor, NULL);
#if PHP_VERSION_ID >= 50500
    zend_execute_old = zend_execute_ex;
    zend_execute_ex = phpdbg_execute_ex;
#else
    zend_execute_old = zend_execute;
    zend_execute = phpdbg_execute_ex;
#endif

    REGISTER_LONG_CONSTANT("PHPDBG_FILE",    FILE_PARAM, CONST_CS|CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("PHPDBG_METHOD",  METHOD_PARAM, CONST_CS|CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("PHPDBG_LINENO", NUMERIC_PARAM, CONST_CS|CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("PHPDBG_FUNC",    STR_PARAM, CONST_CS|CONST_PERSISTENT);

    return SUCCESS;
} /* }}} */

static void php_phpdbg_destroy_bp_file(void *brake) /* {{{ */
{
	zend_llist_destroy((zend_llist*)brake);
} /* }}} */

static void php_phpdbg_destroy_bp_symbol(void *brake) /* {{{ */
{
	efree((char*)((phpdbg_breaksymbol_t*)brake)->symbol);
} /* }}} */

static void php_phpdbg_destroy_bp_methods(void *brake) /* {{{ */
{
    zend_hash_destroy((HashTable*)brake);
} /* }}} */

static void php_phpdbg_destroy_bp_condition(void *data) /* {{{ */
{
    phpdbg_breakcond_t *brake = (phpdbg_breakcond_t*) data;

    if (brake) {
        if (brake->ops) {
            TSRMLS_FETCH();

            destroy_op_array(
                brake->ops TSRMLS_CC);
            efree(brake->ops);
        }
        zval_dtor(&brake->code);
    }
} /* }}} */

static void php_phpdbg_destroy_registered(void *data)
{
	TSRMLS_FETCH();

	zend_function *function = (zend_function*) data;

	destroy_zend_function(
		function TSRMLS_CC);
}

static PHP_RINIT_FUNCTION(phpdbg) /* {{{ */
{
	zend_hash_init(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE],   8, NULL, php_phpdbg_destroy_bp_file, 0);
	zend_hash_init(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM], 8, NULL, php_phpdbg_destroy_bp_symbol, 0);
    zend_hash_init(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE], 8, NULL, NULL, 0);
    zend_hash_init(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD], 8, NULL, php_phpdbg_destroy_bp_methods, 0);
    zend_hash_init(&PHPDBG_G(bp)[PHPDBG_BREAK_COND], 8, NULL, php_phpdbg_destroy_bp_condition, 0);
	zend_hash_init(&PHPDBG_G(seek), 8, NULL, NULL, 0);
	zend_hash_init(&PHPDBG_G(registered), 8, NULL, php_phpdbg_destroy_registered, 0);

	return SUCCESS;
} /* }}} */

static PHP_RSHUTDOWN_FUNCTION(phpdbg) /* {{{ */
{
    zend_hash_destroy(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE]);
    zend_hash_destroy(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM]);
    zend_hash_destroy(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE]);
    zend_hash_destroy(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD]);
    zend_hash_destroy(&PHPDBG_G(bp)[PHPDBG_BREAK_COND]);
	zend_hash_destroy(&PHPDBG_G(seek));
	zend_hash_destroy(&PHPDBG_G(registered));

    if (PHPDBG_G(exec)) {
        efree(PHPDBG_G(exec));
        PHPDBG_G(exec) = NULL;
    }

    if (PHPDBG_G(oplog)) {
        fclose(
            PHPDBG_G(oplog));
        PHPDBG_G(oplog) = NULL;
    }

    if (PHPDBG_G(ops)) {
        destroy_op_array(PHPDBG_G(ops) TSRMLS_CC);
        efree(PHPDBG_G(ops));
        PHPDBG_G(ops) = NULL;
    }

    return SUCCESS;
} /* }}} */

/* {{{ proto void phpdbg_break([integer type, string expression])
    instructs phpdbg to insert a breakpoint at the next opcode */
static PHP_FUNCTION(phpdbg_break)
{
    if (ZEND_NUM_ARGS() > 0) {
        long type;
        char *expr = NULL;
        zend_uint expr_len = 0;
        phpdbg_param_t param;

        if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ls", &type, &expr, &expr_len) == FAILURE) {
            return;
        }

        phpdbg_parse_param(expr, expr_len, &param TSRMLS_CC);

        switch (type) {
            case METHOD_PARAM:
                phpdbg_do_break_method(&param, NULL TSRMLS_CC);
            break;

            case FILE_PARAM:
                phpdbg_do_break_file(&param, NULL TSRMLS_CC);
            break;

            case NUMERIC_PARAM:
                phpdbg_do_break_lineno(&param, NULL TSRMLS_CC);
            break;

            case STR_PARAM:
                phpdbg_do_break_func(&param, NULL TSRMLS_CC);
            break;

            default: zend_error(
                E_WARNING, "unrecognized parameter type %ld", type);
        }

        phpdbg_clear_param(&param TSRMLS_CC);

    } else if (EG(current_execute_data) && EG(active_op_array)) {
        zend_ulong opline_num = (EG(current_execute_data)->opline -
			EG(active_op_array)->opcodes);

        phpdbg_set_breakpoint_opline_ex(
            &EG(active_op_array)->opcodes[opline_num+1] TSRMLS_CC);
    }
} /* }}} */

/* {{{ proto void phpdbg_clear(void)
    instructs phpdbg to clear breakpoints */
static PHP_FUNCTION(phpdbg_clear)
{
    zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE]);
    zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM]);
    zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE]);
    zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD]);
    zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_COND]);
} /* }}} */

ZEND_BEGIN_ARG_INFO_EX(phpdbg_break_arginfo, 0, 0, 0)
    ZEND_ARG_INFO(0, type)
    ZEND_ARG_INFO(0, expression)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(phpdbg_clear_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

zend_function_entry phpdbg_user_functions[] = {
    PHP_FE(phpdbg_clear, phpdbg_clear_arginfo)
    PHP_FE(phpdbg_break, phpdbg_break_arginfo)
#ifdef  PHP_FE_END
	PHP_FE_END
#else
	{NULL,NULL,NULL}
#endif
};

static zend_module_entry sapi_phpdbg_module_entry = {
	STANDARD_MODULE_HEADER,
	"phpdbg",
	phpdbg_user_functions,
	PHP_MINIT(phpdbg),
	NULL,
	PHP_RINIT(phpdbg),
	PHP_RSHUTDOWN(phpdbg),
	NULL,
	"0.1",
	STANDARD_MODULE_PROPERTIES
};

static inline int php_sapi_phpdbg_module_startup(sapi_module_struct *module) /* {{{ */
{
    if (php_module_startup(module, &sapi_phpdbg_module_entry, 1) == FAILURE) {
		return FAILURE;
	}
	return SUCCESS;
} /* }}} */

static char* php_sapi_phpdbg_read_cookies(TSRMLS_D) /* {{{ */
{
    return NULL;
} /* }}} */

static int php_sapi_phpdbg_header_handler(sapi_header_struct *h, sapi_header_op_enum op, sapi_headers_struct *s TSRMLS_DC) /* {{{ */
{
	return 0;
}
/* }}} */

static int php_sapi_phpdbg_send_headers(sapi_headers_struct *sapi_headers TSRMLS_DC) /* {{{ */
{
	/* We do nothing here, this function is needed to prevent that the fallback
	 * header handling is called. */
	return SAPI_HEADER_SENT_SUCCESSFULLY;
}
/* }}} */

static void php_sapi_phpdbg_send_header(sapi_header_struct *sapi_header, void *server_context TSRMLS_DC) /* {{{ */
{
}
/* }}} */

static void php_sapi_phpdbg_log_message(char *message TSRMLS_DC) /* {{{ */
{
	phpdbg_error(message);
}
/* }}} */

static int php_sapi_phpdbg_deactivate(TSRMLS_D) /* {{{ */
{
	fflush(stdout);
	if(SG(request_info).argv0) {
		free(SG(request_info).argv0);
		SG(request_info).argv0 = NULL;
	}
	return SUCCESS;
}
/* }}} */

static void php_sapi_phpdbg_register_vars(zval *track_vars_array TSRMLS_DC) /* {{{ */
{
	unsigned int len;
	char   *docroot = "";

	/* In phpdbg mode, we consider the environment to be a part of the server variables
     */
    php_import_environment_variables(track_vars_array TSRMLS_CC);

    if (PHPDBG_G(exec)) {
        len = PHPDBG_G(exec_len);
        if (sapi_module.input_filter(PARSE_SERVER, "PHP_SELF",
			&PHPDBG_G(exec), PHPDBG_G(exec_len), &len TSRMLS_CC)) {
	        php_register_variable("PHP_SELF", PHPDBG_G(exec),
				track_vars_array TSRMLS_CC);
        }
        if (sapi_module.input_filter(PARSE_SERVER, "SCRIPT_NAME",
			&PHPDBG_G(exec), PHPDBG_G(exec_len), &len TSRMLS_CC)) {
	        php_register_variable("SCRIPT_NAME", PHPDBG_G(exec),
				track_vars_array TSRMLS_CC);
        }

        if (sapi_module.input_filter(PARSE_SERVER, "SCRIPT_FILENAME",
			&PHPDBG_G(exec), PHPDBG_G(exec_len), &len TSRMLS_CC)) {
	        php_register_variable("SCRIPT_FILENAME", PHPDBG_G(exec),
				track_vars_array TSRMLS_CC);
        }
        if (sapi_module.input_filter(PARSE_SERVER, "PATH_TRANSLATED",
			&PHPDBG_G(exec), PHPDBG_G(exec_len), &len TSRMLS_CC)) {
	        php_register_variable("PATH_TRANSLATED", PHPDBG_G(exec),
				track_vars_array TSRMLS_CC);
        }
    }

    /* any old docroot will doo */
    len = 0U;
    if (sapi_module.input_filter(PARSE_SERVER, "DOCUMENT_ROOT",
		&docroot, len, &len TSRMLS_CC)) {
	    php_register_variable("DOCUMENT_ROOT", docroot, track_vars_array TSRMLS_CC);
    }
}
/* }}} */

static inline int php_sapi_phpdbg_ub_write(const char *message, unsigned int length TSRMLS_DC) /* {{{ */
{
	return phpdbg_write(message);
} /* }}} */

static inline void php_sapi_phpdbg_flush(void *context)  /* {{{ */
{
	TSRMLS_FETCH();
	
	fflush(PHPDBG_G(io)[PHPDBG_STDOUT]);
} /* }}} */

/* {{{ sapi_module_struct phpdbg_sapi_module
 */
static sapi_module_struct phpdbg_sapi_module = {
	"phpdbg",						/* name */
	"phpdbg",					    /* pretty name */

	php_sapi_phpdbg_module_startup,	/* startup */
	php_module_shutdown_wrapper,    /* shutdown */

	NULL,		                    /* activate */
	php_sapi_phpdbg_deactivate,		/* deactivate */

	php_sapi_phpdbg_ub_write,		/* unbuffered write */
	php_sapi_phpdbg_flush,			/* flush */
	NULL,							/* get uid */
	NULL,				            /* getenv */

	php_error,						/* error handler */

	php_sapi_phpdbg_header_handler,	/* header handler */
	php_sapi_phpdbg_send_headers,	/* send headers handler */
	php_sapi_phpdbg_send_header,	/* send header handler */

	NULL,				            /* read POST data */
	php_sapi_phpdbg_read_cookies,   /* read Cookies */

	php_sapi_phpdbg_register_vars,	/* register server variables */
	php_sapi_phpdbg_log_message,	/* Log message */
	NULL,							/* Get request time */
	NULL,							/* Child terminate */
	STANDARD_SAPI_MODULE_PROPERTIES
};
/* }}} */

const opt_struct OPTIONS[] = { /* {{{ */
	{'c', 1, "ini path override"},
	{'d', 1, "define ini entry on command line"},
	{'n', 0, "no php.ini"},
	{'z', 1, "load zend_extension"},
	/* phpdbg options */
	{'q', 0, "no banner"},
	{'e', 1, "exec"},
	{'v', 0, "disable quietness"},
	{'s', 0, "enable stepping"},
	{'b', 0, "boring colours"},
	{'i', 1, "specify init"},
	{'I', 0, "ignore init"},
	{'O', 1, "opline log"},
	{'r', 0, "run"},
	{'E', 0, "step-through-eval"},
	{'-', 0, NULL}
}; /* }}} */

const char phpdbg_ini_hardcoded[] =
	"html_errors=Off\n"
	"register_argc_argv=On\n"
	"implicit_flush=On\n"
	"display_errors=Off\n"
	"max_execution_time=0\n"
	"max_input_time=-1\n\0";

/* overwriteable ini defaults must be set in phpdbg_ini_defaults() */
#define INI_DEFAULT(name,value)\
	Z_SET_REFCOUNT(tmp, 0);\
	Z_UNSET_ISREF(tmp);	\
	ZVAL_STRINGL(&tmp, zend_strndup(value, sizeof(value)-1), sizeof(value)-1, 0);\
	zend_hash_update(configuration_hash, name, sizeof(name), &tmp, sizeof(zval), NULL);\

void phpdbg_ini_defaults(HashTable *configuration_hash) /* {{{ */
{
    zval tmp;
	INI_DEFAULT("report_zend_debug", "0");
} /* }}} */

static void phpdbg_welcome(zend_bool cleaning TSRMLS_DC) /* {{{ */
{
    /* print blurb */
	if (!cleaning) {
		phpdbg_notice("Welcome to phpdbg, the interactive PHP debugger, v%s",
			PHPDBG_VERSION);
		phpdbg_writeln("To get help using phpdbg type \"help\" and press enter");
		phpdbg_notice("Please report bugs to <%s>", PHPDBG_ISSUES);
	} else {
		phpdbg_notice("Clean Execution Environment");

		phpdbg_writeln("Classes\t\t\t%d", zend_hash_num_elements(EG(class_table)));
		phpdbg_writeln("Functions\t\t%d", zend_hash_num_elements(EG(function_table)));
		phpdbg_writeln("Constants\t\t%d", zend_hash_num_elements(EG(zend_constants)));
		phpdbg_writeln("Includes\t\t%d",  zend_hash_num_elements(&EG(included_files)));
	}
} /* }}} */

static inline void phpdbg_sigint_handler(int signo) /* {{{ */
{
	TSRMLS_FETCH();
	
	if (EG(in_execution)) {
		/* we don't want to set signalled while phpdbg is interactive */
		if (!(PHPDBG_G(flags) & PHPDBG_IS_INTERACTIVE)) {
			PHPDBG_G(flags) |= PHPDBG_IS_SIGNALED;
		}
	} else {
		/* if we are not executing then just provide advice */
		phpdbg_writeln(EMPTY);
		phpdbg_error(
			"Please leave phpdbg gracefully !");
	}
} /* }}} */

int main(int argc, char **argv) /* {{{ */
{
	sapi_module_struct *phpdbg = &phpdbg_sapi_module;
	char *ini_entries;
	int   ini_entries_len;
	char *exec;
	size_t exec_len;
	char *init_file;
	size_t init_file_len;
	zend_bool init_file_default;
	char *oplog_file;
	size_t oplog_file_len;
	zend_ulong flags;
	char *php_optarg;
	int php_optind, opt, show_banner = 1;
	long cleaning = 0;
	int run = 0;
	int step = 0;

#ifdef ZTS
	void ***tsrm_ls;
#endif

#ifdef PHP_WIN32
	_fmode = _O_BINARY;                 /* sets default for file streams to binary */
	setmode(_fileno(stdin), O_BINARY);  /* make the stdio mode be binary */
	setmode(_fileno(stdout), O_BINARY); /* make the stdio mode be binary */
	setmode(_fileno(stderr), O_BINARY); /* make the stdio mode be binary */
#endif

#ifdef ZTS
	tsrm_startup(1, 1, 0, NULL);

	tsrm_ls = ts_resource(0);
#endif

phpdbg_main:
	ini_entries = NULL;
	ini_entries_len = 0;
	exec = NULL;
	exec_len = 0;
	init_file = NULL;
	init_file_len = 0;
	init_file_default = 1;
	oplog_file = NULL;
	oplog_file_len = 0;
	flags = PHPDBG_DEFAULT_FLAGS;
	php_optarg = NULL;
	php_optind = 1;
	opt = 0;
	run = 0;
	step = 0;

	while ((opt = php_getopt(argc, argv, OPTIONS, &php_optarg, &php_optind, 0, 2)) != -1) {
		switch (opt) {
			case 'r':
				run++;
			break;
			case 'n':
				phpdbg->php_ini_ignore = 1;
			break;
			case 'c':
				if (phpdbg->php_ini_path_override) {
					free(phpdbg->php_ini_path_override);
				}
				phpdbg->php_ini_path_override = strdup(php_optarg);
			break;
			case 'd': {
				int len = strlen(php_optarg);
				char *val;

				if ((val = strchr(php_optarg, '='))) {
					val++;
					if (!isalnum(*val) && *val != '"' && *val != '\'' && *val != '\0') {
						ini_entries = realloc(ini_entries, ini_entries_len + len + sizeof("\"\"\n\0"));
						memcpy(ini_entries + ini_entries_len, php_optarg, (val - php_optarg));
						ini_entries_len += (val - php_optarg);
						memcpy(ini_entries + ini_entries_len, "\"", 1);
						ini_entries_len++;
						memcpy(ini_entries + ini_entries_len, val, len - (val - php_optarg));
						ini_entries_len += len - (val - php_optarg);
						memcpy(ini_entries + ini_entries_len, "\"\n\0", sizeof("\"\n\0"));
						ini_entries_len += sizeof("\n\0\"") - 2;
					} else {
						ini_entries = realloc(ini_entries, ini_entries_len + len + sizeof("\n\0"));
						memcpy(ini_entries + ini_entries_len, php_optarg, len);
						memcpy(ini_entries + ini_entries_len + len, "\n\0", sizeof("\n\0"));
						ini_entries_len += len + sizeof("\n\0") - 2;
					}
				} else {
					ini_entries = realloc(ini_entries, ini_entries_len + len + sizeof("=1\n\0"));
					memcpy(ini_entries + ini_entries_len, php_optarg, len);
					memcpy(ini_entries + ini_entries_len + len, "=1\n\0", sizeof("=1\n\0"));
					ini_entries_len += len + sizeof("=1\n\0") - 2;
				}
			} break;
			case 'z':
				zend_load_extension(php_optarg);
			break;

			/* begin phpdbg options */

			case 'e': { /* set execution context */
				exec_len = strlen(php_optarg);
				if (exec_len) {
					exec = strdup(php_optarg);
				}
			} break;

			case 'I': { /* ignore .phpdbginit */
				init_file_default = 0;
			} break;

			case 'i': { /* set init file */
				init_file_len = strlen(php_optarg);
				if (init_file_len) {
					init_file = strdup(php_optarg);
				}
			} break;

			case 'O': { /* set oplog output */
				oplog_file_len = strlen(php_optarg);
				if (oplog_file_len) {
					oplog_file = strdup(php_optarg);
				}
			} break;

			case 'v': /* set quietness off */
				flags &= ~PHPDBG_IS_QUIET;
			break;

			case 's': /* set stepping on */
				step = 1;
			break;

			case 'E': /* stepping through eval on */
				flags |= PHPDBG_IS_STEPONEVAL;
			break;

			case 'b': /* set colours off */
				flags &= ~PHPDBG_IS_COLOURED;
			break;

			case 'q': /* hide banner */
				show_banner = 0;
			break;
		}
	}

	phpdbg->ini_defaults = phpdbg_ini_defaults;
	phpdbg->phpinfo_as_text = 1;
	phpdbg->php_ini_ignore_cwd = 1;

	sapi_startup(phpdbg);

	phpdbg->executable_location = argv[0];
	phpdbg->phpinfo_as_text = 1;
	phpdbg->php_ini_ignore = 0;

	if (ini_entries) {
		ini_entries = realloc(ini_entries, ini_entries_len + sizeof(phpdbg_ini_hardcoded));
		memmove(ini_entries + sizeof(phpdbg_ini_hardcoded) - 2, ini_entries, ini_entries_len + 1);
		memcpy(ini_entries, phpdbg_ini_hardcoded, sizeof(phpdbg_ini_hardcoded) - 2);
	} else {
		ini_entries = malloc(sizeof(phpdbg_ini_hardcoded));
		memcpy(ini_entries, phpdbg_ini_hardcoded, sizeof(phpdbg_ini_hardcoded));
	}
	ini_entries_len += sizeof(phpdbg_ini_hardcoded) - 2;

	phpdbg->ini_entries = ini_entries;

	if (phpdbg->startup(phpdbg) == SUCCESS) {
		zend_activate(TSRMLS_C);

#ifdef ZEND_SIGNALS
		zend_try {
			zend_signal_activate(TSRMLS_C);
			zend_signal(SIGINT, phpdbg_sigint_handler TSRMLS_CC);
		} zend_end_try();
#else
		signal(SIGINT, phpdbg_sigint_handler);
#endif

		PG(modules_activated) = 0;

		/* set up basic io here */
		PHPDBG_G(io)[PHPDBG_STDIN] = stdin;
		PHPDBG_G(io)[PHPDBG_STDOUT] = stdout;
		PHPDBG_G(io)[PHPDBG_STDERR] = stderr;

        if (exec) { /* set execution context */
            PHPDBG_G(exec) = phpdbg_resolve_path(
                exec TSRMLS_CC);
            PHPDBG_G(exec_len) = strlen(PHPDBG_G(exec));

            free(exec);
        }

		if (oplog_file) { /* open oplog */
			PHPDBG_G(oplog) = fopen(oplog_file, "w+");
			if (!PHPDBG_G(oplog)) {
				phpdbg_error(
				"Failed to open oplog %s", oplog_file);
			}
			free(oplog_file);
		}

        /* set flags from command line */
        PHPDBG_G(flags) = flags;

		zend_try {
			zend_activate_modules(TSRMLS_C);
		} zend_end_try();

		if (show_banner) {
			/* print blurb */
			phpdbg_welcome((cleaning > 0) TSRMLS_CC);
		}

		zend_try {
        	/* activate globals, they can be overwritten */
        	zend_activate_auto_globals(TSRMLS_C);
        } zend_end_try();

        /* initialize from file */
        zend_try {
        	PHPDBG_G(flags) |= PHPDBG_IS_INITIALIZING;
            phpdbg_init(
            	init_file, init_file_len,
            	init_file_default TSRMLS_CC);
            PHPDBG_G(flags) &= ~PHPDBG_IS_INITIALIZING;
        } zend_catch {
        	PHPDBG_G(flags) &= ~PHPDBG_IS_INITIALIZING;
            if (PHPDBG_G(flags) & PHPDBG_IS_QUITTING) {
                goto phpdbg_out;
            }
        } zend_end_try();

        /* step from here, not through init */
        if (step) {
        	PHPDBG_G(flags) |= PHPDBG_IS_STEPPING;
        }

        if (run) {
        	/* no need to try{}, run does it ... */
	    	PHPDBG_COMMAND_HANDLER(run)(NULL, NULL TSRMLS_CC);
	    	if (run > 1) {
	    		/* if -r is on the command line more than once just quit */
	    		goto phpdbg_out;
	    	}
        }

        /* phpdbg main() */
        do {
		    zend_try {
		        phpdbg_interactive(TSRMLS_C);
		    } zend_catch {
                if ((PHPDBG_G(flags) & PHPDBG_IS_CLEANING)) {
                    cleaning = 1;
                    goto phpdbg_out;
                } else cleaning = 0;

                if (PHPDBG_G(flags) & PHPDBG_IS_QUITTING) {
                    goto phpdbg_out;
                }
		    } zend_end_try();
		} while(!(PHPDBG_G(flags) & PHPDBG_IS_QUITTING));

phpdbg_out:
		if (ini_entries) {
		    free(ini_entries);
		}

		if (PG(modules_activated)) {
			zend_try {
				zend_deactivate_modules(TSRMLS_C);
			} zend_end_try();
		}

		zend_deactivate(TSRMLS_C);

		zend_try {
			zend_post_deactivate_modules(TSRMLS_C);
		} zend_end_try();

#ifdef ZEND_SIGNALS
		zend_try {
			zend_signal_deactivate(TSRMLS_C);
		} zend_end_try();
#endif

		zend_try {
		    php_module_shutdown(TSRMLS_C);
		} zend_end_try();

		sapi_shutdown();
	}

	if (cleaning) {
        goto phpdbg_main;
    }

#ifdef ZTS
	/* bugggy */
	/* tsrm_shutdown(); */
#endif

	return 0;
} /* }}} */
