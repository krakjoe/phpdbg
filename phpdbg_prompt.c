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

#include <stdio.h>
#include <string.h>
#include "zend.h"
#include "zend_compile.h"
#include "phpdbg.h"
#include "phpdbg_help.h"
#include "phpdbg_print.h"
#include "phpdbg_info.h"
#include "phpdbg_break.h"
#include "phpdbg_bp.h"
#include "phpdbg_opcode.h"
#include "phpdbg_list.h"
#include "phpdbg_utils.h"
#include "phpdbg_prompt.h"
#include "phpdbg_cmd.h"
#include "phpdbg_frame.h"

/* {{{ command declarations */
const phpdbg_command_t phpdbg_prompt_commands[] = {
	PHPDBG_COMMAND_D(exec,    "set execution context",                    'e', NULL, 1),
	PHPDBG_COMMAND_D(compile, "attempt compilation", 					  'c', NULL, 0),
	PHPDBG_COMMAND_D(step,    "step through execution",                   's', NULL, 1),
	PHPDBG_COMMAND_D(next,    "continue execution",                       'n', NULL, 0),
	PHPDBG_COMMAND_D(run,     "attempt execution",                        'r', NULL, 0),
	PHPDBG_COMMAND_D(eval,    "evaluate some code",                       'E', NULL, 1),
	PHPDBG_COMMAND_D(until,   "continue past the current line",           'u', NULL, 0),
	PHPDBG_COMMAND_D(finish,  "continue past the end of the stack",       'F', NULL, 0),
	PHPDBG_COMMAND_D(leave,   "continue until the end of the stack",      'L', NULL, 0),
	PHPDBG_COMMAND_D(frame,   "switch to a frame",                        'f', NULL, 1),
	PHPDBG_COMMAND_D(print,   "print something",                          'p', phpdbg_print_commands, 2),
	PHPDBG_COMMAND_D(break,   "set breakpoint",                           'b', phpdbg_break_commands, 1),
	PHPDBG_COMMAND_D(back,    "show trace",                               't', NULL, 0),
	PHPDBG_COMMAND_D(list,    "lists some code",                          'l', phpdbg_list_commands, 2),
	PHPDBG_COMMAND_D(info,    "displays some informations",               'i', phpdbg_info_commands, 1),
	PHPDBG_COMMAND_D(clean,   "clean the execution environment",          'X', NULL, 0),
	PHPDBG_COMMAND_D(clear,   "clear breakpoints",                        'C', NULL, 0),
	PHPDBG_COMMAND_D(help,    "show help menu",                           'h', phpdbg_help_commands, 2),
	PHPDBG_COMMAND_D(quiet,   "silence some output",                      'Q', NULL, 1),
	PHPDBG_COMMAND_D(aliases, "show alias list",                          'a', NULL, 0),
	PHPDBG_COMMAND_D(oplog,   "sets oplog output",                        'O', NULL, 1),
	PHPDBG_COMMAND_D(register,"register a function",                      'R', NULL, 1),
	PHPDBG_COMMAND_D(shell,   "shell a command",                          '-', NULL, 1),
	PHPDBG_COMMAND_D(quit,    "exit phpdbg",                              'q', NULL, 0),
	PHPDBG_END_COMMAND
}; /* }}} */

ZEND_EXTERN_MODULE_GLOBALS(phpdbg);

void phpdbg_init(char *init_file, size_t init_file_len, zend_bool use_default TSRMLS_DC) /* {{{ */
{
	zend_bool init_default = 0;

	if (!init_file && use_default) {
		struct stat sb;

		if (VCWD_STAT(".phpdbginit", &sb) != -1) {
			init_file = ".phpdbginit";
			init_file_len = strlen(".phpdbginit");
			init_default = 1;
		}
	}

	if (init_file) {
		FILE *fp = fopen(init_file, "r");
		if (fp) {
			int line = 1;

			char cmd[PHPDBG_MAX_CMD];
			size_t cmd_len = 0L;
			char *code = NULL;
			size_t code_len = 0L;
			zend_bool in_code = 0;

			while (fgets(cmd, PHPDBG_MAX_CMD, fp) != NULL) {
				cmd_len = strlen(cmd)-1;

				while (*cmd && isspace(cmd[cmd_len-1]))
					cmd_len--;

				cmd[cmd_len] = '\0';

				if (*cmd && cmd_len > 0L && cmd[0] != '#') {
					if (cmd_len == 2) {
						if (memcmp(cmd, "<:", sizeof("<:")-1) == SUCCESS) {
							in_code = 1;
							goto next_line;
						} else {
							if (memcmp(cmd, ":>", sizeof(":>")-1) == SUCCESS) {
								in_code = 0;
								code[code_len] = '\0';
								{
									zend_eval_stringl(
										code, code_len, NULL, "phpdbginit code" TSRMLS_CC);
								}
								free(code);
								code = NULL;
								goto next_line;
							}
						}
					}

					if (in_code) {
						if (code == NULL) {
							code = malloc(cmd_len + 1);
						} else code = realloc(code, code_len + cmd_len + 1);

						if (code) {
							memcpy(
								&code[code_len], cmd, cmd_len);
							code_len += cmd_len;
						}
						goto next_line;
					}

					{
						phpdbg_input_t *input = phpdbg_read_input(cmd TSRMLS_CC);
						switch (phpdbg_do_cmd(phpdbg_prompt_commands, input TSRMLS_CC)) {
							case FAILURE:
								phpdbg_error(
									"Unrecognized command in %s:%d: %s!", init_file, line, cmd);
							break;
						}
						phpdbg_destroy_input(&input TSRMLS_CC);
					}

				}
next_line:
				line++;
			}

			if (code) {
				free(code);
			}

			fclose(fp);
		} else {
			phpdbg_error(
				"Failed to open %s for initialization", init_file);
		}

		if (!init_default) {
			free(init_file);
		}
	}
} /* }}} */

