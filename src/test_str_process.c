/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_str_process
 *   - tester for common string processing features with UTF-8 support
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "log.h"
#include "str_process.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

// split_data_lines() writes p_line_offsets[0 .. count-1] and p_line_widths[0 .. count-2]
// and returns at most count-1. Both arrays here carry one canary slot on each side, so a
// caller that sized them from a different constant is caught instead of corrupting the stack.
#define SPLIT_CANARY ((long)0x5a5a5a5aL)

static long split_offsets[64];
static int split_widths[64];

static void test_split_lines(const char *tag, const char *text, int max_display_len, long count,
							  int skip_ctrl_seq, int with_widths, long expected_lines,
							  const long *expected_offsets, const int *expected_widths)
{
	long *p_offsets = &split_offsets[1];
	int *p_widths = &split_widths[1];
	long lines;
	long text_len = (long)strlen(text);
	long i;
	char msg[256];

	for (i = 0; i < (long)(sizeof(split_offsets) / sizeof(split_offsets[0])); i++)
	{
		split_offsets[i] = SPLIT_CANARY;
	}
	for (i = 0; i < (long)(sizeof(split_widths) / sizeof(split_widths[0])); i++)
	{
		split_widths[i] = 0x5a5a;
	}

	// count == 0 would write past a zero-length array, which no caller may pass
	snprintf(msg, sizeof(msg), "%s: return", tag);
	lines = split_data_lines(text, max_display_len, p_offsets, count, skip_ctrl_seq,
							 with_widths ? p_widths : NULL);
	check_int(lines, expected_lines, msg);

	snprintf(msg, sizeof(msg), "%s: never returns more than count-1", tag);
	check_true(lines <= count - 1, msg);

	snprintf(msg, sizeof(msg), "%s: offsets stay within the array", tag);
	check_true(split_offsets[0] == SPLIT_CANARY && split_offsets[count + 1] == SPLIT_CANARY, msg);

	snprintf(msg, sizeof(msg), "%s: widths stay within the array", tag);
	check_true(split_widths[0] == 0x5a5a && split_widths[count] == 0x5a5a &&
				   split_widths[count + 1] == 0x5a5a, msg);

	snprintf(msg, sizeof(msg), "%s: first offset", tag);
	check_int(p_offsets[0], 0, msg);

	for (i = 1; i <= lines; i++)
	{
		snprintf(msg, sizeof(msg), "%s: offset[%ld]", tag, i);
		check_true(p_offsets[i] >= p_offsets[i - 1] && p_offsets[i] <= text_len, msg);

		if (expected_offsets != NULL)
		{
			snprintf(msg, sizeof(msg), "%s: offset[%ld] value", tag, i);
			check_int(p_offsets[i], expected_offsets[i], msg);
		}

		// what split_line reports as the width must be what str_length() sees on the slice
		if (with_widths)
		{
			char slice[256];
			long slice_len = p_offsets[i] - p_offsets[i - 1];

			if (slice_len < (long)sizeof(slice) - 1)
			{
				memcpy(slice, text + p_offsets[i - 1], (size_t)slice_len);
				slice[slice_len] = '\0';

				snprintf(msg, sizeof(msg), "%s: width[%ld] of the same slice", tag, i - 1);
				check_int(p_widths[i - 1], str_length(slice, skip_ctrl_seq), msg);
			}

			if (expected_widths != NULL)
			{
				snprintf(msg, sizeof(msg), "%s: width[%ld] value", tag, i - 1);
				check_int(p_widths[i - 1], expected_widths[i - 1], msg);
			}
		}
	}

	// unless the caller ran out of room, every byte of the text is accounted for
	if (lines < count - 1)
	{
		snprintf(msg, sizeof(msg), "%s: covers the whole text", tag);
		check_int(p_offsets[lines], text_len, msg);
	}
}

