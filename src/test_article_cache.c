/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_article_cache
 *   - tester for the article cache file: its header, its line index and its data
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "article_cache.h"
#include "bbs.h"
#include "common.h"
#include "lml.h"
#include "log.h"
#include "section_list.h"
#include "str_process.h"
#include "trie_dict.h"
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void check_bytes(const char *actual, const char *expected, size_t len, const char *what)
{
	check_count++;

	if (len > 0 && memcmp(actual, expected, len) != 0)
	{
		failed_count++;

		// An article runs to a hundred thousand characters, so a mismatch shows the size and
		// the head of both sides instead of filling the log with the whole document
		printf("FAILED: %s differs in %zu bytes: [%.*s] against [%.*s]\n",
			   what, len, 40, actual, 40, expected);
	}
}

// src/bbs.c is not linked in: article_cache.c reaches the site configuration only through
// these two names, which the suite fixes so the header and the footer can be predicted
char BBS_name[BBS_name_max_len + 1] = "TestSite";
char BBS_server[BBS_server_max_len + 1] = "test.example.com";

enum _test_article_cache_constant_t
{
	SUB_DT = 1700000000,

	META_BUF_LEN = 4096,
	CACHE_DOC_BUF_LEN = 1 << 18, // the longest body the suite builds
	FILE_BUF_LEN = 1 << 20,      // the index and the data of that body

	NO_SPAN = (long)-1,

	// The render breaks the body at the width of the screen, and the index counts the line
	// feed of a data line as the column it is drawn in, so the body has one more to spare
	HEADER_MAX_COLS = SCREEN_COLS,
	BODY_MAX_COLS = SCREEN_COLS + 1,
	FOOTER_MAX_COLS = SCREEN_COLS,

	REGION_HEADER = 0,
	REGION_BODY,
	REGION_FOOTER,
	REGION_COUNT,
};

static const char CACHE_DIR[] = "~test_article_cache";
static const char CACHE_DIR_PATH[] = "~test_article_cache/";
static const char MISSING_DIR_PATH[] = "~test_article_cache_nowhere/";
static const char TRIE_SHM_FILE[] = "~test_article_cache_trie.dat";
static const char TEST_SUB_IP[] = "1.2.3.4";

// The footer cycles through these seven colours with the article number
static const char *const footer_colors[] = {
	"\033[31m",
	"\033[32m",
	"\033[33m",
	"\033[34m",
	"\033[35m",
	"\033[36m",
	"\033[37m",
};
static const int footer_color_count = (int)(sizeof(footer_colors) / sizeof(const char *));

// The struct is half a megabyte, so none of it is left to the stack
static ARTICLE_CACHE disk;
static ARTICLE_CACHE loaded;
static SECTION_LIST section;

static char raw_file[FILE_BUF_LEN];
static long raw_len = 0;
static const char *disk_data = NULL;

static char exp_header[META_BUF_LEN];
static char exp_footer[META_BUF_LEN];
static char *exp_body = NULL;
static long exp_body_len = 0;

static long spans_of_region[MAX_SPLIT_FILE_LINES];
static long exp_offsets[MAX_SPLIT_FILE_LINES + 1];

static long region_start[REGION_COUNT];
static long region_lines[REGION_COUNT];

static char cache_file[FILE_PATH_LEN];

static void make_article(ARTICLE *p_a, int cid, int aid, const char *username, const char *nickname, const char *title)
{
	memset(p_a, 0, sizeof(*p_a));

	p_a->aid = (int32_t)aid;
	p_a->cid = (int32_t)cid;
	p_a->sid = 1;
	p_a->sub_dt = (time_t)SUB_DT;

	strncpy(p_a->username, username, sizeof(p_a->username) - 1);
	strncpy(p_a->nickname, nickname, sizeof(p_a->nickname) - 1);
	strncpy(p_a->title, title, sizeof(p_a->title) - 1);
}

static void make_section(void)
{
	memset(&section, 0, sizeof(section));

	section.sid = 1;
	strncpy(section.sname, "test", sizeof(section.sname) - 1);
	strncpy(section.stitle, "Test section", sizeof(section.stitle) - 1);
}

static const char *cache_path_of(const ARTICLE *p_a)
{
	snprintf(cache_file, sizeof(cache_file), "%s%d", CACHE_DIR_PATH, p_a->cid);

	return cache_file;
}

static long file_size(const char *path)
{
	struct stat sb;

	if (stat(path, &sb) < 0)
	{
		return -1;
	}

	return (long)sb.st_size;
}

// The cache file holds the beginning of the struct in memory, so the index starts where the
// offsets start there and the data starts behind the lines of the document
static long index_at(void)
{
	return (long)offsetof(ARTICLE_CACHE, line_offsets);
}

static long prefix_len(long line_total)
{
	return index_at() + (long)sizeof(long) * (line_total + 1);
}