PHPDBG_COMMAND(exec) /* {{{ */
{
	switch (param->type) {
		case STR_PARAM: {
		    struct stat sb;

		    if (VCWD_STAT(param->str, &sb) != FAILURE) {
		        if (sb.st_mode & (S_IFREG|S_IFLNK)) {
		        	if (PHPDBG_G(exec)) {
		            	phpdbg_notice("Unsetting old execution context: %s", PHPDBG_G(exec));
		            	efree(PHPDBG_G(exec));
		            	PHPDBG_G(exec) = NULL;
		            }

					if (PHPDBG_G(ops)) {
						phpdbg_notice("Destroying compiled opcodes");
						phpdbg_clean(0 TSRMLS_CC);
					}

					PHPDBG_G(exec) = phpdbg_resolve_path(param->str TSRMLS_CC);

					if (!PHPDBG_G(exec)) {
						phpdbg_error("Cannot get real file path");
						return SUCCESS;
					}

					PHPDBG_G(exec_len) = strlen(PHPDBG_G(exec));

					phpdbg_notice("Set execution context: %s", PHPDBG_G(exec));

				} else {
					phpdbg_error("Cannot use %s as execution context, not a valid file or symlink", param->str);
				}
			} else {
				phpdbg_error("Cannot stat %s, ensure the file exists", param->str);
			}
		} break;

		phpdbg_default_switch_case();
	}

	return SUCCESS;
} /* }}} */

int phpdbg_compile(TSRMLS_D) /* {{{ */
{
	zend_file_handle fh;

	if (EG(in_execution)) {
		phpdbg_error("Cannot compile while in execution");
		return FAILURE;
	}

	phpdbg_notice("Attempting compilation of %s", PHPDBG_G(exec));

	if (php_stream_open_for_zend_ex(PHPDBG_G(exec), &fh,
		USE_PATH|STREAM_OPEN_FOR_INCLUDE TSRMLS_CC) == SUCCESS) {

		PHPDBG_G(ops) = zend_compile_file(&fh, ZEND_INCLUDE TSRMLS_CC);
		zend_destroy_file_handle(&fh TSRMLS_CC);

		phpdbg_notice("Success");
		return SUCCESS;
	} else {
		phpdbg_error("Could not open file %s", PHPDBG_G(exec));
	}

	return FAILURE;
} /* }}} */

