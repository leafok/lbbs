/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_init
 *   - tester for the daemon configuration parser
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bbs.h"
#include "common.h"
#include "database.h"
#include "init.h"
#include "log.h"
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// init.c only reads and writes these globals; it never calls db_open(). Declaring the
// definitions here keeps mysql off the link line — the same trick test_article_cache.c
// uses for BBS_name and BBS_server.
char BBS_id[BBS_id_max_len + 1];
char BBS_name[BBS_name_max_len + 1];
char BBS_server[BBS_server_max_len + 1];
char BBS_address[BBS_address_max_len + 1];
in_port_t BBS_port[2];
int BBS_max_client;
int BBS_max_client_per_ip;
char BBS_start_dt[BBS_start_dt_max_len + 1];
int BBS_sys_id;

char DB_host[DB_host_max_len + 1];
char DB_username[DB_username_max_len + 1];
char DB_password[DB_password_max_len + 1];
char DB_database[DB_database_max_len + 1];
char DB_timezone[DB_timezone_max_len + 1];

static const char CONF_FILE[] = "~test_init.conf";

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

static void check_str(const char *actual, const char *expected, const char *what)
{
	check_count++;

	if (strcmp(actual, expected) != 0)
	{
		failed_count++;
		printf("FAILED: %s = [%s], expected [%s]\n", what, actual, expected);
	}
}

static void reset_globals(void)
{
	BBS_id[0] = '\0';
	BBS_name[0] = '\0';
	BBS_server[0] = '\0';
	BBS_address[0] = '\0';
	BBS_port[0] = 0;
	BBS_port[1] = 0;
	BBS_max_client = 0;
	BBS_max_client_per_ip = 0;
	BBS_start_dt[0] = '\0';
	BBS_sys_id = 0;
	DB_host[0] = '\0';
	DB_username[0] = '\0';
	DB_password[0] = '\0';
	DB_database[0] = '\0';
	DB_timezone[0] = '\0';
}

// Each test writes a fresh fixture and calls load_conf once against it
static int write_conf(const char *content)
{
	FILE *f = fopen(CONF_FILE, "w");

	if (f == NULL)
	{
		return -1;
	}

	fputs(content, f);
	fclose(f);

	return 0;
}

static void section_report(int n)
{
	printf("Completed testing #%d: %d of %d checks passed\n", n, check_count - failed_count, check_count);
}

static void test_missing_file(void)
{
	printf("Testing #1 ...\n");

	check_true(load_conf("~test_init_does_not_exist.conf") == -1, "load_conf() of a missing file returns -1");

	section_report(1);
}

static void test_happy_path(void)
{
	int ret;

	printf("Testing #2 ...\n");

	reset_globals();

	ret = write_conf(
		"bbs_id        MYBBS\n"
		"bbs_name      我的站台\n"
		"bbs_server    bbs.example.com\n"
		"bbs_address   10.0.0.1\n"
		"bbs_port      2323\n"
		"bbs_ssh_port  2223\n"
		"bbs_max_client 500\n"
		"bbs_max_client_per_ip 100\n"
		"bbs_sys_id    7\n"
		"db_host       db.internal\n"
		"db_username   bbsadmin\n"
		"db_password   s3cret\n"
		"db_database   bbs_db\n"
		"db_timezone   +08:00\n");
	check_int(ret, 0, "wrote the full happy-path fixture");

	check_int(load_conf(CONF_FILE), 0, "load_conf() accepts a valid file");

	check_str(BBS_id, "MYBBS", "bbs_id populated BBS_id");
	check_str(BBS_name, "我的站台", "bbs_name populated BBS_name (UTF-8 passes through)");
	check_str(BBS_server, "bbs.example.com", "bbs_server populated BBS_server");
	check_str(BBS_address, "10.0.0.1", "bbs_address populated BBS_address");
	check_int(BBS_port[0], 2323, "bbs_port populated BBS_port[0]");
	check_int(BBS_port[1], 2223, "bbs_ssh_port populated BBS_port[1]");
	check_int(BBS_max_client, 500, "bbs_max_client populated");
	check_int(BBS_max_client_per_ip, 100, "bbs_max_client_per_ip populated");
	check_int(BBS_sys_id, 7, "bbs_sys_id populated");
	check_str(DB_host, "db.internal", "db_host populated DB_host");
	check_str(DB_username, "bbsadmin", "db_username populated");
	check_str(DB_password, "s3cret", "db_password populated");
	check_str(DB_database, "bbs_db", "db_database populated");
	check_str(DB_timezone, "+08:00", "db_timezone populated");

	section_report(2);
}

static void test_comments_and_blanks(void)
{
	printf("Testing #3 ...\n");

	reset_globals();

	write_conf(
		"# leading comment line\n"
		"\n"
		"   \n"
		"\t\n"
		"bbs_id  AlphaSite\n"
		"# trailing comment\n"
		"bbs_name Beta\n");

	check_int(load_conf(CONF_FILE), 0, "load_conf() ignores comment and blank lines");
	check_str(BBS_id, "AlphaSite", "the value after a leading comment still parses");
	check_str(BBS_name, "Beta", "the value before a trailing comment still parses");

	section_report(3);
}

static void test_extra_value(void)
{
	printf("Testing #4 ...\n");

	reset_globals();

	// load_conf() only treats a second token as a comment if it starts with '#', and rejects
	// the whole line otherwise
	write_conf(
		"bbs_id One Two\n"
		"bbs_name Real # a trailing comment\n"
		"bbs_server Keep\n");

	check_int(load_conf(CONF_FILE), 0, "load_conf() skips bad lines and continues");
	check_str(BBS_id, "", "a line with an extra value is dropped");
	check_str(BBS_name, "Real", "the value before '#' is kept");
	check_str(BBS_server, "Keep", "the next valid line still parses");

	section_report(4);
}