static void test_split_capacity(void)
{
	// a plain line, no wrapping
	test_split_lines("one short line", "hello", 80, 4, 1, 1, 1,
					 (const long[]){0, 5}, (const int[]){5});

	// an empty buffer still counts as one (empty) line
	test_split_lines("empty buffer", "", 80, 4, 1, 1, 1,
					 (const long[]){0, 0}, (const int[]){0});

	// count = 1 leaves no room for a second offset, so nothing is split
	test_split_lines("count 1", "hello", 80, 1, 1, 1, 0, NULL, NULL);
	test_split_lines("count 1, empty", "", 80, 1, 1, 0, 0, NULL, NULL);

	// a trailing end of line costs the caller one more slot than the visible lines
	test_split_lines("three lines", "a\nb\nc\n", 80, 6, 1, 1, 4,
					 (const long[]){0, 2, 4, 6, 6}, (const int[]){2, 2, 2, 0});

	// the same text with a count that is exactly one line short of the full split
	test_split_lines("three lines, capped", "a\nb\nc\n", 80, 4, 1, 1, 3,
					 (const long[]){0, 2, 4, 6}, (const int[]){2, 2, 2});

	// wrapping every max_display_len columns
	test_split_lines("wrapped", "aaaaaaaaaa", 4, 8, 1, 1, 3,
					 (const long[]){0, 4, 8, 10}, (const int[]){4, 4, 2});

	// wrapping is capped by the array, not by the text
	test_split_lines("wrapped, capped", "aaaaaaaaaa", 4, 3, 1, 1, 2,
					 (const long[]){0, 4, 8}, (const int[]){4, 4});

	// no widths array to maintain
	test_split_lines("without widths", "hello\nworld", 80, 4, 1, 0, 2,
					 (const long[]){0, 6, 11}, NULL);

	// a line break landing right on the wrap column leaves the newline to its own line
	test_split_lines("break at wrap column", "ab\ncd", 2, 8, 1, 1, 3,
					 (const long[]){0, 2, 3, 5}, (const int[]){2, 1, 2});
}

static void test_split_utf8(void)
{
	// three double-wide characters into four columns: two, then one
	test_split_lines("utf-8 wrap", "中中中", 4, 8, 1, 1, 2,
					 (const long[]){0, 6, 9}, (const int[]){4, 2});

	// control sequences cost no display width but still take bytes
	test_split_lines("utf-8 with color", "\033[1;31m中中\033[m", 80, 4, 1, 1, 1,
					 (const long[]){0, 16}, (const int[]){4});

	test_split_lines("utf-8 mixed", "A中\033[0mB", 4, 4, 1, 1, 1,
					 (const long[]){0, 9}, (const int[]){4});
}

static void test_str_length(void)
{
	UTF8_fixed_width = 1;

	check_int(str_length("", 1), 0, "str_length: empty");
	check_int(str_length("hello", 1), 5, "str_length: ascii");
	check_int(str_length("hello\n", 1), 6, "str_length: newline counts one");
	check_int(str_length("a\rb\7c", 1), 3, "str_length: CR and BEL dropped");

	// ANSI CSI sequences
	check_int(str_length("\033[1;31mred\033[m", 1), 3, "str_length: color skipped");
	check_int(str_length("\033[1;31mred\033[m", 0), 13, "str_length: color counted");
	check_int(str_length("\033[2J\033[10Hhome", 1), 4, "str_length: other CSI skipped");
	check_int(str_length("\033[", 1), 0, "str_length: unterminated CSI");
	check_int(str_length("\033A", 1), 2, "str_length: ESC not followed by [");

	// UTF-8, fixed width mode
	check_int(str_length("中", 1), 2, "str_length: cjk");
	check_int(str_length("中文测试", 1), 8, "str_length: cjk run");
	check_int(str_length("A中B", 1), 4, "str_length: cjk between ascii");
	check_int(str_length("é", 1), 2, "str_length: latin-1 counts two when fixed");
	check_int(str_length("😀", 1), 2, "str_length: 4-byte sequence");

	// malformed input must be measured, not walked off the end of the buffer
	check_int(str_length("中", 1), 2, "str_length: truncated sequence");
	check_int(str_length("\x80\x80", 1), 4, "str_length: stray continuation bytes");
	check_int(str_length("\xc0\x80", 1), 2, "str_length: overlong sequence");
	check_int(str_length("\xff\xff", 1), 2, "str_length: invalid head bytes");

	// variable width mode asks the locale, so the same text can measure differently
	UTF8_fixed_width = 0;
	check_int(str_length("hello", 1), 5, "str_length: ascii, variable");
	check_int(str_length("中文", 1), 4, "str_length: cjk, variable");
	UTF8_fixed_width = 1;
}

static void check_split_one(const char *tag, const char *text, int max_display_len, int skip_ctrl_seq,
							int expected_bytes, int expected_eol, int expected_width)
{
	int eol = -1;
	int width = -1;
	int bytes;
	char msg[128];

	bytes = split_line(text, max_display_len, &eol, &width, skip_ctrl_seq);

	snprintf(msg, sizeof(msg), "%s: bytes", tag);
	check_int(bytes, expected_bytes, msg);
	snprintf(msg, sizeof(msg), "%s: end of line", tag);
	check_int(eol, expected_eol, msg);
	snprintf(msg, sizeof(msg), "%s: display width", tag);
	check_int(width, expected_width, msg);
}

