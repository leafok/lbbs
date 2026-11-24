/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_bbs_cmd
 *   - tester for the menu command handler table
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bbs_cmd.h"
#include "data_file_mgr.h"
#include "log.h"
#include "menu_proc.h"
#include "trie_dict.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// The bbs_cmd table takes the address of every handler declared by menu_proc.h and
// data_file_mgr.h, so the test only needs the symbols to link: nothing here invokes them.
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

static void check_ptr(bbs_cmd_handler actual, bbs_cmd_handler expected, const char *what)
{
	check_count++;

	if (actual != expected)
	{
		failed_count++;
		printf("FAILED: %s\n", what);
	}
}

static const char TRIE_DICT_SHM_FILE[] = "~trie_dict_shm.dat";

// bbs_cmd.c's static table takes the address of every menu_proc and data_file_mgr handler,
// so the test needs each symbol at link time. Nothing here calls through them: the checks
// compare the address that get_cmd_handler returns against the same stub address.
static int stub_handler(void *param)
{
	(void)param;

	return MENU_OK;
}

int list_section(void *param) { return stub_handler(param); }
int exec_mbem(void *param) { return stub_handler(param); }
int exit_bbs(void *param) { return stub_handler(param); }
int eula(void *param) { return stub_handler(param); }
int license(void *param) { return stub_handler(param); }
int copyright(void *param) { return stub_handler(param); }
int version(void *param) { return stub_handler(param); }
int reload_bbs_conf(void *param) { return stub_handler(param); }
int shutdown_bbs(void *param) { return stub_handler(param); }
int favor_section_filter(void *param) { return stub_handler(param); }
int view_ex_article(void *param) { return stub_handler(param); }
int list_ex_section(void *param) { return stub_handler(param); }
int show_top10_menu(void *param) { return stub_handler(param); }
int locate_article(void *param) { return stub_handler(param); }
int favor_topic(void *param) { return stub_handler(param); }
int list_user(void *param) { return stub_handler(param); }
int list_online_user(void *param) { return stub_handler(param); }
int edit_intro(void *param) { return stub_handler(param); }
int edit_sign(void *param) { return stub_handler(param); }
int data_file_mgr(void *param) { return stub_handler(param); }

static void section_report(int n)
{
	printf("Completed testing #%d: %d of %d checks passed\n", n, check_count - failed_count, check_count);
}

static void test_load_state(void)
{
	bbs_cmd_handler h;

	printf("Testing #1 ...\n");

	// Before load_cmd() the dictionary pointer is NULL and no lookup should succeed
	h = get_cmd_handler("LIST_SECTION");
	check_ptr(h, NULL, "get_cmd_handler() is NULL before load_cmd()");
	h = get_cmd_handler("");
	check_ptr(h, NULL, "get_cmd_handler(\"\") is NULL before load_cmd()");

	check_int(load_cmd(), 0, "load_cmd() succeeds on the first call");
	// load_cmd() refuses to re-init when the dictionary is already up
	check_int(load_cmd(), -1, "load_cmd() returns -1 when the dictionary is already loaded");

	unload_cmd();

	// unload_cmd() drops the dictionary pointer, so lookups return NULL again
	h = get_cmd_handler("LIST_SECTION");
	check_ptr(h, NULL, "get_cmd_handler() is NULL after unload_cmd()");

	section_report(1);
}

