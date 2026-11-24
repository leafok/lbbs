/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_io
 *   - tester for io_buf_conv()
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "io.h"
#include "log.h"
#include <errno.h>
#include <iconv.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Only io_buf_conv() is exercised here. The rest of io.c is terminal plumbing
// (prints/outc/iflush/igetch) and needs a TTY to observe.
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

static void section_report(int n)
{
	printf("Completed testing #%d: %d of %d checks passed\n", n, check_count - failed_count, check_count);
}

// The buffer contract of io_buf_conv(): p_buf holds *p_buf_len bytes, the scan resumes
// at *p_buf_offset, and produced bytes land at p_conv[*p_conv_len .. conv_size). The
// helper runs one call and returns the exit code so a caller can inspect the mutated
// length and offset fields.
static void test_null(void)
{
	char in[8] = "abc";
	char out[8];
	int in_len = 3, in_off = 0, out_len = 0;
	iconv_t cd;
	int ret;

	printf("Testing #1 ...\n");

	cd = iconv_open("UTF-8", "UTF-8");
	check_true(cd != (iconv_t)(-1), "iconv_open(UTF-8, UTF-8) available on this host");

	if (cd == (iconv_t)(-1))
	{
		section_report(1);

		return;
	}

	ret = io_buf_conv(cd, NULL, &in_len, &in_off, out, sizeof(out), &out_len);
	check_int(ret, -1, "io_buf_conv() rejects a NULL p_buf");
	ret = io_buf_conv(cd, in, NULL, &in_off, out, sizeof(out), &out_len);
	check_int(ret, -1, "io_buf_conv() rejects a NULL p_buf_len");
	ret = io_buf_conv(cd, in, &in_len, NULL, out, sizeof(out), &out_len);
	check_int(ret, -1, "io_buf_conv() rejects a NULL p_buf_offset");
	ret = io_buf_conv(cd, in, &in_len, &in_off, NULL, sizeof(out), &out_len);
	check_int(ret, -1, "io_buf_conv() rejects a NULL p_conv");
	ret = io_buf_conv(cd, in, &in_len, &in_off, out, sizeof(out), NULL);
	check_int(ret, -1, "io_buf_conv() rejects a NULL p_conv_len");
	ret = io_buf_conv((iconv_t)(-1), in, &in_len, &in_off, out, sizeof(out), &out_len);
	check_int(ret, -1, "io_buf_conv() rejects an unusable iconv handle");

	iconv_close(cd);

	section_report(1);
}

static void test_ascii_identity(void)
{
	char in[16] = "hello";
	char out[16];
	int in_len = 5, in_off = 0, out_len = 0;
	iconv_t cd;
	int ret;

	printf("Testing #2 ...\n");

	cd = iconv_open("UTF-8", "UTF-8");
	if (cd == (iconv_t)(-1))
	{
		printf("SKIP: no UTF-8 iconv\n");
		section_report(2);

		return;
	}

	memset(out, 0, sizeof(out));
	ret = io_buf_conv(cd, in, &in_len, &in_off, out, sizeof(out), &out_len);
	check_int(ret, 0, "UTF-8 to UTF-8 identity conversion");
	check_str(out, "hello", "the ASCII payload passes through unchanged");
	check_int(out_len, 5, "five bytes produced");
	// The function zero-folds p_buf_len and p_buf_offset after consuming the whole input, so a
	// caller can immediately refill the buffer without tracking state itself
	check_int(in_off, 0, "input offset is reset after full consumption");
	check_int(in_len, 0, "input length is reset after full consumption");

	iconv_close(cd);

	section_report(2);
}

static void test_wide_utf8(void)
{
	// 测试 = 0xE6 0xB5 0x8B 0xE8 0xAF 0x95
	char in[16] = "\xe6\xb5\x8b\xe8\xaf\x95";
	char out[16];
	int in_len = 6, in_off = 0, out_len = 0;
	iconv_t cd;
	int ret;

	printf("Testing #3 ...\n");

	cd = iconv_open("GBK", "UTF-8");
	if (cd == (iconv_t)(-1))
	{
		printf("SKIP: no UTF-8 to GBK iconv\n");
		section_report(3);

		return;
	}

	memset(out, 0, sizeof(out));
	ret = io_buf_conv(cd, in, &in_len, &in_off, out, sizeof(out), &out_len);
	check_int(ret, 0, "UTF-8 to GBK conversion succeeded");
	check_true(out_len >= 2, "each CJK codepoint takes at least two GBK bytes");
	check_int(in_off, 0, "the full UTF-8 sequence is consumed and the offset resets");
	check_int(in_len, 0, "the full UTF-8 sequence is consumed and the length resets");

	iconv_close(cd);

	section_report(3);
}

