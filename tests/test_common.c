/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_common
 *   - tester for common signal handlers and application constants
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include <stdio.h>
#include <string.h>

static int check_count = 0;
static int failed_count = 0;

static void check_int(long actual, long expected, const char *what)
{
	check_count++;

	if (actual != expected)
	{
		failed_count++;
		printf("FAILED: %s = %ld, expected %ld\n", what, actual, expected);
	}
}

static void check_true(int ok, const char *what)
{
	check_count++;

	if (!ok)
	{
		failed_count++;
		printf("FAILED: %s\n", what);
	}
}

static void section_report(int n)
{
	printf("Completed testing #%d: %d of %d checks passed\n", n, check_count - failed_count, check_count);
}

static void test_signal_handlers(void)
{
	printf("Testing #1 ...\n");

	check_int(SYS_conf_reload, 0, "SYS_conf_reload is 0 before the HUP handler runs");
	check_int(SYS_server_exit, 0, "SYS_server_exit is 0 before the TERM handler runs");
	check_int(SYS_child_exit, 0, "SYS_child_exit is 0 before the CHLD handler runs");
	check_int(SYS_child_process_count, 0, "SYS_child_process_count is 0 before any handler runs");

	// The handlers ignore the signum argument and only flip the corresponding volatile flag
	sig_hup_handler(1);
	check_int(SYS_conf_reload, 1, "sig_hup_handler() raises SYS_conf_reload");
	check_int(SYS_server_exit, 0, "sig_hup_handler() leaves SYS_server_exit alone");
	check_int(SYS_child_exit, 0, "sig_hup_handler() leaves SYS_child_exit alone");

	sig_term_handler(15);
	check_int(SYS_server_exit, 1, "sig_term_handler() raises SYS_server_exit");
	check_int(SYS_conf_reload, 1, "sig_term_handler() leaves SYS_conf_reload raised");
	check_int(SYS_child_exit, 0, "sig_term_handler() leaves SYS_child_exit alone");

	sig_chld_handler(17);
	check_int(SYS_child_exit, 1, "sig_chld_handler() raises SYS_child_exit");
	check_int(SYS_conf_reload, 1, "sig_chld_handler() leaves SYS_conf_reload raised");
	check_int(SYS_server_exit, 1, "sig_chld_handler() leaves SYS_server_exit raised");

	// Re-invocation is idempotent: the flags are set to 1, not incremented
	sig_hup_handler(1);
	sig_term_handler(15);
	sig_chld_handler(17);
	check_int(SYS_conf_reload, 1, "sig_hup_handler() stays at 1 on a second HUP");
	check_int(SYS_server_exit, 1, "sig_term_handler() stays at 1 on a second TERM");
	check_int(SYS_child_exit, 1, "sig_chld_handler() stays at 1 on a second CHLD");

	section_report(1);
}

static void test_constants(void)
{
	printf("Testing #2 ...\n");

	// Values that other modules and wire formats depend on; a silent change breaks them
	check_int(SCREEN_ROWS, 24, "SCREEN_ROWS is 24");
	check_int(SCREEN_COLS, 80, "SCREEN_COLS is 80");
	check_int(LINE_BUFFER_LEN, 1024, "LINE_BUFFER_LEN is 1024");
	check_int(FILE_PATH_LEN, 4096, "FILE_PATH_LEN is 4096");
	check_int(FILE_NAME_LEN, 256, "FILE_NAME_LEN is 256");
	check_int(TAB_SIZE, 4, "TAB_SIZE is 4");
	check_int(MAX_CLIENT_LIMIT, 10000, "MAX_CLIENT_LIMIT is 10000");
	check_int(MAX_CLIENT_PER_IP_LIMIT, 10000, "MAX_CLIENT_PER_IP_LIMIT is 10000");
	check_int(MAX_CLIENT_PER_IP_LIMIT <= MAX_CLIENT_LIMIT, 1, "the per-IP cap fits inside the global cap");
	check_int(IP_ADDR_LEN, 50, "IP_ADDR_LEN is 50");
	check_int(MAX_EVENTS, 10, "MAX_EVENTS is 10");

	section_report(2);
}