static void test_empty_value(void)
{
	printf("Testing #5 ...\n");

	reset_globals();

	// strtok_r collapses to a NULL for the second token on a lone-key line
	write_conf(
		"bbs_id\n"
		"bbs_name NotEmpty\n");

	check_int(load_conf(CONF_FILE), 0, "load_conf() survives a value-less key");
	check_str(BBS_id, "", "a key with no value does not overwrite the global");
	check_str(BBS_name, "NotEmpty", "the next valid line still parses");

	section_report(5);
}

static void test_unknown_key(void)
{
	printf("Testing #6 ...\n");

	reset_globals();

	write_conf(
		"totally_unknown value\n"
		"bbs_id AfterUnknown\n");

	check_int(load_conf(CONF_FILE), 0, "load_conf() does not fail on an unknown key");
	check_str(BBS_id, "AfterUnknown", "the unknown key does not stop the next line");

	section_report(6);
}

static void test_case_insensitive(void)
{
	printf("Testing #7 ...\n");

	reset_globals();

	write_conf("BBS_ID Mixed\nBSB_NAME no\n");

	load_conf(CONF_FILE);
	check_str(BBS_id, "Mixed", "load_conf() matches keys case-insensitively");

	section_report(7);
}

static void test_client_limits_clamp(void)
{
	printf("Testing #8 ...\n");

	reset_globals();

	// Anything outside (0, MAX_CLIENT_LIMIT] resets to MAX_CLIENT_LIMIT
	write_conf("bbs_max_client 20000\n");
	load_conf(CONF_FILE);
	check_int(BBS_max_client, MAX_CLIENT_LIMIT, "bbs_max_client above the cap resets to the cap");

	reset_globals();
	BBS_max_client = 42;
	write_conf("bbs_max_client 0\n");
	load_conf(CONF_FILE);
	check_int(BBS_max_client, MAX_CLIENT_LIMIT, "bbs_max_client of zero resets to the cap");

	reset_globals();
	BBS_max_client = 42;
	write_conf("bbs_max_client -1\n");
	load_conf(CONF_FILE);
	check_int(BBS_max_client, MAX_CLIENT_LIMIT, "bbs_max_client below zero resets to the cap");

	reset_globals();
	write_conf("bbs_max_client_per_ip 99999\n");
	load_conf(CONF_FILE);
	check_int(BBS_max_client_per_ip, MAX_CLIENT_PER_IP_LIMIT, "bbs_max_client_per_ip above the cap resets");

	section_report(8);
}

static void test_sys_id_guard(void)
{
	printf("Testing #9 ...\n");

	reset_globals();
	BBS_sys_id = 3;

	// bbs_sys_id rejects v <= 0 by leaving the previous value in place
	write_conf("bbs_sys_id 0\n");
	load_conf(CONF_FILE);
	check_int(BBS_sys_id, 3, "bbs_sys_id of 0 keeps the previous value");

	write_conf("bbs_sys_id -5\n");
	load_conf(CONF_FILE);
	check_int(BBS_sys_id, 3, "bbs_sys_id of a negative keeps the previous value");

	write_conf("bbs_sys_id 9\n");
	load_conf(CONF_FILE);
	check_int(BBS_sys_id, 9, "bbs_sys_id of a positive replaces the value");

	section_report(9);
}

static void test_start_dt(void)
{
	printf("Testing #10 ...\n");

	reset_globals();

	// "%4s年%2s月%2s日" — right-justified field widths, so short month/day pad with a space
	write_conf("bbs_start_dt 2024-01-15\n");
	load_conf(CONF_FILE);
	check_str(BBS_start_dt, "2024年01月15日", "bbs_start_dt formats a full date");

	reset_globals();
	write_conf("bbs_start_dt 2024-1-5\n");
	load_conf(CONF_FILE);
	check_str(BBS_start_dt, "2024年 1月 5日", "single-digit month and day pad to two columns");

	reset_globals();
	BBS_start_dt[0] = '\0';
	// A date without dashes yields a single y, and m/d come back NULL
	write_conf("bbs_start_dt 20240115\n");
	load_conf(CONF_FILE);
	check_str(BBS_start_dt, "", "bbs_start_dt without dashes is rejected");

	section_report(10);
}

static void test_empty_file(void)
{
	printf("Testing #11 ...\n");

	reset_globals();

	write_conf("");
	check_int(load_conf(CONF_FILE), 0, "load_conf() of an empty file succeeds");
	check_str(BBS_id, "", "the empty file leaves BBS_id untouched");

	section_report(11);
}

int main(void)
{
	if (log_begin("~test_init.log", "~test_init_error.log") < 0)
	{
		printf("Open log error\n");

		return -1;
	}

	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	test_missing_file();
	test_happy_path();
	test_comments_and_blanks();
	test_extra_value();
	test_empty_value();
	test_unknown_key();
	test_case_insensitive();
	test_client_limits_clamp();
	test_sys_id_guard();
	test_start_dt();
	test_empty_file();

	remove(CONF_FILE);

	if (failed_count > 0)
	{
		printf("Failed: %d of %d checks\n", failed_count, check_count);

		return -1;
	}

	printf("Done\n");

	log_end();
	unlink("~test_init.log");
	unlink("~test_init_error.log");
	return 0;
}