static void test_incomplete_tail(void)
{
	// Only the first byte of a 3-byte UTF-8 sequence at the tail
	char in[16] = "\xe6\xb5\x8b\xe8";
	char out[16];
	int in_len = 4, in_off = 0, out_len = 0;
	iconv_t cd;
	int ret;

	printf("Testing #4 ...\n");

	cd = iconv_open("UTF-8", "UTF-8");
	if (cd == (iconv_t)(-1))
	{
		printf("SKIP: no UTF-8 iconv\n");
		section_report(4);

		return;
	}

	memset(out, 0, sizeof(out));
	ret = io_buf_conv(cd, in, &in_len, &in_off, out, sizeof(out), &out_len);
	check_int(ret, 0, "io_buf_conv() returns 0 on an incomplete tail");
	check_true(in_len < 4, "the incomplete tail shrinks p_buf_len so a caller can compact");
	check_true(in_len > 0, "at least the truncated head byte is retained");
	check_int(in_off, 0, "the offset rewinds to zero after an incomplete tail");
	check_int(out_len, 3, "only the completed codepoint was emitted");

	iconv_close(cd);

	section_report(4);
}

static void test_control_pair(void)
{
	// KEY_CONTROL is 0xff, and the routine copies it and the next byte through unconverted
	char in[16];
	char out[16];
	int in_len, in_off = 0, out_len = 0;
	iconv_t cd;
	int ret;

	printf("Testing #5 ...\n");

	cd = iconv_open("UTF-8", "UTF-8");
	if (cd == (iconv_t)(-1))
	{
		printf("SKIP: no UTF-8 iconv\n");
		section_report(5);

		return;
	}

	in[0] = (char)KEY_CONTROL;
	in[1] = 'A';
	in[2] = 'b';
	in_len = 3;
	memset(out, 0, sizeof(out));

	ret = io_buf_conv(cd, in, &in_len, &in_off, out, sizeof(out), &out_len);
	check_int(ret, 0, "a control pair followed by ordinary ASCII converts cleanly");
	check_true(out[0] == (char)KEY_CONTROL, "the KEY_CONTROL byte is passed through");
	check_true(out[1] == 'A', "the byte after KEY_CONTROL is passed through unconverted");
	check_true(out[2] == 'b', "the third byte returns to normal conversion");
	check_int(out_len, 3, "all three bytes produced");

	iconv_close(cd);

	section_report(5);
}

static void test_conv_init_cleanup(void)
{
	printf("Testing #6 ...\n");

	// io_conv_init() installs the stdio-side handles; both paths must be safe
	check_int(io_conv_init(NULL), -1, "io_conv_init(NULL) rejects a missing charset");
	check_int(io_conv_init("UTF-8"), 0, "io_conv_init(UTF-8) succeeds");
	check_int(io_conv_cleanup(), 0, "io_conv_cleanup() succeeds after a fresh install");
	check_int(io_conv_cleanup(), 0, "io_conv_cleanup() is idempotent");

	section_report(6);
}

int main(void)
{
	if (log_begin("~test_io.log", "~test_io_error.log") < 0)
	{
		printf("Open log error\n");

		return -1;
	}

	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	// A few iconv handles are opened and closed per test; keep them off the shared stdio pair
	test_null();
	test_ascii_identity();
	test_wide_utf8();
	test_incomplete_tail();
	test_control_pair();
	test_conv_init_cleanup();

	if (failed_count > 0)
	{
		printf("Failed: %d of %d checks\n", failed_count, check_count);

		return -1;
	}

	printf("Done\n");

	log_end();
	unlink("~test_io.log");
	unlink("~test_io_error.log");
	return 0;
}