static void test_strings(void)
{
	printf("Testing #3 ...\n");

	check_true(strlen(APP_INFO) > 0, "APP_INFO is not empty");
	check_true(strstr(APP_INFO, PACKAGE_STRING) != NULL, "APP_INFO starts with PACKAGE_STRING");
	check_true(strstr(APP_INFO, "build (") != NULL, "APP_INFO records the build architecture");

	check_true(strstr(COPYRIGHT_INFO, "Copyright (C) 2004-2026") != NULL, "COPYRIGHT_INFO names the copyright range");
	check_true(strstr(COPYRIGHT_INFO, "ABSOLUTELY NO WARRANTY") != NULL, "COPYRIGHT_INFO includes the warranty notice");
	check_true(strstr(COPYRIGHT_INFO, "GNU General Public License") != NULL, "COPYRIGHT_INFO cites the GNU GPL");

	// Paths the daemon expects at startup; a typo would break deployment without a compile error
	check_true(strcmp(CONF_BBSD, "conf/bbsd.conf") == 0, "CONF_BBSD points at the daemon configuration");
	check_true(strcmp(CONF_MENU, "var/menu_merged.conf") == 0, "CONF_MENU points at the merged menu");
	check_true(strcmp(CONF_BBSNET, "conf/bbsnet.conf") == 0, "CONF_BBSNET points at the bbsnet configuration");
	check_true(strcmp(CONF_BWF, "conf/badwords.conf") == 0, "CONF_BWF points at the bad-word filter");
	check_true(strcmp(CONF_TOP10_MENU, "var/bbs_top_menu.conf") == 0, "CONF_TOP10_MENU points at the top-10 menu");
	check_true(strcmp(LOG_FILE_INFO, "log/bbsd.log") == 0, "LOG_FILE_INFO points at the info log");
	check_true(strcmp(LOG_FILE_ERROR, "log/error.log") == 0, "LOG_FILE_ERROR points at the error log");

	// The ".~" suffix is the shared memory placeholder; changing it orphans the running shm segments
	check_true(strcmp(VAR_ARTICLE_BLOCK_SHM, "var/article_block_shm.~") == 0, "VAR_ARTICLE_BLOCK_SHM has the shm suffix");
	check_true(strcmp(VAR_SECTION_LIST_SHM, "var/section_list_shm.~") == 0, "VAR_SECTION_LIST_SHM has the shm suffix");
	check_true(strcmp(VAR_TRIE_DICT_SHM, "var/trie_dict_shm.~") == 0, "VAR_TRIE_DICT_SHM has the shm suffix");
	check_true(strcmp(VAR_USER_LIST_SHM, "var/user_list_shm.~") == 0, "VAR_USER_LIST_SHM has the shm suffix");

	section_report(3);
}

static void test_startup_data_files(void)
{
	printf("Testing #4 ...\n");

	// The array drives file_loader's startup preload; count and order are visible in the UI
	check_int(data_files_load_startup_count, 12, "data_files_load_startup has 12 entries");
	check_true(data_files_load_startup[0] == DATA_WELCOME, "the first startup file is DATA_WELCOME");
	check_true(data_files_load_startup[1] == DATA_REGISTER, "the second startup file is DATA_REGISTER");
	check_true(data_files_load_startup[2] == DATA_GOODBYE, "the third startup file is DATA_GOODBYE");
	check_true(data_files_load_startup[data_files_load_startup_count - 1] == VAR_BBS_TOP,
			"the last startup file is VAR_BBS_TOP");

	section_report(4);
}

int main(void)
{
	test_signal_handlers();
	test_constants();
	test_strings();
	test_startup_data_files();

	if (failed_count > 0)
	{
		printf("Failed: %d of %d checks\n", failed_count, check_count);

		return -1;
	}

	printf("Done\n");

	return 0;
}