// Read a cache file back and take its header apart. Everything the suite compares after this
// comes from the bytes on disk, so an index that does not describe its own data cannot be
// excused by an expectation that was read out of the same place.
static int read_cache(const ARTICLE *p_a)
{
	FILE *fp;
	long size;

	disk_data = NULL;
	memset(&disk, 0, sizeof(disk));

	size = file_size(cache_path_of(p_a));
	if (size < index_at() || size > FILE_BUF_LEN)
	{
		raw_len = 0;

		return -1;
	}

	fp = fopen(cache_file, "rb");
	if (fp == NULL)
	{
		raw_len = 0;

		return -1;
	}

	raw_len = (long)fread(raw_file, 1, (size_t)size, fp);
	fclose(fp);

	if (raw_len != size)
	{
		raw_len = 0;

		return -1;
	}

	memcpy(&disk, raw_file, (size_t)index_at());

	if (disk.line_total < 0 || disk.line_total >= MAX_SPLIT_FILE_LINES ||
		prefix_len(disk.line_total) > raw_len)
	{
		raw_len = 0;

		return -1;
	}

	memcpy(disk.line_offsets, raw_file + index_at(),
		   sizeof(long) * (size_t)(disk.line_total + 1));

	disk_data = raw_file + prefix_len(disk.line_total);

	return 0;
}

// The time as the cache writes it: taken from the same call, so another timezone or another
// locale moves the expectation along with the document
static const char *sub_dt_str(time_t sub_dt)
{
	static char str[META_BUF_LEN];
	struct tm tm_sub_dt;

	localtime_r(&sub_dt, &tm_sub_dt);

	if (strftime(str, sizeof(str), "%c %Z", &tm_sub_dt) == 0)
	{
		str[0] = '\0';
	}

	return str;
}

static void build_header(const ARTICLE *p_a)
{
	snprintf(exp_header, sizeof(exp_header),
			 "发布者: %s (%s), 版块: %s (%s)\n标  题: %s\n发布于: %s (%s)\n\n",
			 p_a->username, p_a->nickname, section.sname, section.stitle, p_a->title,
			 BBS_name, sub_dt_str(p_a->sub_dt));
}

static void build_footer(int aid, const char *sub_ip)
{
	snprintf(exp_footer, sizeof(exp_footer),
			 "\n--\n%s※ 来源: %s https://%s [FROM: %s]\033[m\n\n",
			 footer_colors[aid % footer_color_count], BBS_name, BBS_server, sub_ip);
}

// The body of the cache is what the render makes of the article text
static int build_body(const char *content)
{
	size_t buf_len = strlen(content) * 4 + META_BUF_LEN;
	int rendered;

	exp_body = realloc(exp_body, buf_len);
	if (exp_body == NULL)
	{
		check_true(0, "memory for the expected body");

		return -1;
	}

	rendered = lml_render(content, exp_body, (int)buf_len, SCREEN_COLS, 0);
	if (rendered < 0)
	{
		check_true(0, "lml_render() of the expected body");
		exp_body[0] = '\0';
		exp_body_len = 0;

		return -1;
	}

	exp_body_len = (long)rendered;

	// The render stops without terminating its buffer once the output reaches the limit, and
	// the suite would be left without anything to compare against
	check_true(rendered < (int)buf_len - 1, "the expected body fits its buffer");

	return 0;
}

// Bytes and columns of the character at the front of a run of text, as a terminal draws it:
// an escape sequence costs nothing, a multi-byte character costs two columns, a line feed
// costs the one column it is drawn in, a carriage return or a bell is carried along for free
struct char_unit
{
	long bytes;
	long cols;
};

static struct char_unit unit_at(const char *p, long len, int skip_ctrl_seq)
{
	struct char_unit unit;
	unsigned char c;

	unit.bytes = 0;
	unit.cols = 0;

	if (len <= 0)
	{
		return unit;
	}

	c = (unsigned char)p[0];

	if (c == '\r' || c == '\a')
	{
		unit.bytes = 1;

		return unit;
	}

	if (c == '\033' && len > 1 && p[1] == '[' && skip_ctrl_seq)
	{
		long i = 2;

		while (i < len && p[i] != 'm')
		{
			i++;
		}

		unit.bytes = (i < len) ? i + 1 : len;

		return unit;
	}

	if (c >= 0x80)
	{
		long bytes = 1;

		if ((c & 0xf8) == 0xf0)
		{
			bytes = 4;
		}
		else if ((c & 0xf0) == 0xe0)
		{
			bytes = 3;
		}
		else if ((c & 0xe0) == 0xc0)
		{
			bytes = 2;
		}

		unit.bytes = (bytes < len) ? bytes : len;
		unit.cols = 2;

		return unit;
	}

	unit.bytes = 1;
	unit.cols = 1;

	return unit;
}

static long col_width(const char *p, long len, int skip_ctrl_seq)
{
	long width = 0;

	while (len > 0)
	{
		struct char_unit unit = unit_at(p, len, skip_ctrl_seq);

		if (unit.bytes <= 0)
		{
			break;
		}

		width += unit.cols;
		p += unit.bytes;
		len -= unit.bytes;
	}

	return width;
}