PHPDBG_COMMAND(compile) /* {{{ */
{
	if (!PHPDBG_G(exec)) {
		phpdbg_error("No execution context");
		return SUCCESS;
	}

	if (!EG(in_execution)) {
		if (PHPDBG_G(ops)) {
			phpdbg_error("Destroying previously compiled opcodes");
			phpdbg_clean(0 TSRMLS_CC);
		}
	}

	phpdbg_compile(TSRMLS_C);

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(step) /* {{{ */
{
	switch (param->type) {
		case EMPTY_PARAM:
		case NUMERIC_PARAM: {
			if (param->type == NUMERIC_PARAM && param->num) {
				PHPDBG_G(flags) |= PHPDBG_IS_STEPPING;
			} else {
				PHPDBG_G(flags) &= ~PHPDBG_IS_STEPPING;
			}

			phpdbg_notice("Stepping %s",
				(PHPDBG_G(flags) & PHPDBG_IS_STEPPING) ? "on" : "off");
		} break;

		phpdbg_default_switch_case();
	}

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(next) /* {{{ */
{
	return PHPDBG_NEXT;
} /* }}} */

PHPDBG_COMMAND(until) /* {{{ */
{
	if (!EG(in_execution)) {
		phpdbg_error("Not executing");
		return SUCCESS;
	}

	PHPDBG_G(flags) |= PHPDBG_IN_UNTIL;
	{
		zend_uint next = 0,
				  self = (EG(current_execute_data)->opline - EG(active_op_array)->opcodes);
		zend_op  *opline = &EG(active_op_array)->opcodes[self];

		for (next = self; next < EG(active_op_array)->last; next++) {
			if (EG(active_op_array)->opcodes[next].lineno != opline->lineno) {
				zend_hash_index_update(
					&PHPDBG_G(seek),
					(zend_ulong) &EG(active_op_array)->opcodes[next],
					&EG(active_op_array)->opcodes[next],
					sizeof(zend_op), NULL);
				break;
			}
		}
	}

	return PHPDBG_UNTIL;
} /* }}} */

PHPDBG_COMMAND(finish) /* {{{ */
{
	if (!EG(in_execution)) {
		phpdbg_error("Not executing");
		return SUCCESS;
	}

	PHPDBG_G(flags) |= PHPDBG_IN_FINISH;
	{
		zend_uint next = 0,
				  self = (EG(current_execute_data)->opline - EG(active_op_array)->opcodes);

		for (next = self; next < EG(active_op_array)->last; next++) {
			switch (EG(active_op_array)->opcodes[next].opcode) {
				case ZEND_RETURN:
				case ZEND_THROW:
				case ZEND_EXIT:
#ifdef ZEND_YIELD
				case ZEND_YIELD:
#endif
					zend_hash_index_update(
						&PHPDBG_G(seek),
						(zend_ulong) &EG(active_op_array)->opcodes[next],
						&EG(active_op_array)->opcodes[next],
						sizeof(zend_op), NULL);
				break;
			}
		}
	}

	return PHPDBG_FINISH;
} /* }}} */

PHPDBG_COMMAND(leave) /* {{{ */
{
	if (!EG(in_execution)) {
		phpdbg_error("Not executing");
		return SUCCESS;
	}

	PHPDBG_G(flags) |= PHPDBG_IN_LEAVE;
	{
		zend_uint next = 0,
				  self = (EG(current_execute_data)->opline - EG(active_op_array)->opcodes);

		for (next = self; next < EG(active_op_array)->last; next++) {
			switch (EG(active_op_array)->opcodes[next].opcode) {
				case ZEND_RETURN:
				case ZEND_THROW:
				case ZEND_EXIT:
#ifdef ZEND_YIELD
				case ZEND_YIELD:
#endif
					zend_hash_index_update(
						&PHPDBG_G(seek),
						(zend_ulong) &EG(active_op_array)->opcodes[next],
						&EG(active_op_array)->opcodes[next],
						sizeof(zend_op), NULL);
				break;
			}
		}
	}

	return PHPDBG_LEAVE;
} /* }}} */

PHPDBG_COMMAND(frame) /* {{{ */
{
	switch (param->type) {
		case NUMERIC_PARAM:
			switch_to_frame(param->num TSRMLS_CC);
			break;

		case EMPTY_PARAM:
			phpdbg_writeln("Currently at frame %d:", PHPDBG_G(frame).num);
			break;

		phpdbg_default_switch_case();
	}

	return SUCCESS;
} /* }}} */

static inline void phpdbg_handle_exception(TSRMLS_D) /* }}} */
{
	zend_fcall_info fci;

	zval fname,
		 *trace,
		 exception;

	/* get filename and linenumber before unsetting exception */
	const char *filename = zend_get_executed_filename(TSRMLS_C);
	zend_uint lineno = zend_get_executed_lineno(TSRMLS_C);

	/* copy exception */
	exception = *EG(exception);
	zval_copy_ctor(&exception);
	EG(exception) = NULL;

	phpdbg_error(
		"Uncaught %s !",
		Z_OBJCE(exception)->name);

	/* call __toString */
	ZVAL_STRINGL(&fname, "__tostring", sizeof("__tostring")-1, 1);
	fci.size = sizeof(fci);
	fci.function_table = &Z_OBJCE(exception)->function_table;
	fci.function_name = &fname;
	fci.symbol_table = NULL;
	fci.object_ptr = &exception;
	fci.retval_ptr_ptr = &trace;
	fci.param_count = 0;
	fci.params = NULL;
	fci.no_separation = 1;
	zend_call_function(&fci, NULL TSRMLS_CC);

	if (trace) {
		phpdbg_writeln(
			"Uncaught %s", Z_STRVAL_P(trace));
		/* remember to dtor trace */
		zval_ptr_dtor(&trace);
	}

	/* output useful information about address */
	phpdbg_writeln(
		"Stacked entered at %p in %s on line %lu",
		EG(active_op_array)->opcodes, filename, lineno);

	zval_dtor(&fname);
	zval_dtor(&exception);
} /* }}} */

PHPDBG_COMMAND(run) /* {{{ */
{
	if (EG(in_execution)) {
		phpdbg_error("Cannot start another execution while one is in progress");
        return SUCCESS;
	}

	if (PHPDBG_G(ops) || PHPDBG_G(exec)) {
		zend_op **orig_opline = EG(opline_ptr);
		zend_op_array *orig_op_array = EG(active_op_array);
		zval **orig_retval_ptr = EG(return_value_ptr_ptr);

		if (!PHPDBG_G(ops)) {
			if (phpdbg_compile(TSRMLS_C) == FAILURE) {
				phpdbg_error(
				    "Failed to compile %s, cannot run", PHPDBG_G(exec));
				goto out;
			}
		}

		EG(active_op_array) = PHPDBG_G(ops);
		EG(return_value_ptr_ptr) = &PHPDBG_G(retval);
        if (!EG(active_symbol_table)) {
            zend_rebuild_symbol_table(TSRMLS_C);
        }

		/* clean seek state */
		PHPDBG_G(flags) &= ~PHPDBG_SEEK_MASK;
		zend_hash_clean(
			&PHPDBG_G(seek));

		zend_try {
			php_output_activate(TSRMLS_C);
			zend_execute(
			    EG(active_op_array) TSRMLS_CC);
			php_output_deactivate(TSRMLS_C);
		} zend_catch {
		    EG(active_op_array) = orig_op_array;
		    EG(opline_ptr) = orig_opline;
		    EG(return_value_ptr_ptr) = orig_retval_ptr;

			if (!(PHPDBG_G(flags) & PHPDBG_IS_QUITTING)) {
				phpdbg_error("Caught exit/error from VM");
				goto out;
			}
		} zend_end_try();

		if (EG(exception)) {
			phpdbg_handle_exception(TSRMLS_C);
		}

		EG(active_op_array) = orig_op_array;
	    EG(opline_ptr) = orig_opline;
	    EG(return_value_ptr_ptr) = orig_retval_ptr;

	} else {
		phpdbg_error("Nothing to execute!");
	}

out:
	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(eval) /* {{{ */
{
	switch (param->type) {
		case STR_PARAM: {
			zend_bool stepping = ((PHPDBG_G(flags) & PHPDBG_IS_STEPPING)==PHPDBG_IS_STEPPING);
			zval retval;

			if (!(PHPDBG_G(flags) & PHPDBG_IS_STEPONEVAL)) {
				PHPDBG_G(flags) &= ~ PHPDBG_IS_STEPPING;
			}

			/* disable stepping while eval() in progress */
			PHPDBG_G(flags) |= PHPDBG_IN_EVAL;
			if (zend_eval_stringl(param->str, param->len,
				&retval, "eval()'d code" TSRMLS_CC) == SUCCESS) {
				zend_print_zval_r(
					&retval, 0 TSRMLS_CC);
				phpdbg_writeln(EMPTY);
				zval_dtor(&retval);
			}
			PHPDBG_G(flags) &= ~PHPDBG_IN_EVAL;

			/* switch stepping back on */
			if (stepping &&
				!(PHPDBG_G(flags) & PHPDBG_IS_STEPONEVAL)) {
				PHPDBG_G(flags) |= PHPDBG_IS_STEPPING;
			}
		} break;

		phpdbg_default_switch_case();
	}

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(back) /* {{{ */
{
	if (!EG(in_execution)) {
		phpdbg_error("Not executing!");
		return SUCCESS;
	}

	switch (param->type) {
		case EMPTY_PARAM:
		case NUMERIC_PARAM: {
			zval zbacktrace;
			zval **tmp, **argstmp;
			HashPosition position;
			int i = 0,
				limit = (param->type == NUMERIC_PARAM) ? param->num : 0;

			zval **file, **line, **funcname, **class, **type, **args;
			char *func, is_class;
			long funcsize;

			zend_fetch_debug_backtrace(
				&zbacktrace, 0, 0, limit TSRMLS_CC);

			zend_hash_internal_pointer_reset_ex(Z_ARRVAL(zbacktrace), &position);
			zend_hash_get_current_data_ex(Z_ARRVAL(zbacktrace), (void**)&tmp, &position);
			while (1) {
				zend_hash_find(Z_ARRVAL_PP(tmp), "file", sizeof("file"), (void **)&file);
				zend_hash_find(Z_ARRVAL_PP(tmp), "line", sizeof("line"), (void **)&line);
				zend_hash_move_forward_ex(Z_ARRVAL(zbacktrace), &position);
				if (zend_hash_get_current_data_ex(Z_ARRVAL(zbacktrace), (void**)&tmp, &position) == FAILURE) {
					phpdbg_write("frame #%d {main} at %s:%d", i, Z_STRVAL_PP(file), Z_LVAL_PP(line));
					break;
				}
				zend_hash_find(Z_ARRVAL_PP(tmp), "function", sizeof("function"), (void **)&funcname);
				if ((is_class = zend_hash_find(Z_ARRVAL_PP(tmp), "object", sizeof("object"), (void **)&class)) == FAILURE) {
					is_class = zend_hash_find(Z_ARRVAL_PP(tmp), "class", sizeof("class"), (void **)&class);
				} else {
					zend_get_object_classname(*class, (const char **)&Z_STRVAL_PP(class), (zend_uint *)&Z_STRLEN_PP(class) TSRMLS_CC);
				}
				if (is_class) {
					zend_hash_find(Z_ARRVAL_PP(tmp), "type", sizeof("type"), (void **)&type);
				}

				funcsize = Z_STRLEN_PP(funcname) + (is_class == FAILURE?0:Z_STRLEN_PP(type) + Z_STRLEN_PP(class)) + 1;

				func = emalloc(funcsize + 2);
				phpdbg_write("frame #%d: %s%s%s(", i++, Z_STRVAL_PP(funcname), is_class == FAILURE?"":Z_STRVAL_PP(type), is_class == FAILURE?"":Z_STRVAL_PP(class));

				if (zend_hash_find(Z_ARRVAL_PP(tmp), "args", sizeof("args"), (void **)&args) == SUCCESS) {
					HashPosition iterator;
					int j = 0;

					zend_hash_internal_pointer_reset_ex(Z_ARRVAL_PP(args), &iterator);
					while (zend_hash_get_current_data_ex(Z_ARRVAL_PP(args), (void **) &argstmp, &iterator) == SUCCESS) {
						if (j++) {
							phpdbg_write(", ");
						}
						zend_print_flat_zval_r(*argstmp TSRMLS_CC);
						zend_hash_move_forward_ex(Z_ARRVAL_PP(args), &iterator);
					}
				}

				phpdbg_writeln(") at %s:%d", Z_STRVAL_PP(file), Z_LVAL_PP(line));
			}

			phpdbg_writeln(EMPTY);
			zval_dtor(&zbacktrace);
		} break;

		phpdbg_default_switch_case();
	}

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(print) /* {{{ */
{
	switch (param->type) {
		case EMPTY_PARAM: {
			phpdbg_writeln(SEPARATE);
			phpdbg_notice("Execution Context Information");
#ifdef HAVE_LIBREADLINE
			phpdbg_writeln("Readline\tyes");
#else
			phpdbg_writeln("Readline\tno");
#endif

			phpdbg_writeln("Exec\t\t%s", PHPDBG_G(exec) ? PHPDBG_G(exec) : "none");
			phpdbg_writeln("Compiled\t%s", PHPDBG_G(ops) ? "yes" : "no");
			phpdbg_writeln("Stepping\t%s", (PHPDBG_G(flags) & PHPDBG_IS_STEPPING) ? "on" : "off");
			phpdbg_writeln("Quietness\t%s", (PHPDBG_G(flags) & PHPDBG_IS_QUIET) ? "on" : "off");
			phpdbg_writeln("Oplog\t\t%s", PHPDBG_G(oplog) ? "on" : "off");

			if (PHPDBG_G(ops)) {
				phpdbg_writeln("Opcodes\t\t%d", PHPDBG_G(ops)->last);

				if (PHPDBG_G(ops)->last_var) {
					phpdbg_writeln("Variables\t%d", PHPDBG_G(ops)->last_var-1);
				} else {
					phpdbg_writeln("Variables\tNone");
				}
			}

			phpdbg_writeln("Executing\t%s", EG(in_execution) ? "yes" : "no");
			if (EG(in_execution)) {
				phpdbg_writeln("VM Return\t%d", PHPDBG_G(vmret));
			}
			phpdbg_writeln("Classes\t\t%d", zend_hash_num_elements(EG(class_table)));
			phpdbg_writeln("Functions\t%d", zend_hash_num_elements(EG(function_table)));
			phpdbg_writeln("Constants\t%d", zend_hash_num_elements(EG(zend_constants)));
			phpdbg_writeln("Included\t%d", zend_hash_num_elements(&EG(included_files)));

			phpdbg_print_breakpoints(PHPDBG_BREAK_FILE TSRMLS_CC);
			phpdbg_print_breakpoints(PHPDBG_BREAK_SYM TSRMLS_CC);
			phpdbg_print_breakpoints(PHPDBG_BREAK_METHOD TSRMLS_CC);
			phpdbg_print_breakpoints(PHPDBG_BREAK_OPLINE TSRMLS_CC);
			phpdbg_print_breakpoints(PHPDBG_BREAK_COND TSRMLS_CC);

			phpdbg_writeln(SEPARATE);
		} break;

		phpdbg_default_switch_case();
	}

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(info) /* {{{ */
{
	phpdbg_error(
		"No information command selected !");

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(break) /* {{{ */
{
	switch (param->type) {
		case ADDR_PARAM:
			phpdbg_set_breakpoint_opline(param->addr TSRMLS_CC);
			break;
		case NUMERIC_PARAM:
			phpdbg_set_breakpoint_file(phpdbg_current_file(TSRMLS_C), param->num TSRMLS_CC);
			break;
		case METHOD_PARAM:
			phpdbg_set_breakpoint_method(param->method.class, param->method.name TSRMLS_CC);
			break;
		case FILE_PARAM:
			phpdbg_set_breakpoint_file(param->file.name, param->file.line TSRMLS_CC);
			break;
		case STR_PARAM:
			phpdbg_set_breakpoint_symbol(param->str TSRMLS_CC);
			break;

		phpdbg_default_switch_case();
	}

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(shell) /* {{{ */
{
	/* don't allow this to loop, ever ... */
	switch (param->type) {
		case STR_PARAM: {
			FILE *fd = NULL;
			if ((fd=VCWD_POPEN((char*)param->str, "w"))) {
				/* do something perhaps ?? do we want input ?? */
				fclose(fd);
			} else {
				phpdbg_error(
					"Failed to execute %s", param->str);
			}
		} break;

		phpdbg_default_switch_case();
	}
	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(register) /* {{{ */
{
	switch (param->type) {
		case STR_PARAM: {
			zend_function *function;
			char *lcname = zend_str_tolower_dup(param->str, param->len);
			size_t lcname_len = strlen(lcname);

			if (!zend_hash_exists(&PHPDBG_G(registered), lcname, lcname_len+1)) {
				if (zend_hash_find(EG(function_table), lcname, lcname_len+1, (void**) &function) == SUCCESS) {
					zend_hash_update(
						&PHPDBG_G(registered), lcname, lcname_len+1, (void*)&function, sizeof(zend_function), NULL);
					function_add_ref(function);

					phpdbg_notice(
						"Registered %s", lcname);
				} else {
					phpdbg_error("The requested function (%s) could not be found", param->str);
				}
			} else {
				phpdbg_error(
					"The requested name (%s) is already in use", lcname);
			}

			efree(lcname);
		} break;

		phpdbg_default_switch_case();
	}
	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(quit) /* {{{ */
{
    /* don't allow this to loop, ever ... */
	if (!(PHPDBG_G(flags) & PHPDBG_IS_QUITTING)) {

		PHPDBG_G(flags) |= PHPDBG_IS_QUITTING;
		zend_bailout();
	}

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(clean) /* {{{ */
{
	if (EG(in_execution)) {
		phpdbg_error("Cannot clean environment while executing");
		return SUCCESS;
	}

	phpdbg_notice("Cleaning Execution Environment");

	phpdbg_writeln("Classes\t\t\t%d", zend_hash_num_elements(EG(class_table)));
	phpdbg_writeln("Functions\t\t%d", zend_hash_num_elements(EG(function_table)));
	phpdbg_writeln("Constants\t\t%d", zend_hash_num_elements(EG(zend_constants)));
	phpdbg_writeln("Includes\t\t%d", zend_hash_num_elements(&EG(included_files)));

	phpdbg_clean(1 TSRMLS_CC);

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(clear) /* {{{ */
{
	phpdbg_notice("Clearing Breakpoints");

	phpdbg_writeln("File\t\t\t%d", zend_hash_num_elements(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE]));
	phpdbg_writeln("Functions\t\t%d", zend_hash_num_elements(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM]));
	phpdbg_writeln("Methods\t\t\t%d", zend_hash_num_elements(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD]));
	phpdbg_writeln("Oplines\t\t\t%d", zend_hash_num_elements(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE]));
	phpdbg_writeln("Conditionals\t\t%d", zend_hash_num_elements(&PHPDBG_G(bp)[PHPDBG_BREAK_COND]));

	phpdbg_clear_breakpoints(TSRMLS_C);

    return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(aliases) /* {{{ */
{
	const phpdbg_command_t *prompt_command = phpdbg_prompt_commands;

	phpdbg_help_header();
	phpdbg_writeln("Below are the aliased, short versions of all supported commands");
	while (prompt_command && prompt_command->name) {
		if (prompt_command->alias) {
			if (prompt_command->subs) {
				const phpdbg_command_t *sub_command = prompt_command->subs;
				phpdbg_writeln(EMPTY);
				phpdbg_writeln(" %c -> %9s", prompt_command->alias, prompt_command->name);
				while (sub_command && sub_command->name) {
					if (sub_command->alias) {
						phpdbg_writeln(" |-------- %c -> %15s\t%s", sub_command->alias,
							sub_command->name, sub_command->tip);
					}
					++sub_command;
				}
				phpdbg_writeln(EMPTY);
			} else {
				phpdbg_writeln(" %c -> %9s\t\t\t%s", prompt_command->alias,
					prompt_command->name, prompt_command->tip);
			}
		}

		++prompt_command;
	}
	phpdbg_help_footer();

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(oplog) /* {{{ */
{
	switch (param->type) {
		case EMPTY_PARAM:
		case NUMERIC_PARAM:
			if ((param->type != NUMERIC_PARAM) || !param->num) {
				if (PHPDBG_G(oplog)) {
					phpdbg_notice("Disabling oplog");
					fclose(
						PHPDBG_G(oplog));
				} else {
					phpdbg_error("No oplog currently open !");
				}
			} else {
				phpdbg_error(
					"No action taken !");
			}
		break;

		case STR_PARAM: {
			/* open oplog */
			FILE *old = PHPDBG_G(oplog);

			PHPDBG_G(oplog) = fopen(param->str, "w+");
			if (!PHPDBG_G(oplog)) {
				phpdbg_error("Failed to open %s for oplog", param->str);
				PHPDBG_G(oplog) = old;
			} else {
				if (old) {
					phpdbg_notice("Closing previously open oplog");
					fclose(old);
				}
				phpdbg_notice("Successfully opened oplog %s", param->str);
			}
		} break;

		phpdbg_default_switch_case();
	}

    return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(help) /* {{{ */
{
    switch (param->type) {
        case EMPTY_PARAM: {
			const phpdbg_command_t *prompt_command = phpdbg_prompt_commands;
			const phpdbg_command_t *help_command = phpdbg_help_commands;

			phpdbg_help_header();
			phpdbg_writeln("To get help regarding a specific command type \"help command\"");

			phpdbg_notice("Commands");

			while (prompt_command && prompt_command->name) {
				phpdbg_writeln(
					" %10s\t%s", prompt_command->name, prompt_command->tip);
				++prompt_command;
			}

			phpdbg_notice("Help Commands");

			while (help_command && help_command->name) {
				phpdbg_writeln(" %10s\t%s", help_command->name, help_command->tip);
				++help_command;
			}

			phpdbg_notice("Command Line Options and Flags");
			phpdbg_writeln(" Option\tExample\t\t\tPurpose");
			phpdbg_writeln(EMPTY);
			phpdbg_writeln(" -c\t-c/my/php.ini\t\tSet php.ini file to load");
			phpdbg_writeln(" -d\t-dmemory_limit=4G\tSet a php.ini directive");
			phpdbg_writeln(" -n\tN/A\t\t\tDisable default php.ini");
			phpdbg_writeln(" -q\tN/A\t\t\tSupress welcome banner");
			phpdbg_writeln(" -e\t-emytest.php\t\tSet execution context");
			phpdbg_writeln(" -v\tN/A\t\t\tEnable oplog output");
			phpdbg_writeln(" -s\tN/A\t\t\tEnable stepping");
			phpdbg_writeln(" -b\tN/A\t\t\tDisable colour");
			phpdbg_writeln(" -i\t-imy.init\t\tSet .phpdbginit file");
			phpdbg_writeln(" -I\tN/A\t\t\tIgnore default .phpdbginit");
			phpdbg_writeln(" -O\t-Omy.oplog\t\tSets oplog output file");
			phpdbg_writeln(" -r\tN/A\t\t\tRun execution context");
			phpdbg_writeln(" -E\tN/A\t\t\tEnable step through eval, careful !");
			phpdbg_notice(
				"Note: passing -rr will cause phpdbg to quit after execution");
			phpdbg_help_footer();
		} break;

		default: {
			phpdbg_error(
				"No help can be found for the subject \"%s\"", param->str);
		}
	}

	return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(quiet) /* {{{ */
{
	switch (param->type) {
		case NUMERIC_PARAM: {
			if (param->num) {
				PHPDBG_G(flags) |= PHPDBG_IS_QUIET;
			} else {
				PHPDBG_G(flags) &= ~PHPDBG_IS_QUIET;
			}
			phpdbg_notice("Quietness %s",
				(PHPDBG_G(flags) & PHPDBG_IS_QUIET) ? "enabled" : "disabled");
		} break;

		phpdbg_default_switch_case();
	}

    return SUCCESS;
} /* }}} */

PHPDBG_COMMAND(list) /* {{{ */
{
	switch (param->type) {
        case NUMERIC_PARAM:
	    case EMPTY_PARAM:
			return PHPDBG_LIST_HANDLER(lines)(PHPDBG_COMMAND_ARGS);

		case FILE_PARAM:
			return PHPDBG_LIST_HANDLER(lines)(PHPDBG_COMMAND_ARGS);

		case STR_PARAM:
		    phpdbg_list_function_byname(param->str, param->len TSRMLS_CC);
			break;

		case METHOD_PARAM:
		    return PHPDBG_LIST_HANDLER(method)(PHPDBG_COMMAND_ARGS);

		phpdbg_default_switch_case();
    }

	return SUCCESS;
} /* }}} */

static inline int phpdbg_call_register(phpdbg_input_t *input TSRMLS_DC) /* {{{ */
{
	phpdbg_input_t *function = input->argv[0];

	if (zend_hash_exists(
		&PHPDBG_G(registered), function->string, function->length+1)) {

		zval fname, *fretval;
		zend_fcall_info fci;

		ZVAL_STRINGL(&fname, function->string, function->length, 1);

		memset(&fci, 0, sizeof(zend_fcall_info));

		fci.size = sizeof(zend_fcall_info);
		fci.function_table = &PHPDBG_G(registered);
		fci.function_name = &fname;
		fci.symbol_table = EG(active_symbol_table);
		fci.object_ptr = NULL;
		fci.retval_ptr_ptr = &fretval;
		fci.no_separation = 1;

		if (input->argc > 1) {
			int param;
			zval params;

			array_init(&params);

			for (param = 0; param < (input->argc-1); param++) {
				add_next_index_stringl(
					&params,
					input->argv[param+1]->string,
					input->argv[param+1]->length, 1);

				phpdbg_debug(
					"created param[%d] from argv[%d]: %s",
					param, param+1, input->argv[param+1]->string);
			}

			zend_fcall_info_args(&fci, &params TSRMLS_CC);
		} else {
			fci.params = NULL;
			fci.param_count = 0;
		}

		phpdbg_debug(
			"created %d params from %d argvuments",
			fci.param_count, input->argc);

		zend_call_function(&fci, NULL TSRMLS_CC);

		if (fretval) {
			zend_print_zval_r(
				fretval, 0 TSRMLS_CC);
			phpdbg_writeln(EMPTY);
		}

		zval_dtor(&fname);

		return SUCCESS;
	}

	return FAILURE;
} /* }}} */

int phpdbg_interactive(TSRMLS_D) /* {{{ */
{
	int ret = SUCCESS;

	PHPDBG_G(flags) |= PHPDBG_IS_INTERACTIVE;

	phpdbg_input_t *input = phpdbg_read_input(NULL TSRMLS_CC);
	
	if (input && input->length > 0L) {
		do {
			switch (ret = phpdbg_do_cmd(phpdbg_prompt_commands, input TSRMLS_CC)) {
				case FAILURE:
					if (!(PHPDBG_G(flags) & PHPDBG_IS_QUITTING)) {
						if (phpdbg_call_register(input TSRMLS_CC) == FAILURE) {
							phpdbg_error("Failed to execute %s!", input->string);
						}
					}
				break;

				case PHPDBG_LEAVE:
				case PHPDBG_FINISH:
				case PHPDBG_UNTIL:
				case PHPDBG_NEXT: {
					if (!EG(in_execution)) {
						phpdbg_error("Not running");
					}
					goto out;
				}
			}

			phpdbg_destroy_input(&input TSRMLS_CC);
		} while ((input = phpdbg_read_input(NULL TSRMLS_CC)) && (input->length > 0L));

		if (input && !input->length)
			goto last;

	} else {
last:
		if (PHPDBG_G(lcmd)) {
			ret = PHPDBG_G(lcmd)->handler(
					&PHPDBG_G(lparam), input TSRMLS_CC);
			goto out;
		}
	}

out:
	phpdbg_destroy_input(&input TSRMLS_CC);

	if (EG(in_execution)) {
		restore_frame(TSRMLS_C);
	}

	PHPDBG_G(flags) &= ~PHPDBG_IS_INTERACTIVE;

	return ret;
} /* }}} */

void phpdbg_clean(zend_bool full TSRMLS_DC) /* {{{ */
{
	/* this is implicitly required */
	if (PHPDBG_G(ops)) {
		destroy_op_array(PHPDBG_G(ops) TSRMLS_CC);
		efree(PHPDBG_G(ops));
		PHPDBG_G(ops) = NULL;
	}

	if (full) {
		PHPDBG_G(flags) |= PHPDBG_IS_CLEANING;

		zend_bailout();
	}
} /* }}} */

static inline zend_execute_data *phpdbg_create_execute_data(zend_op_array *op_array, zend_bool nested TSRMLS_DC) /* {{{ */
{
#if PHP_VERSION_ID >= 50500
    return zend_create_execute_data_from_op_array(op_array, nested TSRMLS_CC);
#else

#undef EX
#define EX(element) execute_data->element
#undef EX_CV
#define EX_CV(var) EX(CVs)[var]
#undef EX_CVs
#define EX_CVs() EX(CVs)
#undef EX_T
#define EX_T(offset) (*(temp_variable *)((char *) EX(Ts) + offset))
#undef EX_Ts
#define EX_Ts() EX(Ts)

    zend_execute_data *execute_data = (zend_execute_data *)zend_vm_stack_alloc(
		ZEND_MM_ALIGNED_SIZE(sizeof(zend_execute_data)) +
		ZEND_MM_ALIGNED_SIZE(sizeof(zval**) * op_array->last_var * (EG(active_symbol_table) ? 1 : 2)) +
		ZEND_MM_ALIGNED_SIZE(sizeof(temp_variable)) * op_array->T TSRMLS_CC);

	EX(CVs) = (zval***)((char*)execute_data + ZEND_MM_ALIGNED_SIZE(sizeof(zend_execute_data)));
	memset(EX(CVs), 0, sizeof(zval**) * op_array->last_var);
	EX(Ts) = (temp_variable *)(((char*)EX(CVs)) + ZEND_MM_ALIGNED_SIZE(sizeof(zval**) * op_array->last_var * (EG(active_symbol_table) ? 1 : 2)));
	EX(fbc) = NULL;
	EX(called_scope) = NULL;
	EX(object) = NULL;
	EX(old_error_reporting) = NULL;
	EX(op_array) = op_array;
	EX(symbol_table) = EG(active_symbol_table);
	EX(prev_execute_data) = EG(current_execute_data);
	EG(current_execute_data) = execute_data;
	EX(nested) = nested;

	if (!op_array->run_time_cache && op_array->last_cache_slot) {
		op_array->run_time_cache = ecalloc(op_array->last_cache_slot, sizeof(void*));
	}

	if (op_array->this_var != -1 && EG(This)) {
 		Z_ADDREF_P(EG(This)); /* For $this pointer */
		if (!EG(active_symbol_table)) {
			EX_CV(op_array->this_var) = (zval**)EX_CVs() + (op_array->last_var + op_array->this_var);
			*EX_CV(op_array->this_var) = EG(This);
		} else {
			if (zend_hash_add(EG(active_symbol_table), "this", sizeof("this"), &EG(This), sizeof(zval *), (void**)&EX_CV(op_array->this_var))==FAILURE) {
				Z_DELREF_P(EG(This));
			}
		}
	}

	EX(opline) = UNEXPECTED((op_array->fn_flags & ZEND_ACC_INTERACTIVE) != 0) && EG(start_op) ? EG(start_op) : op_array->opcodes;
	EG(opline_ptr) = &EX(opline);

	EX(function_state).function = (zend_function *) op_array;
	EX(function_state).arguments = NULL;

	return execute_data;
#endif
} /* }}} */

#if PHP_VERSION_ID >= 50500
void phpdbg_execute_ex(zend_execute_data *execute_data TSRMLS_DC) /* {{{ */
{
#else
void phpdbg_execute_ex(zend_op_array *op_array TSRMLS_DC) /* {{{ */
{
	long long flags = 0;
	zend_ulong address = 0L;
	zend_execute_data *execute_data;
	zend_bool nested = 0;
#endif
	zend_bool original_in_execution = EG(in_execution);
	HashTable vars;

#if PHP_VERSION_ID < 50500
	if (EG(exception)) {
		return;
	}
#endif

	EG(in_execution) = 1;

#if PHP_VERSION_ID >= 50500
	if (0) {
zend_vm_enter:
		execute_data = phpdbg_create_execute_data(EG(active_op_array), 1 TSRMLS_CC);
	}
	zend_hash_init(&vars, EG(active_op_array)->last, NULL, NULL, 0);
#else
zend_vm_enter:
	execute_data = phpdbg_create_execute_data(op_array, nested TSRMLS_CC);
	nested = 1;
	zend_hash_init(&vars, EG(active_op_array)->last, NULL, NULL, 0);
#endif

	while (1) {
#ifdef ZEND_WIN32
		if (EG(timed_out)) {
			zend_timeout(0);
		}
#endif

#define DO_INTERACTIVE() do {\
	if (!(PHPDBG_G(flags) & PHPDBG_IN_EVAL)) {\
		phpdbg_list_file(\
			zend_get_executed_filename(TSRMLS_C), \
			3, \
			zend_get_executed_lineno(TSRMLS_C)-1, \
			zend_get_executed_lineno(TSRMLS_C) \
			TSRMLS_CC\
		);\
	}\
	\
	do {\
		switch (phpdbg_interactive(TSRMLS_C)) {\
			case PHPDBG_LEAVE:\
			case PHPDBG_FINISH:\
			case PHPDBG_UNTIL:\
			case PHPDBG_NEXT:{\
		    	goto next;\
			}\
		}\
	} while(!(PHPDBG_G(flags) & PHPDBG_IS_QUITTING));\
} while(0)

		/* allow conditional breakpoints and
			initialization to access the vm uninterrupted */
		if ((PHPDBG_G(flags) & PHPDBG_IN_COND_BP) ||
			(PHPDBG_G(flags) & PHPDBG_IS_INITIALIZING)) {
			/* skip possible breakpoints */
			goto next;
		}

		/* perform seek operation */
		if (PHPDBG_G(flags) & PHPDBG_SEEK_MASK) {
			/* current address */
			zend_ulong address = (zend_ulong) execute_data->opline;

			/* run to next line */
			if (PHPDBG_G(flags) & PHPDBG_IN_UNTIL) {
				if (zend_hash_index_exists(&PHPDBG_G(seek), address)) {
					PHPDBG_G(flags) &= ~PHPDBG_IN_UNTIL;
					zend_hash_clean(
						&PHPDBG_G(seek));
				} else {
					/* skip possible breakpoints */
					goto next;
				}
			}

			/* run to finish */
			if (PHPDBG_G(flags) & PHPDBG_IN_FINISH) {
				if (zend_hash_index_exists(&PHPDBG_G(seek), address)) {
					PHPDBG_G(flags) &= ~PHPDBG_IN_FINISH;
					zend_hash_clean(
						&PHPDBG_G(seek));
				}
				/* skip possible breakpoints */
				goto next;
			}

			/* break for leave */
			if (PHPDBG_G(flags) & PHPDBG_IN_LEAVE) {
				if (zend_hash_index_exists(&PHPDBG_G(seek), address)) {
					PHPDBG_G(flags) &= ~PHPDBG_IN_LEAVE;
					zend_hash_clean(
						&PHPDBG_G(seek));
					phpdbg_notice(
						"Breaking for leave at %s:%u",
						zend_get_executed_filename(TSRMLS_C),
						zend_get_executed_lineno(TSRMLS_C)
					);
					DO_INTERACTIVE();
				} else {
					/* skip possible breakpoints */
					goto next;
				}
			}
		}

		/* not while in conditionals */
		phpdbg_print_opline_ex(
			execute_data, &vars, 0 TSRMLS_CC);

		/* conditions cannot be executed by eval()'d code */
		if (!(PHPDBG_G(flags) & PHPDBG_IN_EVAL)
			&& (PHPDBG_G(flags) & PHPDBG_HAS_COND_BP)
			&& phpdbg_find_conditional_breakpoint(TSRMLS_C) == SUCCESS) {
			DO_INTERACTIVE();
		}

		if ((PHPDBG_G(flags) & PHPDBG_HAS_FILE_BP)
			&& phpdbg_find_breakpoint_file(execute_data->op_array TSRMLS_CC) == SUCCESS) {
			DO_INTERACTIVE();
		}

		if ((PHPDBG_G(flags) & (PHPDBG_HAS_METHOD_BP|PHPDBG_HAS_SYM_BP))) {
			/* check we are at the beginning of the stack */
			if (execute_data->opline == EG(active_op_array)->opcodes) {
				if (phpdbg_find_breakpoint_symbol(
						execute_data->function_state.function TSRMLS_CC) == SUCCESS) {
					DO_INTERACTIVE();
				}
			}
		}

		if ((PHPDBG_G(flags) & PHPDBG_HAS_OPLINE_BP)
			&& phpdbg_find_breakpoint_opline(execute_data->opline TSRMLS_CC) == SUCCESS) {
			DO_INTERACTIVE();
		}

		if ((PHPDBG_G(flags) & PHPDBG_IS_STEPPING)) {
			DO_INTERACTIVE();
		}

next:
		if (PHPDBG_G(flags) & PHPDBG_IS_SIGNALED) {
			phpdbg_writeln(EMPTY);
			phpdbg_notice("Program received signal SIGINT");
			PHPDBG_G(flags) &= ~PHPDBG_IS_SIGNALED;
			DO_INTERACTIVE();
		}

		PHPDBG_G(vmret) = execute_data->opline->handler(execute_data TSRMLS_CC);

		if (PHPDBG_G(vmret) > 0) {
			switch (PHPDBG_G(vmret)) {
				case 1:
					EG(in_execution) = original_in_execution;
					zend_hash_destroy(&vars);
					return;
				case 2:
#if PHP_VERSION_ID < 50500
					op_array = EG(active_op_array);
#endif
					zend_hash_destroy(&vars);
					goto zend_vm_enter;
					break;
				case 3:
					execute_data = EG(current_execute_data);
					break;
				default:
					break;
			}
		}
	}
	zend_error_noreturn(E_ERROR, "Arrived at end of main loop which shouldn't happen");
} /* }}} */
