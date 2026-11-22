/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_editor
 *   - tester for the editor data model and the full-screen editor loop
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bbs.h"
#include "common.h"
#include "editor.h"
#include "io.h"
#include "log.h"
#include "screen.h"
#include "str_process.h"
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

static void check_str(const char *actual, const char *expected, const char *what)
{
	check_count++;

	if (strcmp(actual, expected) != 0)
	{
		failed_count++;

		// The documents here run to a thousand characters, so a mismatch shows its size and
		// its head instead of filling the log with the whole text
		printf("FAILED: %s = [%.40s] (%zu bytes), expected [%.40s] (%zu bytes)\n",
			   what, actual, strlen(actual), expected, strlen(expected));
	}
}

// src/screen.c, src/io.c, src/login.c and src/common.c are not linked in: editor.c only
// reaches them through the entry points stubbed below, which record what the display layer
// is asked to do and hand back a scripted sequence of keystrokes.
volatile int SYS_server_exit = 0;
time_t BBS_last_access_tm = 0;
const char DATA_EDITOR_HELP[] = "data/editor_help.txt";

static int moveto_count;
static int moveto_row;
static int moveto_col;
static int prints_count;
static char printed_last[2 * LINE_BUFFER_LEN];
static int clrline_count;
static int clrtoeol_count;
static int iflush_count;
static int display_file_count;
static char display_file_name[FILE_NAME_LEN];
static int online_update_count;

void moveto(int row, int col)
{
	moveto_count++;
	moveto_row = row;
	moveto_col = col;
}

void clrtoeol(void)
{
	clrtoeol_count++;
}

void clrline(int line_begin, int line_end)
{
	(void)line_begin;
	(void)line_end;

	clrline_count++;
}

int prints(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(printed_last, sizeof(printed_last), format, ap);
	va_end(ap);

	prints_count++;

	return 0;
}

int iflush(void)
{
	iflush_count++;

	return 0;
}

int display_file(const char *filename, int eof_exit)
{
	(void)eof_exit;

	display_file_count++;
	snprintf(display_file_name, sizeof(display_file_name), "%s", filename);

	return 0;
}

int user_online_update(const char *action)
{
	(void)action;

	online_update_count++;

	return 0;
}

// The editor asks for one key at a time, either with the idle timeout (igetch_t) or with a
// short per-byte timeout while a multi-byte character is still arriving (igetch). Both draw
// from the same scripted queue. An exhausted queue answers KEY_NULL, which the editor reads
// as "the user stopped typing" and returns from, so a wrong key handling cannot hang the run:
// the loop only ever consumes what was queued.
#define KEY_QUEUE_SIZE 512

static int key_queue[KEY_QUEUE_SIZE];
static int key_queue_len;
static int key_queue_pos;

static void reset_keys(void)
{
	key_queue_len = 0;
	key_queue_pos = 0;
}

static void push_key(int key)
{
	check_count++;

	if (key_queue_len >= KEY_QUEUE_SIZE)
	{
		failed_count++;
		printf("FAILED: the key queue holds more than %d keys\n", KEY_QUEUE_SIZE);

		return;
	}

	key_queue[key_queue_len++] = key;
}

// Type a printable string, one keystroke per character
static void push_keys(const char *str)
{
	for (; *str != '\0'; str++)
	{
		push_key((unsigned char)*str);
	}
}

static int next_key(void)
{
	if (key_queue_pos >= key_queue_len)
	{
		return KEY_NULL;
	}

	return key_queue[key_queue_pos++];
}

int igetch(int timeout)
{
	(void)timeout;

	return next_key();
}

int igetch_t(int sec)
{
	(void)sec;

	return next_key();
}

static void reset_screen_log(void)
{
	moveto_count = 0;
	moveto_row = -1;
	moveto_col = -1;
	prints_count = 0;
	memset(printed_last, 0, sizeof(printed_last));
	clrline_count = 0;
	clrtoeol_count = 0;
	iflush_count = 0;
	display_file_count = 0;
	memset(display_file_name, 0, sizeof(display_file_name));
	online_update_count = 0;
}

static int utf8_fixed_width_saved;

static void use_fixed_width(int fixed)
{
	utf8_fixed_width_saved = UTF8_fixed_width;
	UTF8_fixed_width = fixed;
}

static void restore_fixed_width(void)
{
	UTF8_fixed_width = utf8_fixed_width_saved;
}

static void fill_chars(char *buf, size_t count, char c)
{
	memset(buf, (unsigned char)c, count);
	buf[count] = '\0';
}

// text with `run` inserted at `at`, which is what typing into a line has to produce
static void splice_text(char *out, size_t out_len, const char *text, size_t at, const char *run)
{
	snprintf(out, out_len, "%.*s%s%s", (int)at, text, run, text + at);
}

// text with the byte at `at` (a line feed) dropped and `run` put in its place, which is what
// joining two lines has to produce
static void cut_text(char *out, size_t out_len, const char *text, size_t at, const char *run)
{
	snprintf(out, out_len, "%.*s%s%s", (int)at, text, run, text + at + 1);
}

// ---------------------------------------------------------------------------
// the model: display lines are windows into data lines, data lines are pool blocks
// ---------------------------------------------------------------------------

// Asking for a display line the model does not have answers with a sentinel instead of
// dereferencing NULL, so one broken display_line_total still leaves the checks after it to run.
#define NO_LINE ((long)-1)

static char *dl_ptr(const EDITOR_DATA *p_ed, long i)
{
	if (p_ed == NULL || i < 0 || i >= p_ed->display_line_total || p_ed->p_display_lines[i] == NULL)
	{
		return NULL;
	}

	return p_ed->p_display_lines[i];
}

static long dl_len(const EDITOR_DATA *p_ed, long i)
{
	if (p_ed == NULL || i < 0 || i >= p_ed->display_line_total)
	{
		return NO_LINE;
	}

	return p_ed->display_line_lengths[i];
}

static long dl_width(const EDITOR_DATA *p_ed, long i)
{
	if (p_ed == NULL || i < 0 || i >= p_ed->display_line_total)
	{
		return NO_LINE;
	}

	return p_ed->display_line_widths[i];
}

static long dl_total(const EDITOR_DATA *p_ed)
{
	if (p_ed == NULL)
	{
		return NO_LINE;
	}

	return p_ed->display_line_total;
}

static const char *dl_text(const EDITOR_DATA *p_ed, long i)
{
	return dl_ptr(p_ed, i) != NULL ? dl_ptr(p_ed, i) : "";
}

static int model_blocks;
static int model_worst_content;
static int model_bad_span;
static int model_bad_nul;
static int model_bad_over;
static int model_bad_break;

// A display line continues the data line of its predecessor when it starts exactly where that
// one ends, and a data line is one pool block of MAX_EDITOR_DATA_LINE_LENGTH bytes. Blocks are
// handed out one after another inside a chunk, so two of them are exactly that far apart and
// the two cases cannot be confused.
static void model_scan(const EDITOR_DATA *p_ed)
{
	char *p_block = NULL;
	int ends_line;
	long content = 0;
	long i;

	model_blocks = 0;
	model_worst_content = 0;
	model_bad_span = 0;
	model_bad_nul = 0;
	model_bad_over = 0;
	model_bad_break = 0;

	if (p_ed == NULL)
	{
		return;
	}

	for (i = 0; i < p_ed->display_line_total; i++)
	{
		if (p_ed->p_display_lines[i] == NULL || p_ed->display_line_lengths[i] < 0 ||
			p_ed->display_line_widths[i] < 0)
		{
			model_bad_span++;

			continue;
		}

		if (i == 0 || p_ed->p_display_lines[i] != p_ed->p_display_lines[i - 1] + p_ed->display_line_lengths[i - 1])
		{
			p_block = p_ed->p_display_lines[i];
			content = 0;
			model_blocks++;
		}

		content += p_ed->display_line_lengths[i];

		if (p_block == NULL || p_ed->p_display_lines[i] < p_block ||
			p_ed->p_display_lines[i] + p_ed->display_line_lengths[i] > p_block + MAX_EDITOR_DATA_LINE_LENGTH)
		{
			model_bad_span++;
		}

		ends_line = p_ed->display_line_lengths[i] > 0 &&
					p_ed->p_display_lines[i][p_ed->display_line_lengths[i] - 1] == '\n';

		if (ends_line && i + 1 < p_ed->display_line_total &&
			p_ed->p_display_lines[i + 1] == p_block + content)
		{
			// The line feed that ends a data line is the last byte of that data line, so the
			// text behind it belongs to the block of the next one
			model_bad_break++;
		}

		if (i + 1 == p_ed->display_line_total || ends_line ||
			p_ed->p_display_lines[i + 1] != p_ed->p_display_lines[i] + p_ed->display_line_lengths[i])
		{
			// A data line that does not end on a line feed can still have the cursor on its
			// last byte, where a split writes a line feed and a terminator, so it keeps room
			// for two bytes. One that ends on a line feed only has to keep its terminator.
			if (content > MAX_EDITOR_DATA_LINE_LENGTH - (ends_line ? 1 : 2))
			{
				model_bad_over++;
			}

			if (content > model_worst_content)
			{
				model_worst_content = (int)content;
			}

			// The rest of the block past the text of the data line is its own terminator
			if (content < MAX_EDITOR_DATA_LINE_LENGTH && p_block != NULL && p_block[content] != '\0')
			{
				model_bad_nul++;
			}
		}
	}
}

// The counts of model_scan(), read through these so that no check can compare a value that
// an earlier document left behind
static int data_lines(const EDITOR_DATA *p_ed)
{
	model_scan(p_ed);

	return model_blocks;
}

static int widest_data_line(const EDITOR_DATA *p_ed)
{
	model_scan(p_ed);

	return model_worst_content;
}

// Checks that must hold whatever splitting the editor picked for the text
static void check_model(const EDITOR_DATA *p_ed, const char *what)
{
	char tag[160];

	model_scan(p_ed);

	snprintf(tag, sizeof(tag), "%s: display_line_total is a model", what);
	check_true(dl_total(p_ed) >= 1 && p_ed->display_line_total <= MAX_EDITOR_DATA_LINES, tag);

	snprintf(tag, sizeof(tag), "%s: every display line lies inside its data line", what);
	check_int(model_bad_span, 0, tag);

	// A data line that fills its block leaves no room for the line feed and the terminator
	// that splitting it at the end has to write behind its text, and those two bytes land in
	// the neighbouring block.
	snprintf(tag, sizeof(tag), "%s: every data line keeps its split reserve", what);
	check_int(model_bad_over, 0, tag);

	snprintf(tag, sizeof(tag), "%s: every data line is terminated inside its block", what);
	check_int(model_bad_nul, 0, tag);

	snprintf(tag, sizeof(tag), "%s: a line feed ends its data line", what);
	check_int(model_bad_break, 0, tag);
}

// The model can hold the whole document pool, so the buffers of a round trip are sized for it
#define JOIN_BUF_LEN (MAX_EDITOR_DATA_LINES + MAX_EDITOR_DATA_LINE_LENGTH + 2)

static char joined[JOIN_BUF_LEN];
static char saved[JOIN_BUF_LEN];