// The display lines of a text as a terminal reads them: a line ends on a line feed, which
// stays with the line, or where the next character would pass the width. A text that ends on
// a line feed has an empty line after it, the one the cursor rests on.
static long spans_of(const char *text, long len, int max_cols, int skip_ctrl_seq,
				   long *spans, long cap)
{
	long count = 0;
	long start = 0;

	while (1)
	{
		long i = start;
		long width = 0;
		int on_line_feed = 0;

		while (i < len)
		{
			struct char_unit unit = unit_at(text + i, len - i, skip_ctrl_seq);
			int is_line_feed = (unit.bytes == 1 && unit.cols == 1 && text[i] == '\n');

			if (unit.bytes <= 0)
			{
				break;
			}

			if (width + unit.cols > max_cols)
			{
				break;
			}

			width += unit.cols;
			i += unit.bytes;

			if (is_line_feed)
			{
				on_line_feed = 1;

				break;
			}
		}

		if (count >= cap)
		{
			return NO_SPAN;
		}

		spans[count++] = i - start;
		start = i;

		if (start >= len)
		{
			if (on_line_feed)
			{
				if (count >= cap)
				{
					return NO_SPAN;
				}

				spans[count++] = 0;
			}

			break;
		}
	}

	return count;
}

static long spans_copy(const long *src, long count, long *dest, long cap)
{
	long i;

	if (src == NULL || count < 0 || count > cap)
	{
		return NO_SPAN;
	}

	for (i = 0; i < count; i++)
	{
		dest[i] = src[i];
	}

	return count;
}

static long spans_sum(const long *spans, long at, long count)
{
	long i;
	long sum = 0;

	for (i = 0; i < count; i++)
	{
		sum += spans[at + i];
	}

	return sum;
}

static int region_of(long i)
{
	if (i >= region_start[REGION_FOOTER])
	{
		return REGION_FOOTER;
	}

	if (i >= region_start[REGION_BODY])
	{
		return REGION_BODY;
	}

	return REGION_HEADER;
}

static long region_last_index(int region)
{
	return region_start[region] + region_lines[region] - 1;
}

static void check_index(long header_len, long body_len, long footer_len)
{
	long line_total = region_lines[REGION_HEADER] + region_lines[REGION_BODY] + region_lines[REGION_FOOTER];
	long data_len = header_len + body_len + footer_len;
	long i;

	check_int(disk.line_total, line_total, "the lines the index holds");
	check_true((long)disk.mmap_len == raw_len, "the stored length is the length of the file");
	check_true((long)disk.data_len == data_len, "the stored data length is header, body and footer");
	check_true((long)disk.mmap_len == prefix_len(disk.line_total) + (long)disk.data_len,
			"the file is the header, the index and the data");

	// The two pointers of the struct in memory have no meaning inside a file
	check_true(disk.p_mmap == NULL, "the stored mapping pointer is empty");
	check_true(disk.p_data == NULL, "the stored data pointer is empty");

	if (disk_data == NULL)
	{
		check_true(0, "the data of the cache file");

		return;
	}

	check_bytes(disk_data, exp_header, (size_t)header_len, "the document starts with the header");
	check_bytes(disk_data + header_len, exp_body, (size_t)body_len, "the body follows the header");
	check_bytes(disk_data + header_len + body_len, exp_footer, (size_t)footer_len, "the footer follows the body");

	for (i = 0; i <= line_total; i++)
	{
		check_int(disk.line_offsets[i], exp_offsets[i], "an offset of the line index");

		if (disk.line_offsets[i] != exp_offsets[i])
		{
			// The rest of the index stands on this one, so stop instead of reporting a
			// cascade of failures for one slipped byte
			return;
		}
	}

	check_int(disk.line_offsets[0], 0, "the index starts at the first byte");
	check_int(disk.line_offsets[line_total], data_len, "the index ends at the length of the data");

	// What the index says about every line, read from the file rather than from the
	// expectation that just matched it: no line is wider than its region allows, a line that
	// does not end on a line feed filled its width, and no line feed sits inside a line
	for (i = 0; i < line_total; i++)
	{
		long at = disk.line_offsets[i];
		long end = disk.line_offsets[i + 1];
		int region = region_of(i);
		int max_cols;
		long width;
		int on_line_feed;

		if (region == REGION_HEADER)
		{
			max_cols = HEADER_MAX_COLS;
		}
		else if (region == REGION_BODY)
		{
			max_cols = BODY_MAX_COLS;
		}
		else
		{
			max_cols = FOOTER_MAX_COLS;
		}

		if (end < at || end > data_len)
		{
			check_true(0, "the index runs forward and stays inside the data");

			return;
		}

		width = col_width(disk_data + at, end - at, 1);
		on_line_feed = (end > at && disk_data[end - 1] == '\n');

		check_true(width <= max_cols, "a display line fits the width of its region");

		check_true(memchr(disk_data + at, '\n', (size_t)(end - at)) == NULL || on_line_feed,
				"a line feed only ends a display line");

		if (i != region_last_index(region))
		{
			long next_cols;

			check_true(end > at, "a display line inside a region has bytes of its own");

			if (!on_line_feed)
			{
				// The line was cut because the character behind it would no longer fit, which
				// is the only other way a display line can end before the document does
				next_cols = unit_at(disk_data + end, data_len - end, 1).cols;

				check_true(width + next_cols > max_cols,
						"a display line inside a region is broken where it no longer fits");
			}
		}
	}
}

