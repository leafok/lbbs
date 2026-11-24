/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_log
 *   - tester for the file logger
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "log.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int check_count = 0;
static int failed_count = 0;

static char common_path[] = "~test_log.common.log";
static char error_path[] = "~test_log.error.log";
static char missing_common[] = "~test_log.absent/common.log";
static char missing_error[] = "~test_log.absent/error.log";
static char sub_dir[] = "~test_log.sub";
static char sub_common[] = "~test_log.sub/common.log";
static char sub_error[] = "~test_log.sub/error.log";
static char rot_path[] = "~test_log.rotated.log";
static char redir_common[] = "~test_log.redir-common.txt";
static char redir_error[] = "~test_log.redir-error.txt";
static char err_cap[] = "~test_log.stderr.txt";
static char text[1 << 16];
static int err_saved = -1;

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

static long file_size(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
	{
		return -1;
	}

	return (long)st.st_size;
}

// Slurps the whole log file so assertions can strstr across record boundaries
static long slurp(const char *path)
{
	FILE *fp = fopen(path, "rb");
	size_t n;

	if (fp == NULL)
	{
		text[0] = '\0';
		return -1;
	}

	n = fread(text, 1, sizeof(text) - 1, fp);
	text[n] = '\0';
	fclose(fp);

	return (long)n;
}