static void test_split_line(void)
{
	UTF8_fixed_width = 1;

	check_split_one("short line", "hello", 80, 1, 5, 0, 5);
	check_split_one("exact fit", "hello", 5, 1, 5, 0, 5);
	check_split_one("wrapped", "hello", 3, 1, 3, 0, 3);
	check_split_one("empty", "", 80, 1, 0, 0, 0);
	check_split_one("zero width", "hello", 0, 1, 0, 0, 0);

	// the newline is consumed and accounted for as one column
	check_split_one("line feed", "ab\ncd", 80, 1, 3, 1, 3);
	check_split_one("line feed only", "\n", 80, 1, 1, 1, 1);
	check_split_one("line feed after full width", "ab\ncd", 2, 1, 2, 0, 2);

	// CR and BEL are dropped without costing a column or a byte of the slice
	check_split_one("control chars", "a\rb\7c", 80, 1, 5, 0, 3);

	check_split_one("color", "\033[1;31mred\033[m", 80, 1, 13, 0, 3);
	check_split_one("color kept", "\033[1;31mred\033[m", 80, 0, 13, 0, 13);

	// a wide character is never cut in half, even when it does not fit
	check_split_one("cjk at the edge", "中中", 3, 1, 3, 0, 2);
	check_split_one("cjk exact", "中中", 4, 1, 6, 0, 4);
	check_split_one("cjk and color", "\033[1;33m中\033[mx", 4, 1, 14, 0, 3);
}

static void check_str_filter(const char *tag, const char *text, int skip_ctrl_seq, int expected_len,
							 const char *expected_text)
{
	char buf[128];
	int len;
	char msg[128];

	memcpy(buf, text, strlen(text) + 1);

	len = str_filter(buf, skip_ctrl_seq);

	snprintf(msg, sizeof(msg), "%s: length", tag);
	check_int(len, expected_len, msg);
	snprintf(msg, sizeof(msg), "%s: text", tag);
	check_true(strcmp(buf, expected_text) == 0, msg);

	// the tail beyond the returned length must be cut off, not left dangling
	snprintf(msg, sizeof(msg), "%s: terminated", tag);
	check_true(buf[len] == '\0', msg);
}

static void test_str_filter(void)
{
	char buf[128];
	int len;

	check_str_filter("str_filter: plain", "hello", 1, 5, "hello");
	check_str_filter("str_filter: control chars", "a\rb\7c", 1, 3, "abc");
	check_str_filter("str_filter: color removed", "\033[1;31mred\033[m", 1, 3, "red");
	check_str_filter("str_filter: color kept", "\033[1;31mred\033[m", 0, 13, "\033[1;31mred\033[m");
	check_str_filter("str_filter: empty", "", 1, 0, "");
	check_str_filter("str_filter: mixed", "a\rb\033[0mc\7d", 1, 4, "abcd");

	len = str_length("\033[1;31mred\033[m", 0);
	check_int(len, 13, "str_length: color kept counts every byte");

	// filtering changes nothing about what is left, so running it twice is stable
	memcpy(buf, "a\rb\033[0mc\7d", 11);
	len = str_filter(buf, 1);
	check_int(str_filter(buf, 1), len, "str_filter: idempotent");
	check_int(str_length(buf, 1), len, "str_filter: filtered text measures its own length");
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	if (setlocale(LC_ALL, "en_US.UTF-8") == NULL)
	{
		printf("WARNING: setlocale(LC_ALL, en_US.UTF-8) failed, wide character tests may differ\n");
	}

	if (log_begin("../log/test_str_process.log", "../log/test_str_process_error.log") < 0)
	{
		printf("Open log error\n");
		return -1;
	}

	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	printf("Testing #1: split_data_lines capacity ...\n");
	test_split_capacity();
	printf("Completed testing #1: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #2: split_data_lines with UTF-8 ...\n");
	test_split_utf8();
	printf("Completed testing #2: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #3: str_length ...\n");
	test_str_length();
	printf("Completed testing #3: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #4: split_line ...\n");
	test_split_line();
	printf("Completed testing #4: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #5: str_filter ...\n");
	test_str_filter();
	printf("Completed testing #5: %d of %d checks passed\n", check_count - failed_count, check_count);

	log_end();

	if (failed_count > 0)
	{
		printf("Failed: %d of %d checks\n", failed_count, check_count);

		return -1;
	}

	printf("Done\n");

	return 0;
}