// Write one article and check the file it leaves. The lines of the body can be stated by the
// test; where they are not, they are the lines this file's own reader of a terminal makes.
static void verify_cache(const ARTICLE *p_a, const char *content, const char *sub_ip,
						const long *body_span_list, long body_span_count)
{
	long header_len;
	long body_len;
	long footer_len;
	long lines[REGION_COUNT];
	long at[REGION_COUNT];
	long lengths[REGION_COUNT];
	const char *texts[REGION_COUNT];
	int max_cols[REGION_COUNT];
	long i;
	long total;
	long slot = 0;
	int ret;

	disk_data = NULL;

	build_header(p_a);
	build_footer(p_a->aid, sub_ip);

	if (build_body(content) < 0)
	{
		return;
	}

	header_len = (long)strlen(exp_header);
	body_len = exp_body_len;
	footer_len = (long)strlen(exp_footer);

	texts[REGION_HEADER] = exp_header;
	texts[REGION_BODY] = exp_body;
	texts[REGION_FOOTER] = exp_footer;

	lengths[REGION_HEADER] = header_len;
	lengths[REGION_BODY] = body_len;
	lengths[REGION_FOOTER] = footer_len;

	max_cols[REGION_HEADER] = HEADER_MAX_COLS;
	max_cols[REGION_BODY] = BODY_MAX_COLS;
	max_cols[REGION_FOOTER] = FOOTER_MAX_COLS;

	for (i = 0; i < REGION_COUNT; i++)
	{
		at[i] = slot;

		if (i == REGION_BODY && body_span_list != NULL)
		{
			lines[i] = spans_copy(body_span_list, body_span_count, spans_of_region + slot,
					MAX_SPLIT_FILE_LINES - slot);
		}
		else
		{
			lines[i] = spans_of(texts[i], lengths[i], max_cols[i], 1, spans_of_region + slot,
					MAX_SPLIT_FILE_LINES - slot);
		}

		if (lines[i] <= 0)
		{
			check_true(0, "a part of the document splits into lines");

			return;
		}

		check_int(spans_sum(spans_of_region, at[i], lines[i]), lengths[i],
				"the lines of a part of the document add up to it");

		slot += lines[i];
	}

	total = slot;

	exp_offsets[0] = 0;

	for (i = 0; i < total; i++)
	{
		exp_offsets[i + 1] = exp_offsets[i] + spans_of_region[i];
	}

	region_start[REGION_HEADER] = 0;
	region_start[REGION_BODY] = lines[REGION_HEADER];
	region_start[REGION_FOOTER] = lines[REGION_HEADER] + lines[REGION_BODY];

	region_lines[REGION_HEADER] = lines[REGION_HEADER];
	region_lines[REGION_BODY] = lines[REGION_BODY];
	region_lines[REGION_FOOTER] = lines[REGION_FOOTER];

	ret = article_cache_generate(CACHE_DIR_PATH, p_a, &section, content, sub_ip, 1);
	check_int(ret, 0, "article_cache_generate()");

	if (ret != 0)
	{
		return;
	}

	if (read_cache(p_a) < 0)
	{
		check_true(0, "the generated cache file can be read back");

		return;
	}

	check_index(header_len, body_len, footer_len);
}

static void check_document(const char *what, const ARTICLE *p_a, const char *content,
						const char *sub_ip, const long *body_span_list, long body_span_count)
{
	int failed_before = failed_count;

	verify_cache(p_a, content, sub_ip, body_span_list, body_span_count);

	if (failed_count > failed_before)
	{
		printf("in the case of \"%s\"\n", what);
	}
}

static void check_simple(const char *what, const char *content, int cid, int aid)
{
	ARTICLE article;

	make_article(&article, cid, aid, "tester", "Tester", "Hello");

	check_document(what, &article, content, TEST_SUB_IP, NULL, 0);
}

static void check_with_body_lines(const char *what, const char *content, int cid, int aid,
					const long *body_span_list, long body_span_count)
{
	ARTICLE article;

	make_article(&article, cid, aid, "tester", "Tester", "Hello");

	check_document(what, &article, content, TEST_SUB_IP, body_span_list, body_span_count);
}