// The logger falls back to stderr until log_begin() opens its files, so the
// capture has to swap the descriptor underneath the stderr stream
static void stderr_capture_start(void)
{
	unlink(err_cap);
	err_saved = dup(STDERR_FILENO);

	int cap = open(err_cap, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (cap < 0)
	{
		return;
	}

	fflush(stderr);
	dup2(cap, STDERR_FILENO);
	close(cap);
}

static void stderr_capture_stop(void)
{
	int devnull = open("/dev/null", O_WRONLY);

	fflush(stderr);

	if (devnull >= 0)
	{
		dup2(devnull, STDERR_FILENO);
		close(devnull);
		fflush(stderr);
	}

	if (err_saved >= 0)
	{
		dup2(err_saved, STDERR_FILENO);
		close(err_saved);
		err_saved = -1;
	}
}

static void test_fallback_before_begin(void)
{
	printf("Testing #1 ...\n");

	stderr_capture_start();
	check_true(err_saved >= 0, "dup(STDERR_FILENO) succeeds");

	// log.c:114-117 substitutes stderr so an early log call cannot take the process down
	const char *msg = "written before log_begin";
	int rv = log_printf(LOG_LEVEL_COMMON, "pre.c", 1, "%s", msg);

	check_true(rv > 0, "log_printf() returns a positive byte count before log_begin()");
	check_true(rv >= (long)strlen(msg) + 1, "the fallback return value covers the message plus a newline");

	stderr_capture_stop();

	check_true(slurp(err_cap) > 0, "the stderr capture file received bytes");
	check_true(strstr(text, "] [INFO] written before log_begin") != NULL, "the fallback record keeps the INFO head");

	section_report(1);
}

static void test_log_begin_failures(void)
{
	printf("Testing #2 ...\n");

	stderr_capture_start();

	check_int(log_begin(missing_common, error_path), -1, "log_begin() reports -1 when the common log cannot be opened");
	check_true(file_size(error_path) == -1, "log_begin() skips the error log when the common log already failed");

	// The second handle failing must roll the first one back, not leak it
	check_int(log_begin(common_path, missing_error), -2, "log_begin() reports -2 when only the error log cannot be opened");
	check_int(file_size(common_path), 0, "log_begin() still creates the common log before the error log fails");

	stderr_capture_stop();

	slurp(err_cap);
	check_true(strstr(text, "~test_log.absent/common.log") != NULL, "log_begin() names the failing common path on stderr");
	check_true(strstr(text, "~test_log.absent/error.log") != NULL, "log_begin() names the failing error path on stderr");
	check_true(strstr(text, "No such file or directory") != NULL, "log_begin() reports errno as text");

	stderr_capture_start();
	int rv = log_printf(LOG_LEVEL_COMMON, "roll.c", 2, "common handle was rolled back");
	stderr_capture_stop();

	check_true(rv > 0, "logging still succeeds after the rolled-back log_begin()");
	slurp(err_cap);
	check_true(strstr(text, "common handle was rolled back") != NULL,
	           "the failed log_begin() cleared the common handle instead of leaking it");

	section_report(2);
}

static void test_log_begin_ok(void)
{
	printf("Testing #3 ...\n");

	check_int(log_begin(common_path, error_path), 0, "log_begin() succeeds on two writable paths");
	check_int(slurp(common_path), 0, "the common log starts out empty");
	check_int(slurp(error_path), 0, "the error log starts out empty");

	section_report(3);
}

static void test_levels_and_heads(void)
{
	printf("Testing #4 ...\n");

	time_t now = time(NULL);
	struct tm utc;
	char today[16];

	gmtime_r(&now, &utc);
	strftime(today, sizeof(today), "%Y-%m-%d", &utc);

	long before = file_size(common_path);

	int rv = log_printf(LOG_LEVEL_COMMON, "loc.c", 11, "hello %s %d", "world", 7);
	check_true(rv > 0, "a COMMON record reports the bytes written");
	check_int(file_size(common_path) - before, rv, "the return value equals the bytes appended to the common log");
	check_int(file_size(error_path), 0, "a COMMON record leaves the error log untouched");

	slurp(common_path);
	check_true(text[0] == '[', "the record opens with a bracketed timestamp");
	check_true(strncmp(text + 1, today, 10) == 0, "the timestamp is the UTC date");
	check_true(strstr(text, "[INFO] hello world 7") != NULL, "the COMMON head and the formatted message are on one line");
	check_true(strstr(text, "loc.c") == NULL, "a COMMON record carries no source location");
	check_true(strstr(text, "] [INFO] ") != NULL, "the INFO tag follows the pid field");
	check_int(strchr(text, '\n') - text, rv - 1, "COMMON records are newline terminated");

	before = file_size(error_path);
	rv = log_printf(LOG_LEVEL_ERROR, "loc.c", 11, "boom");
	check_true(rv > 0, "an ERROR record reports the bytes written");
	check_int(file_size(error_path) - before, rv, "the return value equals the bytes appended to the error log");

	slurp(error_path);
	check_true(strstr(text, "[ERROR] [loc.c:11] boom") != NULL, "the ERROR head carries the source location");
	check_true(strstr(text, "[INFO]") == NULL, "the error log holds no INFO record");

	// log.c:113 routes everything but COMMON to the error handle, DEBUG included
	before = file_size(error_path);
	rv = log_printf(LOG_LEVEL_DEBUG, "loc.c", 11, "dbg");
	check_true(rv > 0, "a DEBUG record reports the bytes written");
	check_int(file_size(error_path) - before, rv, "a DEBUG record is appended to the error log");

	slurp(error_path);
	check_true(strstr(text, "[DEBUG] [loc.c:11] dbg") != NULL, "the DEBUG head carries the source location too");
	check_true(text[0] == '[', "the error log begins with a bracketed timestamp");

	section_report(4);
}

static void test_append_order(void)
{
	printf("Testing #5 ...\n");

	log_printf(LOG_LEVEL_COMMON, "ord.c", 1, "first");
	log_printf(LOG_LEVEL_COMMON, "ord.c", 2, "second");
	log_printf(LOG_LEVEL_COMMON, "ord.c", 3, "third");

	slurp(common_path);

	const char *p1 = strstr(text, "] [INFO] first");
	const char *p2 = strstr(text, "] [INFO] second");
	const char *p3 = strstr(text, "] [INFO] third");

	check_true(p1 != NULL, "the first record is present");
	check_true(p2 != NULL, "the second record is present");
	check_true(p3 != NULL, "the third record is present");
	check_true(p1 < p2 && p2 < p3, "records are appended in call order");

	section_report(5);
}

static void test_truncation(void)
{
	printf("Testing #6 ...\n");

	char big[1024];
	memset(big, 'x', sizeof(big));
	big[1000] = '\0';

	long before = file_size(error_path);

	// vsnprintf() clips at the buffer edge, so the writer reports -2 and pads a newline
	int rv = log_printf(LOG_LEVEL_ERROR, "big.c", 1, "%s", big);

	check_int(rv, -2, "an over-long message reports truncation");
	check_int(file_size(error_path) - before, LINE_BUFFER_LEN - 1, "a truncated record fills the buffer exactly");

	check_true(slurp(error_path) > 0, "the truncated record was written");
	size_t n = strlen(text);
	check_true(text[n - 1] == '\n', "a truncated record still ends with a newline");
	check_true(text[n - 2] == 'x', "the truncated body is kept up to the buffer edge");

	section_report(6);
}

static void test_log_end_and_reopen(void)
{
	printf("Testing #7 ...\n");

	long before_common = file_size(common_path);
	long before_error = file_size(error_path);

	log_end();

	stderr_capture_start();
	int rv = log_printf(LOG_LEVEL_COMMON, "end.c", 1, "after log_end");
	stderr_capture_stop();

	check_true(rv > 0, "logging after log_end() still returns a byte count");
	check_int(file_size(common_path), before_common, "log_end() stops writes to the common log");
	check_int(file_size(error_path), before_error, "log_end() leaves the existing logs intact");

	slurp(err_cap);
	check_true(strstr(text, "after log_end") != NULL, "the record after log_end() lands on stderr");

	// Mode "a" rather than "w": restarting must not discard history
	check_int(log_begin(common_path, error_path), 0, "log_begin() reopens the same paths");
	check_int(file_size(common_path), before_common, "reopening does not truncate the common log");
	check_int(file_size(error_path), before_error, "reopening does not truncate the error log");

	log_printf(LOG_LEVEL_COMMON, "end.c", 2, "reopened");
	check_true(file_size(common_path) > before_common, "the reopened common log accepts records again");

	slurp(common_path);
	check_true(strstr(text, "] [INFO] first") != NULL, "history survives the close and reopen");
	check_true(strstr(text, "] [INFO] reopened") != NULL, "the record written after the reopen is present");

	section_report(7);
}

static void test_restart_rotation(void)
{
	printf("Testing #8 ...\n");

	check_true(rename(common_path, rot_path) == 0, "the common log is moved aside as logrotate would");

	// Both handles still point at the old inode until log_restart() reopens them
	log_printf(LOG_LEVEL_COMMON, "rot.c", 1, "still the old handle");

	check_int(log_restart(), 0, "log_restart() succeeds on two still-writable paths");

	check_true(log_printf(LOG_LEVEL_COMMON, "rot.c", 2, "after restart") > 0, "the common log accepts records again after the restart");
	check_true(file_size(common_path) > 0, "log_restart() recreates the common log at the original path");

	slurp(rot_path);
	check_true(strstr(text, "still the old handle") != NULL, "the pre-restart record stays in the rotated file");
	check_true(strstr(text, "after restart") == NULL, "the post-restart record does not follow the rotated file");

	slurp(common_path);
	check_true(strstr(text, "after restart") != NULL, "the post-restart record lands in the fresh file");
	check_true(strstr(text, "still the old handle") == NULL, "the fresh file does not inherit the rotated contents");

	check_int(log_restart(), 0, "log_restart() is repeatable");

	section_report(8);
}

static void test_redir(void)
{
	printf("Testing #9 ...\n");

	int fd_common = open(redir_common, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	check_true(fd_common >= 0, "open() of the common redirection target succeeds");

	long before = file_size(common_path);

	// dup2() yields the descriptor it wrote onto, i.e. the logger's own, so the
	// caller may only close the fd it opened
	int dest = log_common_redir(fd_common);
	check_true(dest >= 0, "log_common_redir() succeeds and returns a descriptor");
	check_true(dest != fd_common, "log_common_redir() returns the logger's descriptor, not the caller's");

	log_printf(LOG_LEVEL_COMMON, "redir.c", 1, "into the redirected stream");

	check_int(file_size(common_path), before, "a redirected common stream stops writing to its original path");

	slurp(redir_common);
	check_true(strstr(text, "into the redirected stream") != NULL, "the redirected record lands in the new target");

	close(fd_common);

	int fd_error = open(redir_error, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	check_true(fd_error >= 0, "open() of the error redirection target succeeds");

	before = file_size(error_path);
	dest = log_error_redir(fd_error);
	check_true(dest >= 0, "log_error_redir() succeeds and returns a descriptor");
	check_true(dest != fd_error, "log_error_redir() returns the logger's descriptor, not the caller's");

	log_printf(LOG_LEVEL_ERROR, "redir.c", 2, "error redirected");
	check_int(file_size(error_path), before, "a redirected error stream stops writing to its original path");

	slurp(redir_error);
	check_true(strstr(text, "[ERROR] [redir.c:2] error redirected") != NULL, "the redirected error record keeps its head");

	close(fd_error);

	// log.c:176/186 skip the reopen for a redirected stream so the descriptor swap survives rotation
	check_int(log_restart(), 0, "log_restart() succeeds while both streams are redirected");

	log_printf(LOG_LEVEL_COMMON, "redir.c", 3, "still redirected");

	slurp(redir_common);
	check_true(strstr(text, "still redirected") != NULL, "log_restart() preserves the common redirection");

	slurp(common_path);
	check_true(strstr(text, "still redirected") == NULL, "the original common path stays frozen while redirected");

	// log_begin() resets both flags, so the paths take over again
	check_int(log_begin(common_path, error_path), 0, "log_begin() clears the redirection state");

	before = file_size(common_path);
	log_printf(LOG_LEVEL_COMMON, "redir.c", 4, "back to the file");
	check_true(file_size(common_path) > before, "log_begin() sends the common stream back to its path");

	slurp(redir_common);
	check_true(strstr(text, "back to the file") == NULL, "the old redirection target stops receiving records");

	section_report(9);
}

static void test_restart_failure(void)
{
	printf("Testing #10 ...\n");

	check_true(mkdir(sub_dir, 0755) == 0, "the temporary log directory is created");
	check_int(log_begin(sub_common, sub_error), 0, "log_begin() opens logs inside the temporary directory");

	check_true(unlink(sub_common) == 0, "the common log is removed");
	check_true(unlink(sub_error) == 0, "the error log is removed");
	check_true(rmdir(sub_dir) == 0, "the temporary directory is removed");

	// The common path is checked first, so -1 names the stream that failed
	check_int(log_restart(), -1, "log_restart() reports -1 when the common path became unreachable");

	section_report(10);
}

int main(void)
{
	unlink(common_path);
	unlink(error_path);
	unlink(rot_path);
	unlink(redir_common);
	unlink(redir_error);
	unlink(err_cap);

	test_fallback_before_begin();
	test_log_begin_failures();
	test_log_begin_ok();
	test_levels_and_heads();
	test_append_order();
	test_truncation();
	test_log_end_and_reopen();
	test_restart_rotation();
	test_redir();
	test_restart_failure();

	log_end();

	unlink(common_path);
	unlink(error_path);
	unlink(rot_path);
	unlink(redir_common);
	unlink(redir_error);
	unlink(err_cap);
	unlink(sub_common);
	unlink(sub_error);
	rmdir(sub_dir);

	printf("Done\n");

	return failed_count == 0 ? 0 : 1;
}