// The saved text is what the caller writes back to a file or the database, so it has to be
// exactly the concatenation of the display lines
static const char *save_joined(const EDITOR_DATA *p_ed, const char *what)
{
	long joined_len = 0;
	long len;
	long ret;
	long i;

	joined[0] = '\0';
	saved[0] = '\0';

	if (p_ed != NULL)
	{
		for (i = 0; i < p_ed->display_line_total; i++)
		{
			len = p_ed->display_line_lengths[i];

			if (len < 0 || joined_len + len >= (long)sizeof(joined))
			{
				joined_len = -1;

				break;
			}

			memcpy(joined + joined_len, p_ed->p_display_lines[i], (size_t)len);
			joined_len += len;
		}

		if (joined_len >= 0)
		{
			joined[joined_len] = '\0';
		}
	}

	ret = editor_data_save(p_ed, saved, sizeof(saved));

	check_int(ret, joined_len, "editor_data_save() of the whole model");

	if (ret >= 0 && joined_len >= 0)
	{
		check_str(saved, joined, what);
	}

	return saved;
}

// ---------------------------------------------------------------------------

static void test_pool_lifecycle(void)
{
	char buf[64];
	EDITOR_DATA *p_ed;
	long display_line = 0;
	long offset = 0;
	long last_updated = 0;
	int ret;

	printf("Testing #1 ...\n");

	// The pools live in statics of editor.c, so cleaning up before the first init has nothing
	// to release and must not crash on the way to it
	editor_memory_pool_cleanup();
	check_int(editor_memory_pool_init(), 0, "editor_memory_pool_init() on a fresh run");
	check_int(editor_memory_pool_init(), -1, "editor_memory_pool_init() of a second run");
	check_int(editor_memory_pool_init(), -1, "editor_memory_pool_init() of a third run");

	// A refused re-init leaves the working pair of pools alone
	p_ed = editor_data_load("alive");
	check_true(p_ed != NULL, "editor_data_load() after a refused re-init");
	check_str(dl_text(p_ed, 0), "alive", "the text of the first document");

	// One document at a time: the editor record comes from a pool of one block
	check_true(editor_data_load("second") == NULL, "editor_data_load() while a document is open");

	editor_data_cleanup(p_ed);
	p_ed = editor_data_load("second");
	check_true(p_ed != NULL, "editor_data_load() after the document was released");
	check_str(dl_text(p_ed, 0), "second", "the text of the second document");
	editor_data_cleanup(p_ed);

	// Closing the pools releases the blocks underneath a document, which is why bbs_main.c
	// does it only once every editor session is closed
	editor_memory_pool_cleanup();
	check_true(editor_data_load("pools closed") == NULL, "editor_data_load() with the pools closed");
	editor_memory_pool_cleanup();
	check_int(editor_memory_pool_init(), 0, "editor_memory_pool_init() after a double cleanup");

	// NULL arguments are refused before anything of theirs is read or written
	check_true(editor_data_load(NULL) == NULL, "editor_data_load(NULL)");
	editor_data_cleanup(NULL);
	check_int(editor_data_save(NULL, buf, sizeof(buf)), -1, "editor_data_save(NULL document)");

	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(NULL, &display_line, &offset, "x", 1, &last_updated);
	check_int(ret, -1, "editor_data_insert(NULL document)");
	ret = editor_data_delete(NULL, &display_line, &offset, &last_updated, 0);
	check_int(ret, -1, "editor_data_delete(NULL document)");

	p_ed = editor_data_load("abc");
	check_true(p_ed != NULL, "editor_data_load() of three characters");
	check_int(editor_data_save(p_ed, NULL, sizeof(buf)), -1, "editor_data_save() into a NULL buffer");
	check_int(editor_data_insert(p_ed, &display_line, &offset, "x", 1, NULL), -1,
			"editor_data_insert() with a NULL last_updated_line");
	check_int(editor_data_delete(p_ed, &display_line, &offset, NULL, 0), -1,
			"editor_data_delete() with a NULL last_updated_line");
	check_int(editor_data_save(p_ed, buf, sizeof(buf)), 3, "editor_data_save() of three characters");
	check_str(buf, "abc", "the buffer of editor_data_save()");
	editor_data_cleanup(p_ed);

	printf("Completed testing #1: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_load_save(void)
{
	char buf[64];
	EDITOR_DATA *p_ed;
	long ret;

	printf("Testing #2 ...\n");

	// An empty text is one empty line, which is what a new post starts on
	p_ed = editor_data_load("");
	check_true(p_ed != NULL, "editor_data_load(\"\")");
	check_int(dl_total(p_ed), 1, "an empty text has one display line");
	check_int(dl_len(p_ed, 0), 0, "an empty text has an empty line");
	check_int(dl_width(p_ed, 0), 0, "an empty text has a line of no width");
	check_true(dl_ptr(p_ed, 0) != NULL, "an empty text still owns a data line");
	check_int(editor_data_save(p_ed, buf, sizeof(buf)), 0, "editor_data_save() of an empty text");
	check_str(buf, "", "the buffer of an empty save");
	check_model(p_ed, "an empty text");
	editor_data_cleanup(p_ed);

	// Without a line feed the text is one line and the editor shows no line of its own behind
	p_ed = editor_data_load("abc");
	check_int(dl_total(p_ed), 1, "editor_data_load(\"abc\") display lines");
	check_int(dl_len(p_ed, 0), 3, "the length of abc");
	check_int(dl_width(p_ed, 0), 3, "the width of abc");
	check_str(dl_text(p_ed, 0), "abc", "the text of abc");
	check_int(data_lines(p_ed), 1, "abc is one data line");
	check_model(p_ed, "abc");
	editor_data_cleanup(p_ed);

	// A line feed belongs to the line it ends, and the caller adds it back on save, so the
	// trailing one of the text is dropped from that last line
	p_ed = editor_data_load("abc\n");
	check_int(dl_total(p_ed), 2, "editor_data_load(\"abc\\n\") display lines");
	check_int(dl_len(p_ed, 0), 4, "the length of the line with its break");
	check_int(dl_width(p_ed, 0), 4, "the width of the line with its break");
	check_str(dl_text(p_ed, 0), "abc\n", "the text of the line with its break");
	check_int(dl_len(p_ed, 1), 0, "the line the trailing break opens");
	check_int(data_lines(p_ed), 2, "abc and the line after it are two data lines");
	check_str(save_joined(p_ed, "the round trip of abc\\n"), "abc\n", "the round trip of abc\\n");
	check_model(p_ed, "abc with a trailing break");
	editor_data_cleanup(p_ed);

	p_ed = editor_data_load("ab\ncd");
	check_int(dl_total(p_ed), 2, "editor_data_load(\"ab\\ncd\") display lines");
	check_int(dl_len(p_ed, 0), 3, "the length of ab with its break");
	check_int(dl_len(p_ed, 1), 2, "the length of the last line");
	check_int(data_lines(p_ed), 2, "two lines are two data lines");
	check_str(save_joined(p_ed, "the round trip of ab and cd"), "ab\ncd", "the round trip of ab and cd");
	check_model(p_ed, "ab and cd");
	editor_data_cleanup(p_ed);

	p_ed = editor_data_load("one\ntwo\nthree");
	check_int(dl_total(p_ed), 3, "editor_data_load() of three lines");
	check_int(data_lines(p_ed), 3, "three lines are three data lines");
	check_str(save_joined(p_ed, "the round trip of three lines"), "one\ntwo\nthree", "the round trip of three lines");
	check_model(p_ed, "three lines");
	editor_data_cleanup(p_ed);

	// Tabs are folded into a single space on the way in, so no data line ever holds one
	p_ed = editor_data_load("a\tb\n\tc");
	check_int(dl_total(p_ed), 2, "editor_data_load() of text with tabs");
	check_int(dl_len(p_ed, 0), 4, "a tab counts as one character of its line");
	check_str(dl_text(p_ed, 0), "a b\n", "a tab becomes a space");
	check_int(dl_len(p_ed, 1), 2, "a tab at the start of the last line becomes a space");
	check_str(dl_text(p_ed, 1), " c", "a tab at the start of a line becomes a space");
	check_str(save_joined(p_ed, "the round trip of text with tabs"), "a b\n c", "the round trip of text with tabs");
	check_true(dl_ptr(p_ed, 0) != NULL && dl_ptr(p_ed, 1) != NULL &&
				   strchr(dl_ptr(p_ed, 0), '\t') == NULL && strchr(dl_ptr(p_ed, 1), '\t') == NULL,
		   "no tab is stored in the model");
	check_model(p_ed, "text with tabs");
	editor_data_cleanup(p_ed);

	// A carriage return or a bell is skipped by the splitter: the byte stays in the line but
	// takes no column of it
	p_ed = editor_data_load("a\r\nb\rc");
	check_int(dl_total(p_ed), 2, "editor_data_load() of CRLF text");
	check_int(dl_len(p_ed, 0), 3, "a carriage return stays in the length");
	check_int(dl_width(p_ed, 0), 2, "a carriage return takes no width");
	check_int(dl_len(p_ed, 1), 3, "a carriage return inside a line stays in the length");
	check_int(dl_width(p_ed, 1), 2, "a carriage return inside a line takes no width");
	check_str(save_joined(p_ed, "the round trip of CRLF text"), "a\r\nb\rc", "the round trip of CRLF text");
	check_model(p_ed, "CRLF text");
	editor_data_cleanup(p_ed);

	use_fixed_width(1);

	// Fixed width mode counts one multi-byte character as two columns
	p_ed = editor_data_load("中文");
	check_int(dl_total(p_ed), 1, "editor_data_load() of two CJK characters");
	check_int(dl_len(p_ed, 0), 6, "the bytes of two CJK characters");
	check_int(dl_width(p_ed, 0), 4, "the width of two CJK characters");
	check_str(save_joined(p_ed, "the round trip of CJK text"), "中文", "the round trip of CJK text");
	check_model(p_ed, "CJK text");
	editor_data_cleanup(p_ed);

	p_ed = editor_data_load("a中b文c");
	check_int(dl_len(p_ed, 0), 9, "the bytes of mixed CJK and ASCII");
	check_int(dl_width(p_ed, 0), 7, "the width of mixed CJK and ASCII");
	check_model(p_ed, "mixed CJK and ASCII");
	editor_data_cleanup(p_ed);

	restore_fixed_width();

	// The editor measures a line with its control sequences in it, which is why it shows an
	// escape byte as a star instead of letting it move the cursor
	p_ed = editor_data_load("\033[32mred\033[m");
	check_int(dl_total(p_ed), 1, "editor_data_load() of coloured text");
	check_int(dl_len(p_ed, 0), 11, "the bytes of coloured text");
	check_int(dl_width(p_ed, 0), 11, "coloured text keeps the width of its sequences");
	check_str(save_joined(p_ed, "the round trip of coloured text"), "\033[32mred\033[m",
			"the round trip of coloured text");
	check_model(p_ed, "coloured text");
	editor_data_cleanup(p_ed);

	// The save buffer is owned by the caller: a text that does not fit is cut at the last
	// whole display line, with the terminator inside the buffer, and a buffer that fits the
	// text exactly is filled completely
	p_ed = editor_data_load("Hello\nWorld");
	check_int(dl_total(p_ed), 2, "editor_data_load() of two lines for the buffer checks");

	ret = editor_data_save(p_ed, buf, 12);
	check_int(ret, 11, "editor_data_save() into a buffer with room for the terminator");
	check_str(buf, "Hello\nWorld", "the buffer of an exact save");

	ret = editor_data_save(p_ed, buf, 11);
	check_int(ret, -2, "editor_data_save() one byte short of the text");
	check_str(buf, "Hello\n", "a short save stops after the last whole display line");

	buf[5] = '#';
	buf[6] = '#';
	ret = editor_data_save(p_ed, buf, 5);
	check_int(ret, -2, "editor_data_save() short of even the first line");
	check_str(buf, "", "a save that fits no line writes no line");
	check_int(buf[5], '#', "a refused save leaves the bytes past its terminator alone");
	check_int(buf[6], '#', "a refused save writes one terminator byte only");

	check_model(p_ed, "two lines for the buffer checks");
	editor_data_cleanup(p_ed);

	printf("Completed testing #2: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_wrapping(void)
{
	char text[MAX_EDITOR_DATA_LINE_LENGTH * 2];
	EDITOR_DATA *p_ed;
	long i;
	long pos;

	printf("Testing #3 ...\n");

	// A line exactly as wide as the screen is one display line, one wider wraps
	fill_chars(text, (size_t)SCREEN_COLS, 'x');
	p_ed = editor_data_load(text);
	check_int(dl_total(p_ed), 1, "a screen wide line is one display line");
	check_int(dl_len(p_ed, 0), SCREEN_COLS, "the length of a screen wide line");
	check_int(dl_width(p_ed, 0), SCREEN_COLS, "the width of a screen wide line");
	check_int(data_lines(p_ed), 1, "a screen wide line is one data line");
	check_model(p_ed, "a screen wide line");
	editor_data_cleanup(p_ed);

	fill_chars(text, (size_t)SCREEN_COLS + 1, 'x');
	p_ed = editor_data_load(text);
	check_int(dl_total(p_ed), 2, "one character past the screen width wraps");
	check_int(dl_len(p_ed, 0), SCREEN_COLS, "the head of the wrap takes the whole screen width");
	check_int(dl_len(p_ed, 1), 1, "the tail of the wrap is one character");
	check_int(data_lines(p_ed), 1, "a wrapped line is still one data line");
	check_str(save_joined(p_ed, "the round trip of the wrap"), text, "the round trip of the wrap");
	check_model(p_ed, "one character past the screen width");
	editor_data_cleanup(p_ed);

	// Two whole screens and a piece, all of it inside one data line
	fill_chars(text, 200, 'x');
	p_ed = editor_data_load(text);
	check_int(dl_total(p_ed), 3, "a 200 character line is three display lines");
	check_int(dl_len(p_ed, 0), 80, "the first display line of a 200 character line");
	check_int(dl_len(p_ed, 1), 80, "the second display line of a 200 character line");
	check_int(dl_len(p_ed, 2), 40, "the last display line of a 200 character line");
	check_int(data_lines(p_ed), 1, "a 200 character line is one data line");
	check_true(dl_ptr(p_ed, 1) == dl_ptr(p_ed, 0) + 80 && dl_ptr(p_ed, 2) == dl_ptr(p_ed, 1) + 80,
			"display lines of one data line follow each other in it");
	check_str(save_joined(p_ed, "the round trip of a 200 character line"), text,
			"the round trip of a 200 character line");
	check_model(p_ed, "a 200 character line");
	editor_data_cleanup(p_ed);

	// A data line is one pool block, so a logical line past the room of a block is broken
	// into several data lines that the reader of the document cannot tell apart
	fill_chars(text, MAX_EDITOR_DATA_LINE_LENGTH - 2, 'a');
	p_ed = editor_data_load(text);
	check_int(dl_total(p_ed), 13, "a 1022 character line is thirteen display lines");
	check_int(dl_len(p_ed, 12), 62, "the last display line of a 1022 character line");
	check_int(data_lines(p_ed), 1, "a 1022 character line fits one data line");
	check_int(widest_data_line(p_ed), MAX_EDITOR_DATA_LINE_LENGTH - 2,
			"a 1022 character line fills its block up to the reserve");
	check_str(save_joined(p_ed, "the round trip of a 1022 character line"), text,
			"the round trip of a 1022 character line");
	check_model(p_ed, "a 1022 character line");
	editor_data_cleanup(p_ed);

	fill_chars(text, MAX_EDITOR_DATA_LINE_LENGTH - 1, 'a');
	p_ed = editor_data_load(text);
	check_int(dl_total(p_ed), 13, "a 1023 character line is thirteen display lines");
	check_int(data_lines(p_ed), 2, "a 1023 character line does not fit one data line");
	check_str(save_joined(p_ed, "the round trip of a 1023 character line"), text,
			"the round trip of a 1023 character line");
	check_model(p_ed, "a 1023 character line");
	editor_data_cleanup(p_ed);

	fill_chars(text, 2000, 'b');
	p_ed = editor_data_load(text);
	check_int(dl_total(p_ed), 25, "a 2000 character line is twenty five display lines");
	check_int(data_lines(p_ed), 3, "a 2000 character line needs three data lines");
	check_str(save_joined(p_ed, "the round trip of a 2000 character line"), text,
			"the round trip of a 2000 character line");
	check_model(p_ed, "a 2000 character line");
	editor_data_cleanup(p_ed);

	// Long lines, short lines and wrapped lines in one document, which is what an article is
	pos = 0;

	for (i = 0; i < 30; i++)
	{
		fill_chars(text + pos, (size_t)(13 * (i % 9) + 1), 'c');
		pos += (long)strlen(text + pos);
		text[pos++] = '\n';
	}

	text[pos] = '\0';
	p_ed = editor_data_load(text);
	check_str(save_joined(p_ed, "the round trip of the article text"), text, "the round trip of the article text");
	check_true(data_lines(p_ed) >= 30, "the article text opens one data line per line at least");
	check_model(p_ed, "the article text");
	editor_data_cleanup(p_ed);

	// Loading and releasing the same document over and over has to hand back exactly the
	// blocks it took, or the pools run out and the editor stops opening documents at all
	for (i = 0; i < 60; i++)
	{
		p_ed = editor_data_load("one\ntwo\nthree\n");

		if (p_ed == NULL)
		{
			check_true(0, "editor_data_load() of the pool churn loop");

			break;
		}

		check_int(data_lines(p_ed), 4, "three lines with a trailing break are four data lines");
		editor_data_cleanup(p_ed);
	}

	p_ed = editor_data_load("one\ntwo\nthree\n");
	check_true(p_ed != NULL, "editor_data_load() after the pool churn loop");
	check_model(p_ed, "the document after the pool churn loop");
	editor_data_cleanup(p_ed);

	printf("Completed testing #3: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_insert_in_place(void)
{
	char expect[128];
	char run[64];
	EDITOR_DATA *p_ed;
	long display_line;
	long offset;
	long last_updated;
	int ret;

	printf("Testing #4 ...\n");

	p_ed = editor_data_load("Hello World\nsecond line\n");
	check_true(p_ed != NULL, "editor_data_load() for the in place inserts");
	check_int(dl_total(p_ed), 3, "the in place inserts start on three display lines");
	check_int(data_lines(p_ed), 3, "the in place inserts start on three data lines");

	// A run that keeps the data line inside its block is written into the line itself and the
	// display lines of that line are measured again
	display_line = 0;
	offset = 5;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, ",", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() of one comma");
	check_str(dl_text(p_ed, 0), "Hello, World\n", "the data line after the comma");
	check_int(dl_len(p_ed, 0), 13, "the length after the comma");
	check_int(dl_width(p_ed, 0), 13, "the width after the comma");
	check_int(display_line, 0, "the cursor stays on the edited display line");
	check_int(offset, 6, "the cursor moves behind the inserted comma");
	check_int(last_updated, 0, "last_updated_line of the comma");
	check_int(dl_total(p_ed), 3, "the comma adds no display line");
	check_str(save_joined(p_ed, "the round trip after the comma"), "Hello, World\nsecond line\n",
			"the round trip after the comma");
	check_model(p_ed, "after the comma");

	// The same at the very start of a line
	display_line = 1;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "> ", 2, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a quote at the start of a line");
	check_str(dl_text(p_ed, 1), "> second line\n", "the data line after the quote");
	check_int(display_line, 1, "the cursor after the quote");
	check_int(offset, 2, "the offset after the quote");
	check_str(save_joined(p_ed, "the round trip after the quote"), "Hello, World\n> second line\n",
			"the round trip after the quote");
	check_model(p_ed, "after the quote");

	// Typing in front of the line feed of a line, which is where the cursor sits at the end
	// of a line that has one
	display_line = 0;
	offset = 12;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "!", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() at the end of a display line");
	check_str(dl_text(p_ed, 0), "Hello, World!\n", "the data line after the exclamation mark");
	check_int(dl_len(p_ed, 0), 14, "the length after the exclamation mark");
	check_int(offset, 13, "the cursor after the exclamation mark");
	check_str(save_joined(p_ed, "the round trip after the exclamation mark"),
			"Hello, World!\n> second line\n", "the round trip after the exclamation mark");
	check_model(p_ed, "after the exclamation mark");

	// Inserting into the trailing empty line of the text
	display_line = 2;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "tail", 4, &last_updated);
	check_int(ret, 0, "editor_data_insert() into the trailing empty line");
	check_str(dl_text(p_ed, 2), "tail", "the trailing line holds the inserted run");
	check_int(dl_len(p_ed, 2), 4, "the length of the trailing line after the insert");
	check_str(save_joined(p_ed, "the round trip of the trailing insert"), "Hello, World!\n> second line\ntail",
			"the round trip of the trailing insert");
	check_model(p_ed, "after the trailing insert");

	// A multi-byte character goes in as one unit and is measured as two columns
	use_fixed_width(1);

	display_line = 1;
	offset = 1;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "中", 3, &last_updated);
	check_int(ret, 0, "editor_data_insert() of one CJK character");
	check_str(dl_text(p_ed, 1), ">中 second line\n", "the data line after the CJK character");
	check_int(dl_len(p_ed, 1), 17, "the bytes of the line with the CJK character");
	check_int(dl_width(p_ed, 1), 16, "the columns of the line with the CJK character");
	check_int(offset, 4, "the cursor behind the CJK character");
	check_model(p_ed, "after the CJK character");
	restore_fixed_width();

	editor_data_cleanup(p_ed);

	// A run at the end of a wrapped line pushes the break of its data line onto a display
	// line of its own
	fill_chars(run, 20, 'T');
	p_ed = editor_data_load("aaaaaaaaaaaaaaaa\nbbbb\n");
	check_int(dl_total(p_ed), 3, "editor_data_load() of the wrap insert text");

	display_line = 0;
	offset = 16;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, run, 20, &last_updated);
	check_int(ret, 0, "editor_data_insert() of twenty characters at the end of a wrapped line");
	check_int(dl_len(p_ed, 0), 37, "the run goes in front of the line feed of its line");
	check_int(dl_width(p_ed, 0), 37, "the width after the run at the end of the line");
	check_str(dl_text(p_ed, 0), "aaaaaaaaaaaaaaaaTTTTTTTTTTTTTTTTTTTT\n", "the display line of the inserted run");
	check_int(dl_len(p_ed, 1), 5, "the line behind the run is not moved");
	check_int(dl_total(p_ed), 3, "the wrap insert adds no display line");
	check_int(display_line, 0, "the cursor stays on the display line of the run");
	check_int(offset, 36, "the cursor sits behind the inserted run");
	check_str(save_joined(p_ed, "the round trip of the wrap insert"),
			"aaaaaaaaaaaaaaaaTTTTTTTTTTTTTTTTTTTT\nbbbb\n", "the round trip of the wrap insert");
	check_model(p_ed, "the wrap insert");
	editor_data_cleanup(p_ed);

	// The same run at the start of a line, where the text it pushes out wraps
	p_ed = editor_data_load("aaaaaaaaaaaaaaaa\nbbbb\n");
	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, run, 20, &last_updated);
	check_int(ret, 0, "editor_data_insert() of twenty characters at the start of a line");
	check_int(display_line, 0, "the cursor of the run at the start of a line");
	check_int(offset, 20, "the offset of the run at the start of a line");
	check_int(dl_len(p_ed, 0), 37, "the run and the head of the line share a display line");
	snprintf(expect, sizeof(expect), "%saaaaaaaaaaaaaaaa\n", run);
	check_str(dl_text(p_ed, 0), expect, "the run comes before the text it pushed");
	check_int(dl_total(p_ed), 3, "the run at the start of a line adds no display line");
	snprintf(expect, sizeof(expect), "%saaaaaaaaaaaaaaaa\nbbbb\n", run);
	check_str(save_joined(p_ed, "the round trip of the run at the start"), expect,
			"the round trip of the run at the start");
	check_model(p_ed, "the run at the start of a line");
	editor_data_cleanup(p_ed);

	// An insert into an empty document gives it its first line
	p_ed = editor_data_load("");
	check_int(dl_total(p_ed), 1, "editor_data_load(\"\") for the first insert");
	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "start", 5, &last_updated);
	check_int(ret, 0, "editor_data_insert() into an empty document");
	check_str(dl_text(p_ed, 0), "start", "the first text of an empty document");
	check_int(dl_width(p_ed, 0), 5, "the width of the first text");
	check_int(dl_total(p_ed), 1, "the first insert adds no display line");
	check_str(save_joined(p_ed, "the round trip of the first insert"), "start", "the round trip of the first insert");
	check_model(p_ed, "the first insert");
	editor_data_cleanup(p_ed);

	printf("Completed testing #4: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_insert_line_break(void)
{
	char text[MAX_EDITOR_DATA_LINE_LENGTH * 2];
	char expect[MAX_EDITOR_DATA_LINE_LENGTH * 3];
	EDITOR_DATA *p_ed;
	long display_line;
	long offset;
	long last_updated;
	int ret;

	printf("Testing #5 ...\n");

	// A carriage return always takes a data line of its own: the head of the line ends on the
	// break and everything behind the cursor moves into a new line
	p_ed = editor_data_load("abcdef\n");
	check_true(p_ed != NULL, "editor_data_load() for the break inserts");
	check_int(dl_total(p_ed), 2, "the break inserts start on two display lines");
	check_int(data_lines(p_ed), 2, "the break inserts start on two data lines");

	display_line = 0;
	offset = 3;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a break in mid line");
	check_str(dl_text(p_ed, 0), "abc\n", "the head keeps the break");
	check_str(dl_text(p_ed, 1), "def\n", "the tail moves into a line of its own");
	check_int(display_line, 1, "the cursor moves onto the new line");
	check_int(offset, 0, "the cursor sits at the start of the new line");
	check_int(dl_total(p_ed), 3, "a break in mid line adds one display line");
	check_int(data_lines(p_ed), 3, "a break in mid line adds one data line");
	check_str(save_joined(p_ed, "the round trip of a break in mid line"), "abc\ndef\n",
			"the round trip of a break in mid line");
	check_model(p_ed, "a break in mid line");

	// Breaking at the start of a line pushes the whole line down
	display_line = 1;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a break at the start of a line");
	check_str(dl_text(p_ed, 1), "\n", "the break at the start leaves an empty line");
	check_str(dl_text(p_ed, 2), "def\n", "the line pushed down keeps its text");
	check_int(display_line, 2, "the cursor moves onto the text pushed down");
	check_int(offset, 0, "the cursor stays at the start of the text");
	check_str(save_joined(p_ed, "the round trip of a break at the start"), "abc\n\ndef\n",
			"the round trip of a break at the start");
	check_model(p_ed, "a break at the start of a line");

	// Breaking at the end of a line, where the cursor sits past the last byte
	display_line = 2;
	offset = 4;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a break at the end of a line");
	check_int(offset, 0, "the cursor sits at the start of the line the break opens");
	check_str(save_joined(p_ed, "the round trip of a break at the end"), "abc\n\ndef\n\n",
			"the round trip of a break at the end");
	check_model(p_ed, "a break at the end of a line");

	// And breaking the empty line that the trailing break of the text opened
	display_line = 3;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a break on the last empty line");
	check_str(save_joined(p_ed, "the round trip of a break on the last line"), "abc\n\ndef\n\n\n",
			"the round trip of a break on the last line");
	check_model(p_ed, "a break on the last empty line");
	editor_data_cleanup(p_ed);

	// Breaking a line of the text that has no break of its own: the tail keeps its text and
	// the head ends on the break the insert writes
	p_ed = editor_data_load("0123456789");
	check_int(dl_total(p_ed), 1, "editor_data_load() of ten digits");

	display_line = 0;
	offset = 5;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a break inside a line with no break");
	check_str(dl_text(p_ed, 0), "01234\n", "the head of the break inside a line");
	check_str(dl_text(p_ed, 1), "56789", "the tail of the break inside a line");
	check_int(dl_width(p_ed, 0), 6, "the width of the head of the break inside a line");
	check_int(dl_width(p_ed, 1), 5, "the width of the tail of the break inside a line");
	check_int(display_line, 1, "the cursor of the break inside a line");
	check_int(dl_total(p_ed), 2, "a break inside a line adds one display line");
	check_str(save_joined(p_ed, "the round trip of the break inside a line"), "01234\n56789",
			"the round trip of the break inside a line");
	check_model(p_ed, "a break inside a line with no break");
	editor_data_cleanup(p_ed);

	// Breaking a logical line of thirty characters, which had no display line of its own
	p_ed = editor_data_load("012345678901234567890123456789");
	check_int(dl_total(p_ed), 1, "editor_data_load() of thirty characters");

	display_line = 0;
	offset = 10;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a break in a thirty character line");
	check_str(dl_text(p_ed, 0), "0123456789\n", "the head of the break in a thirty character line");
	check_str(dl_text(p_ed, 1), "01234567890123456789", "the tail of the break in a thirty character line");
	check_str(save_joined(p_ed, "the round trip of the break in a thirty character line"),
			"0123456789\n01234567890123456789", "the round trip of the break in a thirty character line");
	check_model(p_ed, "a break in a thirty character line");
	editor_data_cleanup(p_ed);

	// Breaking a wrapped line: the display lines of the tail are rebuilt from the new data
	// line and the head is cut short
	fill_chars(text, 200, 'w');
	p_ed = editor_data_load(text);
	check_int(dl_total(p_ed), 3, "editor_data_load() of a 200 character line for a break");

	display_line = 1;
	offset = 40;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a break inside a wrapped line");
	splice_text(expect, sizeof(expect), text, 120, "\n");
	check_str(save_joined(p_ed, "the round trip of the break inside a wrapped line"), expect,
			"the round trip of the break inside a wrapped line");
	check_int(dl_len(p_ed, 1), 41, "the display line of the break is cut to the break");
	check_int(data_lines(p_ed), 2, "the break inside a wrapped line opens a data line");
	check_model(p_ed, "a break inside a wrapped line");
	editor_data_cleanup(p_ed);

	printf("Completed testing #5: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_insert_over_length(void)
{
	char text[MAX_EDITOR_DATA_LINE_LENGTH * 2];
	char run[64];
	char expect[MAX_EDITOR_DATA_LINE_LENGTH * 3];
	EDITOR_DATA *p_ed;
	long display_line;
	long offset;
	long last_updated;
	int ret;

	printf("Testing #6 ...\n");

	// A run that pushes the data line past the room of its block: everything behind the
	// cursor moves into a new data line and the line the cursor was on ends on the run
	fill_chars(text, MAX_EDITOR_DATA_LINE_LENGTH - 2, 'y');
	p_ed = editor_data_load(text);
	check_true(p_ed != NULL, "editor_data_load() of a 1022 character line");
	check_int(dl_total(p_ed), 13, "a 1022 character line is thirteen display lines");
	check_int(data_lines(p_ed), 1, "a 1022 character line fits one data line");

	fill_chars(run, 20, 'T');
	display_line = 6;
	offset = 20;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, run, 20, &last_updated);
	check_int(ret, 0, "editor_data_insert() of twenty characters past the room of a block");
	check_int(display_line, 6, "the cursor of the run that pushes the tail out");
	check_int(offset, 40, "the offset of the run that pushes the tail out");
	check_int(dl_len(p_ed, 6), 41, "the edited display line holds its head, the run and the break");
	check_str(dl_text(p_ed, 6), "yyyyyyyyyyyyyyyyyyyyTTTTTTTTTTTTTTTTTTTT\n", "the run sits inside the edited line");
	check_int(dl_total(p_ed), 14, "the run that pushes the tail out adds one display line");
	check_int(data_lines(p_ed), 2, "the run that pushes the tail out opens a data line");
	check_int(dl_len(p_ed, 7), 80, "the tail of the line keeps the screen width");
	splice_text(expect, sizeof(expect), text, 500, "TTTTTTTTTTTTTTTTTTTT\n");
	check_str(save_joined(p_ed, "the round trip of the run that pushes the tail out"), expect,
			"the round trip of the run that pushes the tail out");
	check_model(p_ed, "the run that pushes the tail out");
	editor_data_cleanup(p_ed);

	// Typing into the end of a data line that already has the reserve: the run itself does
	// not fit behind the text, so it starts the new data line
	fill_chars(text, MAX_EDITOR_DATA_LINE_LENGTH - 2, 'a');
	p_ed = editor_data_load(text);
	display_line = 12;
	offset = 62;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "xyz", 3, &last_updated);
	check_int(ret, 0, "editor_data_insert() at the end of a full block line");
	check_int(display_line, 13, "the cursor moves onto the data line the run opened");
	check_int(offset, 3, "the cursor sits behind the run on the new data line");
	check_str(dl_text(p_ed, 13), "xyz", "the run survives the split of a full block line");
	check_int(dl_len(p_ed, 12), 63, "the head line of the split ends on its break");
	check_int(data_lines(p_ed), 2, "the split of a full block line opens a data line");
	snprintf(expect, sizeof(expect), "%s\nxyz", text);
	check_str(save_joined(p_ed, "the round trip of the split of a full block line"), expect,
			"a split of a full block line breaks the line where it ran out of room");
	check_model(p_ed, "the split of a full block line");
	editor_data_cleanup(p_ed);

	// The same position asked for with a break instead of a run
	fill_chars(text, MAX_EDITOR_DATA_LINE_LENGTH - 2, 'a');
	p_ed = editor_data_load(text);
	display_line = 12;
	offset = 62;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a break at the end of a full block line");
	check_int(display_line, 13, "the cursor moves onto the line the break opened");
	check_int(offset, 0, "the cursor sits at the start of the line the break opened");
	check_int(data_lines(p_ed), 2, "the break at the end of a full block line opens a data line");
	snprintf(expect, sizeof(expect), "%s\n", text);
	check_str(save_joined(p_ed, "the round trip of the break at the end of a full block line"), expect,
			"the round trip of the break at the end of a full block line");
	check_model(p_ed, "the break at the end of a full block line");
	editor_data_cleanup(p_ed);

	// A run longer than the reserve is written into the data line the cursor was on and its
	// own tail moves out, which is the other branch of the same room check
	fill_chars(text, MAX_EDITOR_DATA_LINE_LENGTH - 2, 'b');
	p_ed = editor_data_load(text);
	fill_chars(run, 59, 'R');
	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, run, 59, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a fifty nine character run at the start of a block");
	check_int(display_line, 0, "the cursor of the fifty nine character run");
	check_int(offset, 59, "the cursor sits behind the fifty nine character run");
	check_int(dl_len(p_ed, 0), 60, "the run and the break share the first display line");
	snprintf(expect, sizeof(expect), "%s\n", run);
	check_str(dl_text(p_ed, 0), expect, "the fifty nine character run is typed out in full");
	check_int(dl_total(p_ed), 14, "the fifty nine character run adds one display line");
	check_int(data_lines(p_ed), 2, "the fifty nine character run moves the rest into a data line");
	snprintf(expect, sizeof(expect), "%s\n%s", run, text);
	check_str(save_joined(p_ed, "the round trip of the fifty nine character run"), expect,
			"the round trip of the fifty nine character run");
	check_model(p_ed, "the fifty nine character run");
	editor_data_cleanup(p_ed);

	// Breaking a wrapped line inside a full block: the head display line ends on the break
	// and the rest of the block, wrap lines and all, moves into a new data line
	fill_chars(text, MAX_EDITOR_DATA_LINE_LENGTH - 2, 'c');
	p_ed = editor_data_load(text);
	display_line = 5;
	offset = 40;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() of a break inside a full block line");
	check_int(dl_len(p_ed, 5), 41, "the display line of the break keeps its head");
	snprintf(expect, sizeof(expect), "%.*s\n", 40, text + 400);
	check_str(dl_text(p_ed, 5), expect, "the break inside a full block line ends its data line");
	check_int(data_lines(p_ed), 2, "the break inside a full block line opens a data line");
	splice_text(expect, sizeof(expect), text, 440, "\n");
	check_str(save_joined(p_ed, "the round trip of the break inside a full block line"), expect,
			"the round trip of the break inside a full block line");
	check_model(p_ed, "the break inside a full block line");
	editor_data_cleanup(p_ed);

	printf("Completed testing #6: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_data_line_capacity(void)
{
	// The capacity tests build several logical lines of just over one block each
	char text[(MAX_EDITOR_DATA_LINE_LENGTH + 2) * 5];
	char expect[(MAX_EDITOR_DATA_LINE_LENGTH + 2) * 5 + 16];
	static char multi_block_text[MAX_EDITOR_DATA_LINE_LENGTH * 10 + 2];
	EDITOR_DATA *p_ed;
	long display_line;
	long offset;
	long last_updated;
	long len;
	long span;
	long span_max = 0;
	int i;
	int ret;

	printf("Testing #7 ...\n");

	// A data line has to keep two bytes spare: splitting it at the end writes a line feed and
	// a terminator behind the text it already holds, and past the block those two bytes land
	// in the neighbouring data line, quietly overwriting the first character of the document
	// part that got the block next.
	for (len = MAX_EDITOR_DATA_LINE_LENGTH - 3; len <= MAX_EDITOR_DATA_LINE_LENGTH + 1; len++)
	{
		fill_chars(text, (size_t)len, 'a');
		p_ed = editor_data_load(text);

		if (p_ed == NULL)
		{
			check_true(0, "editor_data_load() of the capacity loop");

			continue;
		}

		check_str(save_joined(p_ed, "the round trip of the capacity loop"), text,
				"the round trip of the capacity loop");
		check_model(p_ed, "the capacity loop");

		// Type one character at the end of the last display line. Past the room of its data
		// line the editor has to break the line there, so the text grows by the character and
		// by at most one line feed, and none of the text it already had may change.
		display_line = dl_total(p_ed) - 1;
		offset = dl_len(p_ed, display_line);

		if (offset < 0)
		{
			check_true(0, "the last display line of the capacity loop");
			editor_data_cleanup(p_ed);

			continue;
		}

		last_updated = 0;
		ret = editor_data_insert(p_ed, &display_line, &offset, "z", 1, &last_updated);
		check_int(ret, 0, "editor_data_insert() at the end of a line of the capacity loop");

		// Only a line that already fills its block up to the last spare byte has to break
		// where the character no longer fits
		snprintf(expect, sizeof(expect), "%s%sz", text,
				len >= MAX_EDITOR_DATA_LINE_LENGTH - 2 ? "\n" : "");
		check_str(save_joined(p_ed, "the round trip after typing at the end of the capacity loop"), expect,
				"the round trip after typing at the end of the capacity loop");
		check_model(p_ed, "after typing at the end of the capacity loop");

		// And the same position asked for with a break, which ends the line there
		display_line = dl_total(p_ed) - 1;
		offset = dl_len(p_ed, display_line);
		last_updated = 0;
		ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
		check_int(ret, 0, "editor_data_insert() of a break at the end of a line of the capacity loop");
		snprintf(expect, sizeof(expect), "%s%sz\n", text,
				len >= MAX_EDITOR_DATA_LINE_LENGTH - 2 ? "\n" : "");
		check_str(save_joined(p_ed, "the round trip after the break of the capacity loop"), expect,
				"the round trip after the break of the capacity loop");
		check_model(p_ed, "after the break of the capacity loop");

		editor_data_cleanup(p_ed);
	}

	// Several over-long logical lines in a row, so the data line that a stray terminator
	// would land in belongs to the same document and its first character can be read back
	for (i = 1; i <= 4; i++)
	{
		size_t pos = 0;
		int j;

		for (j = 0; j < i; j++)
		{
			fill_chars(text + pos, MAX_EDITOR_DATA_LINE_LENGTH + 1, 'd');
			pos += strlen(text + pos);
			text[pos++] = '\n';
		}

		text[pos] = '\0';
		p_ed = editor_data_load(text);

		if (p_ed == NULL)
		{
			check_true(0, "editor_data_load() of the multi line capacity text");

			continue;
		}

		check_str(save_joined(p_ed, "the round trip of the multi line capacity text"), text,
				"the round trip of the multi line capacity text");
		check_int(data_lines(p_ed), i * 2 + 1, "every over-long line of the capacity text takes two data lines");
		check_model(p_ed, "the multi line capacity text");

		display_line = dl_total(p_ed) - 1;
		offset = dl_len(p_ed, display_line);
		last_updated = 0;
		ret = editor_data_insert(p_ed, &display_line, &offset, "tail", 4, &last_updated);
		check_int(ret, 0, "editor_data_insert() at the end of the multi line capacity text");
		snprintf(expect, sizeof(expect), "%stail", text);
		check_str(save_joined(p_ed, "the round trip after typing at the end of the multi line capacity text"),
				expect, "the round trip after typing at the end of the multi line capacity text");
		check_model(p_ed, "after typing at the end of the multi line capacity text");

		display_line = dl_total(p_ed) - 1;
		offset = dl_len(p_ed, display_line);
		last_updated = 0;
		ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
		check_int(ret, 0, "editor_data_insert() of a break at the end of the multi line capacity text");
		snprintf(expect, sizeof(expect), "%stail\n", text);
		check_str(save_joined(p_ed, "the round trip after the break at the end of the multi line capacity text"),
				expect, "the round trip after the break at the end of the multi line capacity text");
		check_model(p_ed, "after the break at the end of the multi line capacity text");

		editor_data_cleanup(p_ed);
	}

	// A logical line longer than one data block is stored in several of them, and every block
	// has to come back to the pool when the document is released: a release that keeps one
	// behind marches forward through the pool until the editor cannot open a document at all.
	// The pools start fresh here, so the blocks of a document that is loaded and released
	// over and over stay in the handful of blocks it needs.
	editor_memory_pool_cleanup();
	check_int(editor_memory_pool_init(), 0, "editor_memory_pool_init() for the release cycle");

	fill_chars(multi_block_text, MAX_EDITOR_DATA_LINE_LENGTH * 10, 'a');

	for (i = 0; i < 200; i++)
	{
		p_ed = editor_data_load(multi_block_text);

		if (p_ed == NULL)
		{
			check_true(0, "editor_data_load() in the release cycle");

			break;
		}

		if (i == 0)
		{
			check_int(data_lines(p_ed), 11, "a ten thousand character line takes eleven data blocks");
		}

		span = p_ed->p_display_lines[p_ed->display_line_total - 1] - p_ed->p_display_lines[0];

		if (span < 0)
		{
			span = -span;
		}

		if (span > span_max)
		{
			span_max = span;
		}

		editor_data_cleanup(p_ed);
	}

	check_true(span_max < 64 * MAX_EDITOR_DATA_LINE_LENGTH,
			"the data blocks of a multi block line come back to the pool");

	printf("Completed testing #7: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_delete(void)
{
	char text[MAX_EDITOR_DATA_LINE_LENGTH * 2];
	char expect[MAX_EDITOR_DATA_LINE_LENGTH * 3];
	EDITOR_DATA *p_ed;
	long display_line;
	long offset;
	long last_updated;
	int ret;

	printf("Testing #8 ...\n");

	// A display line that ends a data line carries the line feed in its length and its width,
	// so the position past its text is one byte further than the last character. Fresh
	// documents throughout: no case here depends on the shape a previous one left behind.
	p_ed = editor_data_load("Hello\nWorld\nthird");
	check_true(p_ed != NULL, "editor_data_load() for the first delete");
	check_int(dl_total(p_ed), 3, "three lines are three display lines");
	check_int(dl_len(p_ed, 0), 6, "a line feed is part of the length of its line");
	check_int(dl_width(p_ed, 0), 6, "a line feed takes one column of its line");
	check_int(data_lines(p_ed), 3, "three lines are three data lines");

	display_line = 0;
	offset = 6;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 0);
	check_int(ret, 0, "editor_data_delete() with the cursor past the text of its line");
	check_int(display_line, 0, "the cursor of a refused delete");
	check_int(offset, 6, "the offset of a refused delete");
	check_str(save_joined(p_ed, "the round trip of a refused delete"), "Hello\nWorld\nthird",
			"the round trip of a refused delete");
	check_model(p_ed, "a refused delete");
	editor_data_cleanup(p_ed);

	// One character in mid line moves the rest of the data line one byte back
	p_ed = editor_data_load("Hello\nWorld\nthird");
	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 0);
	check_int(ret, 1, "editor_data_delete() of one character");
	check_str(dl_text(p_ed, 0), "ello\n", "the character is gone from the data line");
	check_int(dl_len(p_ed, 0), 5, "the length after the character delete");
	check_int(dl_width(p_ed, 0), 5, "the width after the character delete");
	check_int(display_line, 0, "the cursor of the character delete");
	check_int(offset, 0, "the offset of the character delete");
	check_int(data_lines(p_ed), 3, "the character delete releases no data line");
	check_str(save_joined(p_ed, "the round trip of the character delete"), "ello\nWorld\nthird",
			"the round trip of the character delete");
	check_model(p_ed, "the character delete");
	editor_data_cleanup(p_ed);

	// The line feed at the end of a line joins the line behind it into the data line
	p_ed = editor_data_load("Hello\nWorld\nthird");
	display_line = 0;
	offset = 5;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 0);
	check_int(ret, 1, "editor_data_delete() of a line feed");
	check_str(dl_text(p_ed, 0), "HelloWorld\n", "the two lines join into one data line");
	check_int(dl_len(p_ed, 0), 11, "the length of the joined line");
	check_int(dl_total(p_ed), 2, "the join drops one display line");
	check_int(data_lines(p_ed), 2, "the join releases one data line");
	check_int(display_line, 0, "the cursor of the join");
	check_int(offset, 5, "the offset of the join");
	check_true(last_updated >= 0, "the joined line is reported as updated");
	cut_text(expect, sizeof(expect), "Hello\nWorld\nthird", 5, "");
	check_str(save_joined(p_ed, "the round trip of the join"), expect, "the round trip of the join");
	check_model(p_ed, "the join");
	editor_data_cleanup(p_ed);

	// Ctrl-K removes the rest of the display line, line feed included, and pulls the next
	// line up into the block that was left behind
	p_ed = editor_data_load("one\ntwo\nthree");
	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 1);
	check_int(ret, 4, "editor_data_delete() of the whole first line");
	check_str(dl_text(p_ed, 0), "two\n", "the line behind is pulled up over the empty head");
	check_int(dl_total(p_ed), 2, "the whole line delete drops one display line");
	check_int(data_lines(p_ed), 2, "the whole line delete releases one data line");
	check_str(save_joined(p_ed, "the round trip of the whole line delete"), "two\nthree",
			"the round trip of the whole line delete");
	check_model(p_ed, "the whole line delete");
	editor_data_cleanup(p_ed);

	// Ctrl-K from the middle of a line keeps what is in front of the cursor
	p_ed = editor_data_load("one\ntwo\nthree");
	display_line = 0;
	offset = 2;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 1);
	check_int(ret, 2, "editor_data_delete() of the rest of a line");
	check_str(dl_text(p_ed, 0), "ontwo\n", "the head of the line is kept and the next line joins it");
	check_str(save_joined(p_ed, "the round trip of the rest of the line"), "ontwo\nthree",
			"the round trip of the rest of the line");
	check_model(p_ed, "the rest of the line");
	editor_data_cleanup(p_ed);

	// Ctrl-Y at the last line of the document empties it, since there is nothing to join
	p_ed = editor_data_load("one\ntwo\nthree");
	display_line = 2;
	offset = 0;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 1);
	check_int(ret, 5, "editor_data_delete() of the last line of the document");
	check_int(dl_len(p_ed, 2), 0, "the last display line is emptied");
	check_int(dl_total(p_ed), 3, "the last line keeps its display line");
	check_int(data_lines(p_ed), 3, "the emptied line still owns its data line");
	check_str(save_joined(p_ed, "the round trip of the emptied last line"), "one\ntwo\n",
			"the round trip of the emptied last line");
	check_model(p_ed, "the emptied last line");
	editor_data_cleanup(p_ed);

	// Deleting the line feed of an empty line pulls the line behind it up into that line
	p_ed = editor_data_load("\nabc\ndef\n");
	check_int(dl_total(p_ed), 4, "editor_data_load() of empty and full lines");

	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 0);
	check_int(ret, 1, "editor_data_delete() of the line feed of an empty line");
	check_str(dl_text(p_ed, 0), "abc\n", "an empty line pulled up takes the text of the next one");
	check_int(dl_total(p_ed), 3, "the pull up drops one display line");
	check_str(save_joined(p_ed, "the round trip after pulling a line up"), "abc\ndef\n",
			"the round trip after pulling a line up");
	check_model(p_ed, "pulling a line up");
	editor_data_cleanup(p_ed);

	// Deleting the line feed of the last line of the text, where only the empty line behind
	// it goes away
	p_ed = editor_data_load("short\n");
	check_int(dl_total(p_ed), 2, "editor_data_load() of a line with a trailing break");

	display_line = 0;
	offset = 5;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 0);
	check_int(ret, 1, "editor_data_delete() of the trailing line feed of the text");
	check_str(dl_text(p_ed, 0), "short", "the last line loses its break");
	check_int(dl_total(p_ed), 1, "the display line the trailing break opened goes with it");
	check_int(data_lines(p_ed), 1, "the data line of the trailing break is released");
	check_str(save_joined(p_ed, "the round trip of the deleted trailing break"), "short",
			"the round trip of the deleted trailing break");
	check_model(p_ed, "the deleted trailing break");
	editor_data_cleanup(p_ed);

	// Deleting inside a line of more than one display line re-measures them
	p_ed = editor_data_load("012345678901234567890123456789");
	check_int(dl_total(p_ed), 1, "editor_data_load() of thirty characters for the delete");

	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 0);
	check_int(ret, 1, "editor_data_delete() of the first of thirty characters");
	check_int(dl_len(p_ed, 0), 29, "the length of the line after the delete");
	check_int(dl_width(p_ed, 0), 29, "the width of the line after the delete");
	check_str(dl_text(p_ed, 0), "12345678901234567890123456789", "the first character is gone");

	display_line = 0;
	offset = 20;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 1);
	check_int(ret, 9, "editor_data_delete() of the rest of a twenty nine character line");
	check_str(save_joined(p_ed, "the round trip of the rest of a twenty nine character line"),
			"12345678901234567890", "the round trip of the rest of a twenty nine character line");
	check_model(p_ed, "the rest of a twenty nine character line");
	editor_data_cleanup(p_ed);

	// A wrapped line: Ctrl-K removes the rest of the screen line and the text behind it moves
	// up into its display line, so the document keeps everything but those seventy characters
	fill_chars(text, 200, 'x');
	p_ed = editor_data_load(text);
	check_int(dl_total(p_ed), 3, "editor_data_load() of a 200 character line for the delete");

	display_line = 1;
	offset = 10;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 1);
	check_int(ret, 70, "editor_data_delete() of the rest of a wrapped display line");
	check_int(dl_len(p_ed, 0), 80, "the display line before the delete keeps its width");
	check_int(dl_len(p_ed, 1), 50, "the tail of the wrapped line moves up into its own display line");
	check_int(dl_total(p_ed), 2, "the wrapped delete drops one display line");
	fill_chars(expect, 130, 'x');
	check_str(save_joined(p_ed, "the round trip of the wrapped delete"), expect,
			"the round trip of the wrapped delete");
	check_model(p_ed, "the wrapped delete");
	editor_data_cleanup(p_ed);

	// A multi-byte character is deleted as one unit, not as one byte of it
	use_fixed_width(1);
	p_ed = editor_data_load("a中b");
	check_int(dl_len(p_ed, 0), 5, "editor_data_load() of mixed text for the wide delete");
	check_int(dl_width(p_ed, 0), 4, "the width of mixed text before the wide delete");

	display_line = 0;
	offset = 1;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 0);
	check_int(ret, 3, "editor_data_delete() removes all three bytes of a CJK character");
	check_str(save_joined(p_ed, "the round trip of the wide delete"), "ab", "the round trip of the wide delete");
	check_int(dl_width(p_ed, 0), 2, "the width after the wide delete");
	check_model(p_ed, "the wide delete");

	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 0);
	check_int(ret, 1, "editor_data_delete() of an ASCII character next to the wide one");
	check_str(save_joined(p_ed, "the round trip after deleting ASCII"), "b", "the round trip after deleting ASCII");
	check_model(p_ed, "after deleting ASCII");
	editor_data_cleanup(p_ed);
	restore_fixed_width();

	// Pulling a line up into a data line that has no room left for it is refused: the text
	// stays where it is instead of running past the block and eating the reserve that a split
	// of that line at its end needs
	fill_chars(text, MAX_EDITOR_DATA_LINE_LENGTH - 3, 'g');
	snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s", "\nef");
	p_ed = editor_data_load(text);
	check_true(p_ed != NULL, "editor_data_load() of a full line and a short one");
	check_int(dl_total(p_ed), 14, "a full line and a short one are fourteen display lines");
	check_int(data_lines(p_ed), 2, "a full line and a short one are two data lines");
	check_int(widest_data_line(p_ed), MAX_EDITOR_DATA_LINE_LENGTH - 2, "the full line fills its block up to the reserve");

	display_line = 12;
	offset = 61;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 0);
	check_int(ret, 0, "editor_data_delete() of a line feed that has no room to join");
	check_str(save_joined(p_ed, "the round trip of the refused join"), text, "the round trip of the refused join");
	check_int(dl_total(p_ed), 14, "the refused join keeps the display lines");
	check_model(p_ed, "the refused join");
	editor_data_cleanup(p_ed);

	// One character less is room enough, and the joined line still fits its block
	fill_chars(text, MAX_EDITOR_DATA_LINE_LENGTH - 4, 'g');
	snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s", "\nef");
	p_ed = editor_data_load(text);
	check_int(dl_total(p_ed), 14, "editor_data_load() of a nearly full line and a short one");
	check_int(data_lines(p_ed), 2, "a nearly full line and a short one are two data lines");

	display_line = 12;
	offset = 60;
	last_updated = 0;
	ret = editor_data_delete(p_ed, &display_line, &offset, &last_updated, 0);
	check_int(ret, 1, "editor_data_delete() of a line feed with room to join");
	cut_text(expect, sizeof(expect), text, 1020, "");
	check_str(save_joined(p_ed, "the round trip of the join into a full block"), expect,
			"the round trip of the join into a full block");
	check_int(dl_total(p_ed), 13, "the join into a full block drops one display line");
	check_int(data_lines(p_ed), 1, "the join into a full block releases one data line");
	check_int(widest_data_line(p_ed), MAX_EDITOR_DATA_LINE_LENGTH - 2, "the joined line keeps its reserve");
	check_model(p_ed, "the join into a full block");
	editor_data_cleanup(p_ed);

	printf("Completed testing #8: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_display_line_limit(void)
{
	static char text[MAX_EDITOR_DATA_LINES + 2];
	EDITOR_DATA *p_ed;
	long display_line;
	long offset;
	long last_updated;
	long i;
	long ret;

	printf("Testing #9 ...\n");

	// MAX_EDITOR_DATA_LINES display lines fill the model, one data line per line of the text,
	// and the splitter stops one line short of the size of its offset array
	for (i = 0; i < MAX_EDITOR_DATA_LINES; i++)
	{
		text[i] = '\n';
	}

	text[MAX_EDITOR_DATA_LINES] = '\0';
	p_ed = editor_data_load(text);
	check_true(p_ed != NULL, "editor_data_load() of the whole model");

	if (p_ed == NULL)
	{
		printf("Completed testing #9: %d of %d checks passed\n", check_count - failed_count, check_count);

		return;
	}

	check_int(dl_total(p_ed), MAX_EDITOR_DATA_LINES, "the model holds every line of the text");
	check_model(p_ed, "the whole model");
	check_str(save_joined(p_ed, "the round trip of the whole model"), text + 1,
			"the round trip of the whole model");

	// Splitting a line takes a data block and a display line, and with the model full the
	// insert is refused before either of them is taken
	display_line = 0;
	offset = 1;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, -2, "editor_data_insert() of a break with the model full");

	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "\r", 1, &last_updated);
	check_int(ret, -2, "editor_data_insert() of a break at the start of a line with the model full");
	check_int(dl_total(p_ed), MAX_EDITOR_DATA_LINES, "a refused break leaves the model alone");
	check_str(save_joined(p_ed, "the round trip of the whole model after a refused break"), text + 1,
			"the round trip of the whole model after a refused break");

	// Writing into the last display line of a full model is allowed, since it needs no new
	// display line: the empty line the trailing break opened is the one that takes it
	display_line = MAX_EDITOR_DATA_LINES - 1;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "x", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() into the last display line of the model");
	check_str(dl_text(p_ed, MAX_EDITOR_DATA_LINES - 1), "x", "the last display line of the model takes the character");
	check_int(dl_total(p_ed), MAX_EDITOR_DATA_LINES, "the model stays at its limit");
	check_model(p_ed, "after the insert into the last display line");

	// A character in front of the first break pushes one more display line out of the line,
	// and with no display line left to take it ends the line there and cuts the last one back
	// to the width that still fits its block
	display_line = 0;
	offset = 0;
	last_updated = 0;
	ret = editor_data_insert(p_ed, &display_line, &offset, "y", 1, &last_updated);
	check_int(ret, 0, "editor_data_insert() into the first line of the full model");
	check_int(dl_total(p_ed), MAX_EDITOR_DATA_LINES, "the model is still at its limit");
	check_str(dl_text(p_ed, 0), "y\n", "the first display line keeps its text and its break");
	check_int(dl_len(p_ed, MAX_EDITOR_DATA_LINES - 1), 1, "the last display line is not pushed out of the model");
	check_model(p_ed, "after the inserts of the full model");

	// Saving a model at its limit must not run past the buffer the caller owns
	save_joined(p_ed, "the full model after the inserts of the limit test");
	check_true(p_ed != NULL && p_ed->display_line_total == MAX_EDITOR_DATA_LINES, "the model is still at its limit");

	editor_data_cleanup(p_ed);

	// The document opens again after all those data lines were released, which is what proves
	// that cleanup handed every block back to the pool
	p_ed = editor_data_load("back again\n");
	check_true(p_ed != NULL, "editor_data_load() after the full model was released");
	check_model(p_ed, "back again");
	editor_data_cleanup(p_ed);

	printf("Completed testing #9: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_display_typing(void)
{
	char text[MAX_EDITOR_DATA_LINE_LENGTH * 2];
	char expect[MAX_EDITOR_DATA_LINE_LENGTH * 3];
	EDITOR_DATA *p_ed;
	int ret;

	printf("Testing #10 ...\n");

	// Typing is one batch of keys between two status lines, so the cursor the editor leaves
	// behind only shows up in the moveto of the next status line. Key 0 asks for that line
	// without changing anything.
	p_ed = editor_data_load("");
	check_true(p_ed != NULL, "editor_data_load() for the typing runs");

	reset_keys();
	reset_screen_log();
	push_keys("Hi there");
	push_key(0);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() returns the key that left it");
	check_str(save_joined(p_ed, "the text typed into an empty document"), "Hi there",
			"the text typed into an empty document");
	check_int(moveto_row, 1, "the cursor of a one line document sits on the first row");
	check_int(moveto_col, 9, "the cursor sits behind the typed text");
	check_true(prints_count > 0, "editor_display() prints the document");
	check_true(clrtoeol_count > 0, "editor_display() clears each row before printing it");
	check_true(clrline_count > 0, "editor_display() clears the rows it draws on");
	check_true(iflush_count > 0, "editor_display() flushes the screen");
	check_true(online_update_count > 0, "editor_display() keeps the user record updated");
	check_model(p_ed, "after typing");
	editor_data_cleanup(p_ed);

	// The end of the document, then typing there
	p_ed = editor_data_load("one\ntwo");
	reset_keys();
	push_key(KEY_END);
	push_keys("!x");
	push_key(0);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() typing at the end of the document");
	check_str(save_joined(p_ed, "the text typed at the end of the document"), "one\ntwo!x",
			"the text typed at the end of the document");
	check_int(moveto_row, 2, "the cursor row at the end of the document");
	check_int(moveto_col, 6, "the cursor column behind the text typed at the end");
	check_model(p_ed, "typing at the end of the document");
	editor_data_cleanup(p_ed);

	// Ctrl-E only goes as far as the end of the line the cursor is on
	p_ed = editor_data_load("one\ntwo");
	reset_keys();
	push_key(Ctrl('E'));
	push_keys("!x");
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() typing after Ctrl-E");
	check_str(save_joined(p_ed, "the text typed after Ctrl-E"), "one!x\ntwo",
			"Ctrl-E keeps the cursor on the line it started from");
	check_model(p_ed, "after Ctrl-E typing");
	editor_data_cleanup(p_ed);

	// A tab asks for the next tab stop, which the editor fills with spaces
	p_ed = editor_data_load("ab");
	reset_keys();
	push_key('\t');
	push_key('x');
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() typing a tab at the start of a line");
	check_str(save_joined(p_ed, "the text typed with a tab"), "    xab", "a tab asks for the next tab stop");
	check_model(p_ed, "after a tab");

	// Enter opens a line and the text goes on there
	reset_keys();
	push_key(KEY_END);
	push_key(CR);
	push_keys("cd");
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() typing a break");
	check_str(save_joined(p_ed, "the text typed after a break"), "    xab\ncd", "a break opens a new line");
	check_model(p_ed, "after a break");

	// Backspace walks back over the text, and a third one takes the break with it
	editor_data_cleanup(p_ed);
	p_ed = editor_data_load("ab\ncd");
	reset_keys();
	push_key(KEY_END);
	push_key(BACKSPACE);
	push_key(BACKSPACE);
	push_key('Z');
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() typing after two backspaces");
	check_str(save_joined(p_ed, "the text after two backspaces"), "ab\nZ", "the text after two backspaces");
	check_model(p_ed, "after two backspaces");
	editor_data_cleanup(p_ed);

	p_ed = editor_data_load("ab\ncd");
	reset_keys();
	push_key(KEY_END);
	push_key(BACKSPACE);
	push_key(BACKSPACE);
	push_key(BACKSPACE);
	push_key('Z');
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() typing after a third backspace");
	check_str(save_joined(p_ed, "the text after the third backspace"), "abZ",
			"the third backspace takes the break and joins the lines");
	check_int(dl_total(p_ed), 1, "the joined lines are one display line");
	check_model(p_ed, "after the third backspace");
	editor_data_cleanup(p_ed);

	// Backspace at the very start of the document is refused
	p_ed = editor_data_load("ab\ncd");
	reset_keys();
	push_key(KEY_HOME);
	push_key(BACKSPACE);
	push_key(BACKSPACE);
	push_key('Q');
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with backspace at the document start");
	check_str(save_joined(p_ed, "the text of backspace at the document start"), "Qab\ncd",
			"backspace at the document start deletes nothing");
	check_model(p_ed, "backspace at the document start");
	editor_data_cleanup(p_ed);

	// Del removes the character behind the cursor
	p_ed = editor_data_load("ab\ncd");
	reset_keys();
	push_key(KEY_HOME);
	push_key(KEY_DEL);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with Del");
	check_str(save_joined(p_ed, "the text after Del"), "b\ncd", "the text after Del");
	check_model(p_ed, "after Del");

	// Ctrl-K removes the rest of the line and Ctrl-Y the whole line
	reset_keys();
	push_key(KEY_HOME);
	push_key(Ctrl('K'));
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with Ctrl-K");
	check_str(save_joined(p_ed, "the text after Ctrl-K"), "cd", "Ctrl-K takes the rest of the line with it");
	check_model(p_ed, "after Ctrl-K");

	reset_keys();
	push_key(KEY_HOME);
	push_key(Ctrl('Y'));
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with Ctrl-Y");
	check_str(save_joined(p_ed, "the text after Ctrl-Y"), "", "Ctrl-Y on the last line leaves an empty document");
	check_model(p_ed, "after Ctrl-Y");
	editor_data_cleanup(p_ed);

	// Insert toggles overwrite mode: the character under the cursor goes away with the one
	// that replaces it, and the status line names the mode
	p_ed = editor_data_load("abcd");
	reset_keys();
	push_key(KEY_RIGHT);
	push_key(KEY_INS);
	push_key('X');
	push_key(0);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() typing in overwrite mode");
	check_str(save_joined(p_ed, "the text typed in overwrite mode"), "aXcd", "the text typed in overwrite mode");
	check_int(moveto_col, 3, "the cursor moves one column past the overwritten character");
	check_true(strstr(printed_last, "替换") != NULL, "the status line names the overwrite mode");
	check_model(p_ed, "overwrite typing");
	editor_data_cleanup(p_ed);

	// A multi-byte character arrives as its bytes and is stored as one character
	use_fixed_width(1);
	p_ed = editor_data_load("a");
	reset_keys();
	push_key(Ctrl('E'));
	push_key(0xe4);
	push_key(0xb8);
	push_key(0xad);
	push_key(0);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() typing a CJK character");
	check_str(save_joined(p_ed, "the text typed as a CJK character"), "a中",
			"the bytes of one CJK character are stored as one");
	check_int(dl_width(p_ed, 0), 3, "the width after a CJK character");
	check_int(moveto_col, 4, "the cursor moves two columns for a CJK character");
	check_model(p_ed, "after a CJK character");

	// Half a character is dropped, and the timeout that ended it leaves the editor
	reset_keys();
	push_key(Ctrl('E'));
	push_key(0xe4);
	push_key(KEY_TIMEOUT);
	push_key('b');
	ret = editor_display(p_ed);
	check_int(ret, KEY_TIMEOUT, "editor_display() returns on the timeout of an incomplete character");
	check_str(save_joined(p_ed, "the text after an incomplete CJK character"), "a中",
			"an incomplete character is not stored");
	check_model(p_ed, "after an incomplete CJK character");
	restore_fixed_width();
	editor_data_cleanup(p_ed);

	// Typing at the end of a document whose line does not fit one data block: the editor has
	// to break the line where the block ran out, which is where the text gains a line feed it
	// did not have
	fill_chars(text, MAX_EDITOR_DATA_LINE_LENGTH + 7, 'a');
	p_ed = editor_data_load(text);
	check_true(p_ed != NULL, "editor_data_load() of a line past one block for typing");
	check_int(data_lines(p_ed), 2, "a line past one block takes two data lines");

	reset_keys();
	push_key(KEY_END);
	push_keys("tail");
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() typing at the end of a line past one block");
	snprintf(expect, sizeof(expect), "%s\ntail", text);
	check_str(save_joined(p_ed, "the text typed at the end of a line past one block"), expect,
			"the split of a full data block breaks the line where it ran out");
	check_model(p_ed, "after typing at the end of a line past one block");

	reset_keys();
	push_key(KEY_END);
	push_key(CR);
	push_keys("sec");
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() breaking a line past one block");
	snprintf(expect, sizeof(expect), "%s\ntail\nsec", text);
	check_str(save_joined(p_ed, "the text after breaking a line past one block"), expect,
			"the text after breaking a line past one block");
	check_model(p_ed, "after breaking a line past one block");

	// Backspace at the start of such a document deletes nothing
	reset_keys();
	push_key(KEY_HOME);
	push_key(BACKSPACE);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with backspace in a line past one block");
	check_model(p_ed, "after backspace in a line past one block");
	editor_data_cleanup(p_ed);

	// An empty key queue is the user stopping to type: the editor returns KEY_NULL and leaves
	// the document exactly as it was
	p_ed = editor_data_load("idle");
	reset_keys();
	reset_screen_log();
	ret = editor_display(p_ed);
	check_int(ret, KEY_NULL, "editor_display() returns KEY_NULL when the input ends");
	check_str(save_joined(p_ed, "the text of an idle editor"), "idle", "the text of an idle editor");
	check_model(p_ed, "idle");

	// A timeout is reported the same way, and Ctrl-W and Ctrl-X both leave
	reset_keys();
	push_key(KEY_TIMEOUT);
	ret = editor_display(p_ed);
	check_int(ret, KEY_TIMEOUT, "editor_display() returns the timeout it was given");

	reset_keys();
	push_key(Ctrl('X'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('X'), "editor_display() leaves on Ctrl-X");
	editor_data_cleanup(p_ed);

	// When the server starts to shut down the editor must not sit in its loop
	p_ed = editor_data_load("exit");
	reset_keys();
	push_keys("nope");
	SYS_server_exit = 1;
	ret = editor_display(p_ed);
	check_int(ret, 0, "editor_display() returns before reading a key when the server exits");
	SYS_server_exit = 0;
	check_str(save_joined(p_ed, "the text of an exited editor session"), "exit",
			"nothing is typed while the server exits");
	editor_data_cleanup(p_ed);

	printf("Completed testing #10: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_display_navigation(void)
{
	char text[(SCREEN_ROWS + 12) * (SCREEN_COLS + 1)];
	char *p_pos;
	EDITOR_DATA *p_ed;
	long i;
	int ret;

	printf("Testing #11 ...\n");

	// A document taller than the screen, which the editor scrolls as the cursor moves
	p_pos = text;

	for (i = 0; i < SCREEN_ROWS + 10; i++)
	{
		p_pos += snprintf(p_pos, (size_t)(text + sizeof(text) - p_pos), "line %02ld\n", i);
	}

	*p_pos = '\0';
	p_ed = editor_data_load(text);
	check_true(p_ed != NULL, "editor_data_load() of the scrolling document");
	check_int(dl_total(p_ed), SCREEN_ROWS + 11, "the trailing line feed opens one more display line");

	reset_keys();
	reset_screen_log();
	push_key(KEY_DOWN);
	push_key(KEY_DOWN);
	push_key(KEY_DOWN);
	push_key(0);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with three down keys");
	check_int(moveto_row, 4, "the cursor row after three down keys");
	check_int(moveto_col, 1, "the cursor column after three down keys");
	check_str(save_joined(p_ed, "the scrolling document is not changed by navigation"), text,
			"the scrolling document is not changed by navigation");

	// The page keys move a screen full at a time and stop at both ends of the document
	reset_keys();
	push_key(KEY_PGDN);
	push_key(KEY_PGDN);
	push_key(KEY_PGUP);
	push_key(0);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with page keys");
	check_true(moveto_row >= 1 && moveto_row < SCREEN_ROWS, "the cursor row after the page keys sits on the screen");
	check_str(save_joined(p_ed, "the scrolling document after the page keys"), text,
			"the scrolling document after the page keys");

	reset_keys();
	push_key(KEY_HOME);
	push_key(KEY_PGUP);
	push_key(KEY_PGUP);
	push_key(0);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with page keys at the document start");
	check_int(moveto_row, 1, "the cursor of the document start after paging up");
	check_int(moveto_col, 1, "the column of the document start after paging up");

	reset_keys();
	push_key(KEY_END);
	push_key(KEY_PGDN);
	push_key(KEY_PGDN);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with page keys at the document end");
	check_str(save_joined(p_ed, "the scrolling document after paging to the end"), text,
			"the scrolling document after paging to the end");
	check_model(p_ed, "the scrolling document");

	// Ctrl-T and Ctrl-B move to the top and the bottom row of the screen
	reset_keys();
	push_key(KEY_END);
	push_key(Ctrl('T'));
	push_key(Ctrl('B'));
	push_key(Ctrl('T'));
	push_key(0);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with Ctrl-T and Ctrl-B");
	check_int(moveto_row, 1, "Ctrl-T leaves the cursor on the first row of the screen");
	check_str(save_joined(p_ed, "the scrolling document after Ctrl-T and Ctrl-B"), text,
			"the scrolling document after Ctrl-T and Ctrl-B");
	editor_data_cleanup(p_ed);

	// The arrows walk the display lines of a wrapped line
	p_ed = editor_data_load("0123456789");
	check_int(dl_total(p_ed), 1, "editor_data_load() of ten digits for the arrows");

	reset_keys();
	push_key(Ctrl('E'));
	push_key(KEY_LEFT);
	push_key(KEY_RIGHT);
	push_key(Ctrl('A'));
	push_key(Ctrl('E'));
	push_key(KEY_UP);
	push_key(KEY_DOWN);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with the arrow keys");
	check_str(save_joined(p_ed, "the ten digits are not changed by the arrows"), "0123456789",
			"the ten digits are not changed by the arrows");
	check_model(p_ed, "after the arrows");

	// The Ctrl-arrow keys go to the start and the end of a line
	reset_keys();
	push_key(KEY_CTRL_RIGHT);
	push_key(KEY_CTRL_LEFT);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with the Ctrl-arrow keys");
	check_str(save_joined(p_ed, "the ten digits are not changed by the Ctrl-arrows"), "0123456789",
			"the ten digits are not changed by the Ctrl-arrows");

	// The help screen is asked for by both F1 and Ctrl-Q, and the refresh key redraws the text
	reset_keys();
	push_key(KEY_F1);
	push_key(KEY_F5);
	push_key(Ctrl('Q'));
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with the help and refresh keys");
	check_int(display_file_count, 2, "F1 and Ctrl-Q each read the help screen");
	check_str(display_file_name, DATA_EDITOR_HELP, "the help screen asked for the editor help file");
	check_model(p_ed, "after the help keys");

	// A key the editor does not know is ignored
	reset_keys();
	push_key(KEY_F12);
	push_key('q');
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with an unknown key");
	check_str(save_joined(p_ed, "the text after an unknown key"), "q0123456789",
			"the key after an unknown one is still typed");
	editor_data_cleanup(p_ed);

	// A wrapped line: the arrows walk its display lines and nothing is written
	fill_chars(text, 200, 'w');
	p_ed = editor_data_load(text);
	check_int(dl_total(p_ed), 3, "editor_data_load() of a 200 character line for the arrows");

	reset_keys();
	push_key(KEY_DOWN);
	push_key(KEY_DOWN);
	push_key(KEY_RIGHT);
	push_key(KEY_UP);
	push_key(KEY_LEFT);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() with arrows on a wrapped line");
	check_str(save_joined(p_ed, "the 200 character line is not changed by the arrows"), text,
			"the 200 character line is not changed by the arrows");
	check_model(p_ed, "arrows on a wrapped line");
	editor_data_cleanup(p_ed);

	// An empty document: every cursor key and delete key is refused, the model stays one line
	p_ed = editor_data_load("");
	reset_keys();
	push_key(Ctrl('E'));
	push_key(KEY_DOWN);
	push_key(KEY_UP);
	push_key(KEY_LEFT);
	push_key(KEY_RIGHT);
	push_key(BACKSPACE);
	push_key(KEY_DEL);
	push_key(Ctrl('K'));
	push_key(Ctrl('Y'));
	push_key(KEY_HOME);
	push_key(KEY_END);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() of an empty document");
	check_str(save_joined(p_ed, "the empty document stays empty"), "", "the empty document stays empty");
	check_model(p_ed, "an empty document");

	// The bottom line of the editor says where the cursor is, counts the lines and names the
	// modes the user is in
	reset_screen_log();
	reset_keys();
	push_keys("ab");
	push_key(0);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() typing into the empty document");
	check_str(save_joined(p_ed, "the text typed into the empty document"), "ab",
			"the text typed into the empty document");
	check_true(strstr(printed_last, "第") != NULL, "the status line counts the lines");
	check_true(strstr(printed_last, "插入") != NULL, "the status line names the insert mode");
	check_true(strstr(printed_last, "定宽") != NULL, "the status line names the fixed width mode");
	check_true(strstr(printed_last, "Ctrl-W") != NULL, "the status line offers Ctrl-W as the exit key");
	check_true(strstr(printed_last, "\033[32m1\033[33m;\033[32m3\033[33m]") != NULL,
			"the status line reports the cursor at the first row, third column");
	check_model(p_ed, "after typing into the empty document");

	// Variable width mode is what the status line reports when the terminal asks for it
	use_fixed_width(0);
	reset_keys();
	push_key(0);
	push_key(Ctrl('W'));
	ret = editor_display(p_ed);
	check_int(ret, Ctrl('W'), "editor_display() of the variable width document");
	check_true(strstr(printed_last, "变宽") != NULL, "the status line names the variable width mode");
	restore_fixed_width();
	editor_data_cleanup(p_ed);

	printf("Completed testing #11: %d of %d checks passed\n", check_count - failed_count, check_count);
}

int main(void)
{
	log_begin("../log/test_editor.log", "../log/test_editor_error.log");
	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	if (setlocale(LC_ALL, "en_US.UTF-8") == NULL)
	{
		printf("WARNING: setlocale(LC_ALL, en_US.UTF-8) failed, wide character tests may differ\n");
	}

	test_pool_lifecycle();
	test_load_save();
	test_wrapping();
	test_insert_in_place();
	test_insert_line_break();
	test_insert_over_length();
	test_data_line_capacity();
	test_delete();
	test_display_line_limit();
	test_display_typing();
	test_display_navigation();

	// Every document of the suite is closed above, so this releases the pools themselves and
	// LeakSanitizer only has the log files left to complain about
	editor_memory_pool_cleanup();

	if (failed_count > 0)
	{
		printf("Failed: %d of %d checks\n", failed_count, check_count);

		return -1;
	}

	printf("Done\n");

	return 0;
}