static void test_lookup(void)
{
	printf("Testing #2 ...\n");

	check_int(load_cmd(), 0, "load_cmd() re-arms the dictionary");

	// Every registered command resolves to the handler named in bbs_cmd_list
	check_ptr(get_cmd_handler("LIST_SECTION"), (bbs_cmd_handler)list_section, "\"LIST_SECTION\" maps to list_section");
	check_ptr(get_cmd_handler("RunMBEM"), (bbs_cmd_handler)exec_mbem, "\"RunMBEM\" maps to exec_mbem");
	check_ptr(get_cmd_handler("EXITBBS"), (bbs_cmd_handler)exit_bbs, "\"EXITBBS\" maps to exit_bbs");
	check_ptr(get_cmd_handler("EULA"), (bbs_cmd_handler)eula, "\"EULA\" maps to eula");
	check_ptr(get_cmd_handler("LICENSE"), (bbs_cmd_handler)license, "\"LICENSE\" maps to license");
	check_ptr(get_cmd_handler("COPYRIGHT"), (bbs_cmd_handler)copyright, "\"COPYRIGHT\" maps to copyright");
	check_ptr(get_cmd_handler("VERSION"), (bbs_cmd_handler)version, "\"VERSION\" maps to version");
	check_ptr(get_cmd_handler("RELOADCONF"), (bbs_cmd_handler)reload_bbs_conf, "\"RELOADCONF\" maps to reload_bbs_conf");
	check_ptr(get_cmd_handler("DATAMGR"), (bbs_cmd_handler)data_file_mgr, "\"DATAMGR\" maps to data_file_mgr");
	check_ptr(get_cmd_handler("SHUTDOWN"), (bbs_cmd_handler)shutdown_bbs, "\"SHUTDOWN\" maps to shutdown_bbs");
	check_ptr(get_cmd_handler("M_FAVOR_SECTION"), (bbs_cmd_handler)favor_section_filter,
			"\"M_FAVOR_SECTION\" maps to favor_section_filter");
	check_ptr(get_cmd_handler("VIEW_EX_ARTICLE"), (bbs_cmd_handler)view_ex_article,
			"\"VIEW_EX_ARTICLE\" maps to view_ex_article");
	check_ptr(get_cmd_handler("LIST_EX_SECTION"), (bbs_cmd_handler)list_ex_section,
			"\"LIST_EX_SECTION\" maps to list_ex_section");
	check_ptr(get_cmd_handler("TOP10"), (bbs_cmd_handler)show_top10_menu, "\"TOP10\" maps to show_top10_menu");
	check_ptr(get_cmd_handler("LOCATE_ARTICLE"), (bbs_cmd_handler)locate_article,
			"\"LOCATE_ARTICLE\" maps to locate_article");
	check_ptr(get_cmd_handler("FAVOR_TOPIC"), (bbs_cmd_handler)favor_topic, "\"FAVOR_TOPIC\" maps to favor_topic");
	check_ptr(get_cmd_handler("LIST_USER"), (bbs_cmd_handler)list_user, "\"LIST_USER\" maps to list_user");
	check_ptr(get_cmd_handler("LIST_ONLINE_USER"), (bbs_cmd_handler)list_online_user,
			"\"LIST_ONLINE_USER\" maps to list_online_user");
	check_ptr(get_cmd_handler("EDIT_INTRO"), (bbs_cmd_handler)edit_intro, "\"EDIT_INTRO\" maps to edit_intro");
	check_ptr(get_cmd_handler("EDIT_SIGN"), (bbs_cmd_handler)edit_sign, "\"EDIT_SIGN\" maps to edit_sign");

	section_report(2);
}

static void test_unknown(void)
{
	printf("Testing #3 ...\n");

	// The trie is case-sensitive, so a lowercase variant of a real command has to miss
	check_ptr(get_cmd_handler("list_section"), NULL, "the lookup is case sensitive");
	check_ptr(get_cmd_handler("LIST-SECTION"), NULL, "\"LIST-SECTION\" is not registered");
	check_ptr(get_cmd_handler("LIST_SECTIO"), NULL, "a prefix of a registered command does not resolve");
	check_ptr(get_cmd_handler("LIST_SECTION!"), NULL, "a suffix breaks the lookup");
	check_ptr(get_cmd_handler(""), NULL, "the empty command does not resolve");
	check_ptr(get_cmd_handler("NOSUCHKEY"), NULL, "an unknown key does not resolve");

	section_report(3);
}

int main(void)
{
	if (log_begin("~test_bbs_cmd.log", "~test_bbs_cmd_error.log") < 0)
	{
		printf("Open log error\n");

		return -1;
	}

	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	if (trie_dict_init(TRIE_DICT_SHM_FILE, TRIE_NODE_PER_POOL) < 0)
	{
		printf("trie_dict_init(%s) error\n", TRIE_DICT_SHM_FILE);

		return -2;
	}

	test_load_state();
	test_lookup();
	test_unknown();

	unload_cmd();
	trie_dict_cleanup();

	if (failed_count > 0)
	{
		printf("Failed: %d of %d checks\n", failed_count, check_count);

		return -1;
	}

	printf("Done\n");

	log_end();
	unlink("~test_bbs_cmd.log");
	unlink("~test_bbs_cmd_error.log");
	return 0;
}