static void test_contracts(void)
{
	ARTICLE article;
	int ret;

	printf("Testing #1 ...");

	make_section();
	make_article(&article, 100, 1, "tester", "Tester", "Hello");

	ret = article_cache_generate(NULL, &article, &section, "body", "1.2.3.4", 1);
	check_int(ret, -1, "article_cache_generate() without a cache directory");

	ret = article_cache_generate(CACHE_DIR_PATH, NULL, &section, "body", "1.2.3.4", 1);
	check_int(ret, -1, "article_cache_generate() without an article");

	ret = article_cache_generate(CACHE_DIR_PATH, &article, &section, NULL, "1.2.3.4", 1);
	check_int(ret, -1, "article_cache_generate() without content");

	ret = article_cache_generate(MISSING_DIR_PATH, &article, &section, "body", "1.2.3.4", 1);
	check_int(ret, -2, "article_cache_generate() into a directory that is not there");

	check_int(file_size(cache_path_of(&article)), -1, "a refused call leaves no cache file");

	ret = article_cache_load(NULL, CACHE_DIR_PATH, &article);
	check_int(ret, -1, "article_cache_load() without a struct to fill");

	ret = article_cache_load(&loaded, NULL, &article);
	check_int(ret, -1, "article_cache_load() without a cache directory");

	ret = article_cache_load(&loaded, CACHE_DIR_PATH, NULL);
	check_int(ret, -1, "article_cache_load() without an article");

	ret = article_cache_unload(NULL);
	check_int(ret, -1, "article_cache_unload() without a struct");

	// Nothing is mapped into this struct, so there is nothing to release
	memset(&loaded, 0, sizeof(loaded));
	ret = article_cache_unload(&loaded);
	check_int(ret, -1, "article_cache_unload() of a struct that holds no mapping");

	printf("Completed testing #1: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_simple_documents(void)
{
	long body_spans[8];

	printf("Testing #2 ...");

	make_section();

	// Two lines that end on their line feeds take a third, empty line behind them: the last
	// line feed of a document always leaves one the cursor rests on
	body_spans[0] = 11;
	body_spans[1] = 12;
	body_spans[2] = 0;
	check_with_body_lines("two lines", "First line\nSecond line\n", 101, 1, body_spans, 3);

	// A document that does not end on a line feed has one line and no empty one behind it
	body_spans[0] = 3;
	check_with_body_lines("no trailing line feed", "abc", 102, 2, body_spans, 1);

	// An empty body is still one line, of no bytes at all
	body_spans[0] = 0;
	check_with_body_lines("empty body", "", 103, 3, body_spans, 1);

	body_spans[0] = 1;
	body_spans[1] = 0;
	check_with_body_lines("one line feed", "\n", 104, 4, body_spans, 2);

	body_spans[0] = 4;
	body_spans[1] = 0;
	check_with_body_lines("spaces", "   \n", 105, 5, body_spans, 2);

	body_spans[0] = 1;
	body_spans[1] = 1;
	body_spans[2] = 1;
	body_spans[3] = 0;
	check_with_body_lines("blank lines", "\n\n\n", 106, 6, body_spans, 4);

	body_spans[0] = 8; // four letters, three spaces and the line feed that ends them
	body_spans[1] = 1;
	body_spans[2] = 0;
	check_with_body_lines("trailing spaces", "text   \n\n", 107, 7, body_spans, 3);

	body_spans[0] = 19; // six characters of three bytes and the line feed
	body_spans[1] = 0;
	check_with_body_lines("cjk", "中文测试一行\n", 108, 8, body_spans, 2);

	body_spans[0] = 4; // the render drops the carriage returns
	body_spans[1] = 4;
	body_spans[2] = 0;
	check_with_body_lines("crlf", "one\r\ntwo\r\n", 109, 9, body_spans, 3);

	body_spans[0] = 6;
	body_spans[1] = 0;
	check_with_body_lines("tab", "a\tb\n", 110, 10, body_spans, 2);

	// Left to this file's own reader of a terminal: what the render makes of control
	// characters is not something the index should be trusted to hide
	check_simple("bell and backspace", "a\bb\ac\n", 111, 11);
	check_simple("long subject", "012345678901234567890123456789, 这是一段中文的行\n", 112, 12);

	printf("Completed testing #2: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_wrapped_documents(void)
{
	static char content[CACHE_DOC_BUF_LEN];
	long body_spans[8];

	printf("Testing #3 ...");

	make_section();

	// A line of exactly the width of the screen keeps its line feed on the same display line,
	// because the render breaks the text one column before it runs out
	memset(content, 'x', SCREEN_COLS);
	content[SCREEN_COLS] = '\0';
	body_spans[0] = SCREEN_COLS;
	check_with_body_lines("a line of the screen width", content, 120, 1, body_spans, 1);

	// One column past it and the render breaks the line, which gives the line feed the
	// column of the whole display line it would otherwise have needed
	memset(content, 'x', SCREEN_COLS + 1);
	content[SCREEN_COLS + 1] = '\0';
	body_spans[0] = SCREEN_COLS + 1;
	body_spans[1] = 1;
	check_with_body_lines("a line past the screen width", content, 121, 2, body_spans, 2);

	// A line of longer than two screens breaks into whole lines first and one for the rest
	memset(content, 'y', 200);
	content[200] = '\0';
	body_spans[0] = 81;
	body_spans[1] = 81;
	body_spans[2] = 40;
	check_with_body_lines("a line of two screens", content, 122, 3, body_spans, 3);

	// Wide characters break as whole characters: 82 columns need three display lines, and
	// the line the render adds takes the last one
	for (long i = 0; i < 41; i++)
	{
		memcpy(content + i * 3, "中", 3);
	}

	content[123] = '\0';

	body_spans[0] = 121; // forty characters and the line feed the render puts behind them
	body_spans[1] = 3;
	check_with_body_lines("wide characters past the screen", content, 123, 4, body_spans, 2);

	// A subject long enough to wrap spreads the header over several display lines, which the
	// index has to count as part of the document
	{
		ARTICLE article;

		make_article(&article, 124, 5, "tester", "Tester",
				"01234567890123456789012345678901234567890123456789"
				"01234567890123456789012345678901234567890123456789"
				"01234567890123456789012345678901234567890123456789");

		check_document("a wrapping subject", &article, "body\n", TEST_SUB_IP, NULL, 0);
	}

	printf("Completed testing #3: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_rendered_documents(void)
{
	printf("Testing #4 ...");

	make_section();

	check_simple("a coloured line", "[color red]Red[/color] body\n", 130, 1);
	check_simple("a quoted line", ": quoted line\nplain line\n", 131, 2);
	check_simple("a nested quote", ": : deep quote\n", 132, 3);
	check_simple("a link", "[url http://test.example.com]click[/url]\n", 133, 4);
	check_simple("an unknown tag", "[notatag]text[/notatag]", 134, 5);
	check_simple("a literal quote", "[nolml][color red]red[/color]", 135, 6);
	check_simple("leading tabs", "\tx\t\ty\n", 136, 7);
	check_simple("a signature", "--\nnot a footer\n", 137, 8);

	printf("Completed testing #4: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_file_names_and_overwrite(void)
{
	ARTICLE article;
	char path[FILE_PATH_LEN];
	long before;
	long after;
	int ret;

	printf("Testing #5 ...");

	make_section();

	make_article(&article, 5001, 77, "tester", "Tester", "Named");

	ret = article_cache_generate(CACHE_DIR_PATH, &article, &section, "body\n", "1.2.3.4", 1);
	check_int(ret, 0, "article_cache_generate() an article to name");

	snprintf(path, sizeof(path), "%s%d", CACHE_DIR_PATH, article.cid);
	check_true(file_size(path) > 0, "the cache of an article is named by its content id");

	snprintf(path, sizeof(path), "%s%d", CACHE_DIR_PATH, article.aid);
	check_int(file_size(path), -1, "no cache is named by the article number");

	article.cid = 5002;

	ret = article_cache_generate(CACHE_DIR_PATH, &article, &section, "body\n", "1.2.3.4", 1);
	check_int(ret, 0, "article_cache_generate() the same article under another content id");

	snprintf(path, sizeof(path), "%s%d", CACHE_DIR_PATH, article.cid);
	check_true(file_size(path) > 0, "another content id is another file");

	// An existing cache is only rewritten when the caller asks for it
	before = file_size(path);

	ret = article_cache_generate(CACHE_DIR_PATH, &article, &section,
			"a longer body, with more lines than the one before it held\n", "1.2.3.4", 0);
	check_int(ret, 0, "article_cache_generate() over a cache that is already there");
	check_int(file_size(path), before, "a cache that is already there is left as it is");

	ret = article_cache_generate(CACHE_DIR_PATH, &article, &section,
			"a longer body, with more lines than the one before it held\n", "1.2.3.4", 1);
	check_int(ret, 0, "article_cache_generate() over a cache the caller wants rewritten");

	after = file_size(path);
	check_true(after != before, "a cache is rewritten when the caller asks for it");

	check_document("the rewritten cache", &article,
			"a longer body, with more lines than the one before it held\n", TEST_SUB_IP, NULL, 0);

	printf("Completed testing #5: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_footer_colors(void)
{
	char label[64];
	ARTICLE article;
	int aid;

	printf("Testing #6 ...");

	make_section();

	for (aid = 0; aid < 2 * footer_color_count; aid++)
	{
		make_article(&article, 600 + aid, aid, "tester", "Tester", "Colour");

		snprintf(label, sizeof(label), "the footer of article %d", aid);

		check_document(label, &article, "footer\n", TEST_SUB_IP, NULL, 0);

		if (disk_data != NULL)
		{
			// Behind the two lines that open the footer, which is where the cycle shows
			check_true(memcmp(disk_data + (long)disk.data_len - (long)strlen(exp_footer) + 4,
					footer_colors[aid % footer_color_count], 5) == 0,
					"the colour of the footer follows the article number");
		}
	}

	printf("Completed testing #6: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_footer_wrap(void)
{
	char chosen[64];
	char sub_ip[64];
	ARTICLE article;
	long counted;
	long kept = 0;
	long footer_len;
	long len;
	int aid = 9;
	int found = 0;

	printf("Testing #7 ...");

	make_section();
	make_article(&article, 7501, aid, "tester", "Tester", "Edge");

	// A source address long enough to bring the footer to the edge of the screen, where it
	// makes a difference that the colour codes of the render are drawn in no room at all
	for (len = 1; len < 40; len++)
	{
		memset(sub_ip, '9', (size_t)len);
		sub_ip[len] = '\0';

		build_footer(aid, sub_ip);
		footer_len = (long)strlen(exp_footer);

		kept = spans_of(exp_footer, footer_len, FOOTER_MAX_COLS, 1, spans_of_region,
				MAX_SPLIT_FILE_LINES);
		counted = spans_of(exp_footer, footer_len, FOOTER_MAX_COLS, 0, spans_of_region,
				MAX_SPLIT_FILE_LINES);

		if (kept > 0 && counted > kept)
		{
			snprintf(chosen, sizeof(chosen), "%s", sub_ip);

			found = 1;

			break;
		}
	}

	check_true(found != 0, "a source address that brings the footer to the edge of the screen");

	if (!found)
	{
		printf("Completed testing #7: %d of %d checks passed\n", check_count - failed_count, check_count);

		return;
	}

	check_document("a footer at the edge of the screen", &article, "short body\n", chosen, NULL, 0);

	printf("Completed testing #7: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_load_and_unload(void)
{
	ARTICLE article;
	long i;
	int ret;

	printf("Testing #8 ...");

	make_section();
	make_article(&article, 7001, 4, "tester", "Tester", "Loaded");

	// Written by the same module, so the reader is checked against the writer
	check_document("a document to load", &article, "First line\nSecond line\n", TEST_SUB_IP, NULL, 0);

	if (disk_data == NULL)
	{
		printf("Completed testing #8: %d of %d checks passed\n", check_count - failed_count, check_count);

		return;
	}

	memset(&loaded, 0, sizeof(loaded));

	ret = article_cache_load(&loaded, CACHE_DIR_PATH, &article);
	check_int(ret, 0, "article_cache_load()");

	check_true(loaded.p_mmap != NULL, "the loaded cache holds a mapping");
	check_true(loaded.p_data != NULL, "the loaded cache points at its data");
	check_true((long)loaded.mmap_len == (long)disk.mmap_len, "the loaded length is the length of the file");
	check_true(loaded.data_len == disk.data_len, "the loaded data length is the length in the file");
	check_int(loaded.line_total, disk.line_total, "the loaded number of lines");

	for (i = 0; i <= loaded.line_total; i++)
	{
		check_int(loaded.line_offsets[i], disk.line_offsets[i], "an offset of the loaded index");
	}

	check_bytes((const char *)loaded.p_data, disk_data, loaded.data_len, "the loaded data is the data of the file");

	check_true((char *)loaded.p_mmap + prefix_len(loaded.line_total) == loaded.p_data,
			"the data of the mapping follows its index");

	ret = article_cache_unload(&loaded);
	check_int(ret, 0, "article_cache_unload()");
	check_true(loaded.p_mmap == NULL, "the released struct holds no mapping");

	ret = article_cache_unload(&loaded);
	check_int(ret, -1, "article_cache_unload() of a struct that was released");

	printf("Completed testing #8: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void write_broken_cache(int cid, const char *bytes, long len)
{
	char path[FILE_PATH_LEN];
	FILE *fp;

	snprintf(path, sizeof(path), "%s%d", CACHE_DIR_PATH, cid);

	fp = fopen(path, "wb");
	if (fp == NULL)
	{
		check_true(0, "a cache file to corrupt");

		return;
	}

	if (len > 0 && fwrite(bytes, 1, (size_t)len, fp) != (size_t)len)
	{
		check_true(0, "the bytes of a cache file to corrupt");
	}

	fclose(fp);
}

static void test_unreadable_caches(void)
{
	ARTICLE article;
	char path[FILE_PATH_LEN];
	long whole;
	int ret;

	printf("Testing #9 ...");

	make_section();
	make_article(&article, 8001, 6, "tester", "Tester", "Broken");

	memset(&loaded, 0, sizeof(loaded));

	ret = article_cache_load(&loaded, CACHE_DIR_PATH, &article);
	check_int(ret, -2, "article_cache_load() of a cache that is not there");

	// A file the size of nothing cannot be mapped at all
	write_broken_cache(article.cid, "", 0);

	memset(&loaded, 0, sizeof(loaded));
	ret = article_cache_load(&loaded, CACHE_DIR_PATH, &article);
	check_int(ret, -2, "article_cache_load() of an empty cache file");
	check_true(loaded.p_mmap == NULL, "an empty cache file is not mapped");

	// Shorter than its own header: the length it claims is the nothing that follows it
	write_broken_cache(article.cid, "0123456789", 10);

	memset(&loaded, 0, sizeof(loaded));
	ret = article_cache_load(&loaded, CACHE_DIR_PATH, &article);
	check_int(ret, -3, "article_cache_load() of a cache file with no header");
	check_true(loaded.p_mmap == NULL, "a cache file with no header is not mapped");

	write_broken_cache(article.cid, "This is not an article cache.", 29);

	memset(&loaded, 0, sizeof(loaded));
	ret = article_cache_load(&loaded, CACHE_DIR_PATH, &article);
	check_int(ret, -3, "article_cache_load() of a cache file of prose");
	check_true(loaded.p_mmap == NULL, "a cache file of prose is not mapped");

	// A cache cut short keeps the header of the whole document, which no longer describes it
	check_document("a document to truncate", &article, "First line\nSecond line\n", TEST_SUB_IP, NULL, 0);

	snprintf(path, sizeof(path), "%s%d", CACHE_DIR_PATH, article.cid);

	whole = file_size(path);
	check_true(whole > 0, "the document to truncate");

	if (whole > 0 && truncate(path, (off_t)(whole / 2)) == 0)
	{
		memset(&loaded, 0, sizeof(loaded));
		ret = article_cache_load(&loaded, CACHE_DIR_PATH, &article);
		check_int(ret, -3, "article_cache_load() of a cache file cut in half");
		check_true(loaded.p_mmap == NULL, "a cache file cut in half is not mapped");
	}
	else
	{
		check_true(0, "truncate() of a cache file");
	}

	unlink(path);

	printf("Completed testing #9: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_index_limit(void)
{
	// One line of the document costs two bytes, and the index holds MAX_SPLIT_FILE_LINES of
	// them for the whole document: header, body and footer together
	static char content[2 * MAX_SPLIT_FILE_LINES];
	ARTICLE article;
	long header_len;
	long body_len;
	long data_len;
	long hdr_lines;
	long body_lines;
	long expect;
	long first_bad = -1;
	long i;
	int ok;
	int ret;

	printf("Testing #10 ...");

	make_section();
	make_article(&article, 9001, 8, "tester", "Tester", "Long");

	for (i = 0; i < MAX_SPLIT_FILE_LINES - 4; i++)
	{
		memcpy(content + 2 * i, "a\n", 2);
	}

	content[2 * (MAX_SPLIT_FILE_LINES - 4)] = '\0';

	ret = article_cache_generate(CACHE_DIR_PATH, &article, &section, content, "1.2.3.4", 1);
	check_int(ret, 0, "article_cache_generate() a document longer than the index");

	check_true(read_cache(&article) == 0, "the long cache file can be read back");

	if (disk_data == NULL)
	{
		printf("Completed testing #10: %d of %d checks passed\n", check_count - failed_count, check_count);

		return;
	}

	build_header(&article);
	header_len = (long)strlen(exp_header);

	hdr_lines = spans_of(exp_header, header_len, HEADER_MAX_COLS, 1, spans_of_region,
			MAX_SPLIT_FILE_LINES);
	check_int(hdr_lines, 5, "the lines of the header of a long document");

	body_lines = MAX_SPLIT_FILE_LINES - 1 - hdr_lines;
	body_len = 2 * body_lines;
	data_len = header_len + body_len;

	// No entry is left for the footer, so the footer of such a document is cut short and
	// never written, and the last lines of its body go with it
	check_int(disk.line_total, MAX_SPLIT_FILE_LINES - 1, "the lines of a document past the index");
	check_true((long)disk.data_len == data_len, "the data of a document past the index");
	check_int(file_size(cache_path_of(&article)), prefix_len(disk.line_total) + data_len,
			"the file of a document past the index");
	check_int((long)strlen(content) - body_len, 4,
			"the bytes of the body no line of the index is left for");

	ok = 1;

	for (i = 0; i <= disk.line_total; i++)
	{
		if (i < hdr_lines)
		{
			expect = 0;

			for (long line = 0; line < i; line++)
			{
				expect += spans_of_region[line];
			}
		}
		else if (i == hdr_lines)
		{
			expect = header_len;
		}
		else
		{
			expect = header_len + 2 * (i - hdr_lines);
		}

		if (disk.line_offsets[i] != expect)
		{
			ok = 0;
			first_bad = i;

			break;
		}
	}

	check_true(ok, "every offset of a document past the index");

	if (!ok)
	{
		printf("the index of the long document differs from it at line %ld\n", first_bad);
	}

	check_bytes(disk_data + data_len - 2, "a\n", 2, "a document past the index ends on its body");

	printf("Completed testing #10: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void test_after_cleanup(void)
{
	printf("Testing #11 ...");

	article_cache_cleanup();

	check_simple("a document after the cleanup", "Body after cleanup\n", 10001, 9);

	printf("Completed testing #11: %d of %d checks passed\n", check_count - failed_count, check_count);
}

static void remove_cache_dir(void)
{
	DIR *dp;
	struct dirent *de;
	char path[FILE_PATH_LEN];

	dp = opendir(CACHE_DIR);
	if (dp == NULL)
	{
		return;
	}

	while ((de = readdir(dp)) != NULL)
	{
		if (de->d_name[0] == '.')
		{
			continue;
		}

		snprintf(path, sizeof(path), "%s/%s", CACHE_DIR, de->d_name);
		unlink(path);
	}

	closedir(dp);
	rmdir(CACHE_DIR);
}

int main(void)
{
	log_begin("../log/test_article_cache.log", "../log/test_article_cache_error.log");
	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	if (setlocale(LC_ALL, "en_US.UTF-8") == NULL)
	{
		printf("WARNING: setlocale(LC_ALL, en_US.UTF-8) failed, wide character tests may differ\n");
	}

	if (mkdir(CACHE_DIR, 0755) < 0 && errno != EEXIST)
	{
		printf("mkdir(%s) error (%d)\n", CACHE_DIR, errno);

		return -1;
	}

	{
		FILE *fp = fopen(TRIE_SHM_FILE, "w");

		if (fp == NULL || fclose(fp) != 0)
		{
			printf("fopen(%s) error\n", TRIE_SHM_FILE);

			return -1;
		}
	}

	if (trie_dict_init(TRIE_SHM_FILE, TRIE_NODE_PER_POOL) < 0)
	{
		printf("trie_dict_init() failed\n");

		return -1;
	}

	if (lml_init() < 0)
	{
		printf("lml_init() failed\n");

		return -1;
	}

	test_contracts();
	test_simple_documents();
	test_wrapped_documents();
	test_rendered_documents();
	test_file_names_and_overwrite();
	test_footer_colors();
	test_footer_wrap();
	test_load_and_unload();
	test_unreadable_caches();
	test_index_limit();
	test_after_cleanup();

	lml_cleanup();
	trie_dict_cleanup();
	article_cache_cleanup();
	free(exp_body);
	exp_body = NULL;

	remove_cache_dir();
	unlink(TRIE_SHM_FILE);

	if (failed_count > 0)
	{
		printf("Failed: %d of %d checks\n", failed_count, check_count);

		return -1;
	}

	printf("Done\n");

	return 0;
}
