/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_section_list_loader
 *   - tester for section_list_loader query functions
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "article_view_log.h"
#include "bbs.h"
#include "bbs_cmd.h"
#include "log.h"
#include "menu.h"
#include "section_list.h"
#include "section_list_loader.h"
#include "trie_dict.h"
#include "user_list.h"
#include "user_priv.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char ARTICLE_BLOCK_SHM_FILE[] = "~test_sll_article_block.dat";
static const char SECTION_LIST_SHM_FILE[] = "~test_sll_section_list.dat";
static const char TRIE_DICT_SHM_FILE[] = "~test_sll_trie_dict.dat";
static const char USER_LIST_SHM_FILE[] = "~test_sll_user_list.dat";

static int check_count = 0;
static int failed_count = 0;

// Stub: always return "not viewed" so scan_unread_article_in_section works predictably
int article_view_log_is_viewed(int32_t aid, const ARTICLE_VIEW_LOG *p_view_log)
{
	(void)aid;
	(void)p_view_log;
	return 0;
}

ARTICLE_VIEW_LOG BBS_article_view_log;

// Stubs for menu/screen dependencies (not exercised by the query functions under test)
const char CTRL_SEQ_CLR_LINE[] = "\033[K";
MENU_SET bbs_menu;
MENU_SET top10_menu;

void moveto(int row, int col)
{
	(void)row;
	(void)col;
}

void clrtoeol(void)
{
}

int outc(char c)
{
	(void)c;
	return 0;
}

int prints(const char *format, ...)
{
	(void)format;
	return 0;
}

void press_any_key(void)
{
}

int detach_menu_shm(MENU_SET *p_menu)
{
	(void)p_menu;
	return 0;
}

int load_menu(MENU_SET *p_menu, const char *menu_file)
{
	(void)p_menu;
	(void)menu_file;
	return 0;
}

int unload_menu(MENU_SET *p_menu)
{
	(void)p_menu;
	return 0;
}

bbs_cmd_handler get_cmd_handler(const char *cmd)
{
	(void)cmd;
	return NULL;
}

void clrline(void)
{
}

void show_top(const char *title, int page_id, int page_count)
{
	(void)title;
	(void)page_id;
	(void)page_count;
}

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

static void setup_shm(void)
{
	FILE *fp;

	fp = fopen(ARTICLE_BLOCK_SHM_FILE, "w");
	if (fp)
		fclose(fp);

	fp = fopen(SECTION_LIST_SHM_FILE, "w");
	if (fp)
		fclose(fp);

	fp = fopen(TRIE_DICT_SHM_FILE, "w");
	if (fp)
		fclose(fp);

	fp = fopen(USER_LIST_SHM_FILE, "w");
	if (fp)
		fclose(fp);

	trie_dict_init(TRIE_DICT_SHM_FILE, TRIE_NODE_PER_POOL);
	article_block_init(ARTICLE_BLOCK_SHM_FILE, 10);
	section_list_init(SECTION_LIST_SHM_FILE);
	user_list_pool_init(USER_LIST_SHM_FILE);
}

static void cleanup_shm(void)
{
	user_list_pool_cleanup();
	article_block_cleanup();
	trie_dict_cleanup();

	unlink(USER_LIST_SHM_FILE);
	unlink(TRIE_DICT_SHM_FILE);
	unlink(ARTICLE_BLOCK_SHM_FILE);
	unlink(SECTION_LIST_SHM_FILE);
}

static SECTION_LIST *create_test_section(int sid, const char *name)
{
	SECTION_LIST *p_section;

	section_list_rw_lock(NULL);
	p_section = section_list_create(sid, name, "Test Section", "testuser");
	section_list_rw_unlock(NULL);

	return p_section;
}

static void append_article(SECTION_LIST *p_section, int32_t aid, int32_t uid, const char *username, const char *title,
						   int visible, int ontop)
{
	ARTICLE article;

	memset(&article, 0, sizeof(article));
	article.aid = aid;
	article.tid = 0;
	article.sid = p_section->sid;
	article.cid = aid;
	article.uid = uid;
	strncpy(article.username, username, sizeof(article.username) - 1);
	strncpy(article.title, title, sizeof(article.title) - 1);
	article.visible = (int8_t)visible;
	article.excerption = 0;
	article.ontop = (int8_t)ontop;
	article.lock = 0;
	article.transship = 0;

	section_list_rw_lock(p_section);
	section_list_append_article(p_section, &article);
	section_list_rw_unlock(p_section);
}

static void test_query_section_articles_empty(void)
{
	printf("Testing #1 ...\n");

	SECTION_LIST *p_section = create_test_section(100, "Empty");
	const ARTICLE *p_articles[BBS_article_limit_per_page];
	int article_count = 0;
	int page_count = 0;
	int ontop_start_offset = 0;

	int ret = query_section_articles(p_section, 0, p_articles, &article_count, &page_count, &ontop_start_offset);

	check_int(ret, 0, "query_section_articles() on empty section returns 0");
	check_int(article_count, 0, "empty section has 0 articles");
	check_int(page_count, 0, "empty section has 0 pages");
	check_int(ontop_start_offset, BBS_article_limit_per_page, "ontop_start_offset is set to limit");

	section_report(1);
}

static void test_query_section_articles_basic(void)
{
	printf("Testing #2 ...\n");

	SECTION_LIST *p_section = create_test_section(200, "Basic");

	// Append 5 visible articles with aids 1-5
	for (int i = 1; i <= 5; i++)
	{
		char title[64];
		snprintf(title, sizeof(title), "Article %d", i);
		append_article(p_section, i, 100 + i, "user", title, 1, 0);
	}

	const ARTICLE *p_articles[BBS_article_limit_per_page];
	int article_count = 0;
	int page_count = 0;
	int ontop_start_offset = 0;

	int ret = query_section_articles(p_section, 0, p_articles, &article_count, &page_count, &ontop_start_offset);

	check_int(ret, 0, "query_section_articles() on basic section returns 0");
	check_int(article_count, 5, "5 visible articles returned");
	check_int(page_count, 1, "1 page total");
	check_int(ontop_start_offset, 5, "ontop_start_offset equals visible count");
	check_true(p_articles[0]->aid == 1, "first article is aid=1");
	check_true(p_articles[4]->aid == 5, "last article is aid=5");

	section_report(2);
}

static void test_query_section_articles_with_invisible(void)
{
	printf("Testing #3 ...\n");

	SECTION_LIST *p_section = create_test_section(300, "Invisible");

	append_article(p_section, 100, 101, "user1", "Visible 1", 1, 0);
	append_article(p_section, 101, 102, "user2", "Invisible", 0, 0);
	append_article(p_section, 102, 103, "user3", "Visible 2", 1, 0);

	const ARTICLE *p_articles[BBS_article_limit_per_page];
	int article_count = 0;
	int page_count = 0;
	int ontop_start_offset = 0;

	query_section_articles(p_section, 0, p_articles, &article_count, &page_count, &ontop_start_offset);

	check_int(article_count, 2, "only 2 visible articles returned");
	check_true(p_articles[0]->aid == 100, "first visible is aid=100");
	check_true(p_articles[1]->aid == 102, "second visible is aid=102");

	section_report(3);
}

static void test_query_section_articles_with_ontop(void)
{
	printf("Testing #4 ...\n");

	SECTION_LIST *p_section = create_test_section(400, "Ontop");

	append_article(p_section, 200, 101, "user1", "Normal 1", 1, 0);
	append_article(p_section, 201, 102, "user2", "Ontop 1", 1, 1);
	append_article(p_section, 202, 103, "user3", "Normal 2", 1, 0);

	const ARTICLE *p_articles[BBS_article_limit_per_page];
	int article_count = 0;
	int page_count = 0;
	int ontop_start_offset = 0;

	query_section_articles(p_section, 0, p_articles, &article_count, &page_count, &ontop_start_offset);

	check_int(article_count, 4, "all 4 articles returned (3 from normal list + 1 ontop appended)");
	check_int(ontop_start_offset, 3, "ontop_start_offset is 3 (after normal articles)");

	section_report(4);
}

static void test_last_article_in_section(void)
{
	printf("Testing #5 ...\n");

	SECTION_LIST *p_section = create_test_section(500, "Last");

	append_article(p_section, 300, 101, "user1", "First", 1, 0);
	append_article(p_section, 301, 102, "user2", "Second", 1, 0);
	append_article(p_section, 302, 103, "user3", "Last", 1, 0);

	const ARTICLE *p_article = NULL;
	int ret = last_article_in_section(p_section, &p_article);

	check_int(ret, 1, "last_article_in_section() finds the last article");
	check_true(p_article != NULL, "p_article is not NULL");
	check_true(p_article->aid == 302, "last article is aid=302");

	// Test with invisible last article
	append_article(p_section, 303, 104, "user4", "Invisible Last", 0, 0);
	ret = last_article_in_section(p_section, &p_article);
	check_int(ret, 1, "last_article_in_section() skips invisible articles");
	check_true(p_article->aid == 302, "last visible article is still aid=302");

	section_report(5);
}

static void test_scan_article_by_uid(void)
{
	printf("Testing #6 ...\n");

	SECTION_LIST *p_section = create_test_section(600, "UID");

	append_article(p_section, 400, 101, "user1", "Article 1", 1, 0);
	append_article(p_section, 401, 102, "user2", "Article 2", 1, 0);
	append_article(p_section, 402, 101, "user1", "Article 3", 1, 0);
	append_article(p_section, 403, 103, "user3", "Article 4", 1, 0);

	const ARTICLE *p_cur = article_block_find_by_aid(400);
	const ARTICLE *p_found = NULL;

	int ret = scan_article_in_section_by_uid(p_section, p_cur, 1, 101, &p_found);
	check_int(ret, 1, "scan by uid forward finds next article by same user");
	check_true(p_found != NULL, "p_found is not NULL");
	check_true(p_found->aid == 402, "found article is aid=402");

	ret = scan_article_in_section_by_uid(p_section, p_cur, 1, 999, &p_found);
	check_int(ret, 0, "scan by uid forward returns 0 when not found");
	check_true(p_found == NULL, "p_found is NULL when not found");

	section_report(6);
}

static void test_scan_article_by_username(void)
{
	printf("Testing #7 ...\n");

	SECTION_LIST *p_section = create_test_section(700, "Username");

	append_article(p_section, 500, 101, "alice", "Article 1", 1, 0);
	append_article(p_section, 501, 102, "bob", "Article 2", 1, 0);
	append_article(p_section, 502, 103, "ALICE", "Article 3", 1, 0);

	const ARTICLE *p_cur = article_block_find_by_aid(500);
	const ARTICLE *p_found = NULL;

	// Case-insensitive match
	int ret = scan_article_in_section_by_username(p_section, p_cur, 1, "alice", &p_found);
	check_int(ret, 1, "scan by username forward finds case-insensitive match");
	check_true(p_found != NULL, "p_found is not NULL");
	check_true(p_found->aid == 502, "found article is aid=502 (ALICE matches alice)");

	section_report(7);
}

static void test_scan_article_by_title(void)
{
	printf("Testing #8 ...\n");

	SECTION_LIST *p_section = create_test_section(800, "Title");

	append_article(p_section, 600, 101, "user1", "Introduction to C", 1, 0);
	append_article(p_section, 601, 102, "user2", "Advanced C++", 1, 0);
	append_article(p_section, 602, 103, "user3", "C Programming", 1, 0);

	const ARTICLE *p_cur = article_block_find_by_aid(600);
	const ARTICLE *p_found = NULL;

	// Substring match, case-insensitive
	int ret = scan_article_in_section_by_title(p_section, p_cur, 1, "c", &p_found);
	check_int(ret, 1, "scan by title forward finds substring match");
	check_true(p_found != NULL, "p_found is not NULL");
	check_true(p_found->aid == 601, "found article is aid=601 (C++ matches c)");

	ret = scan_article_in_section_by_title(p_section, p_cur, 1, "programming", &p_found);
	check_int(ret, 1, "scan by title forward finds 'programming'");
	check_true(p_found->aid == 602, "found article is aid=602");

	section_report(8);
}

static void test_scan_unread_article(void)
{
	printf("Testing #9 ...\n");

	SECTION_LIST *p_section = create_test_section(900, "Unread");

	BBS_priv.uid = 101;

	append_article(p_section, 700, 101, "user1", "My Article", 1, 0);
	append_article(p_section, 701, 102, "user2", "Other Article", 1, 0);

	const ARTICLE *p_cur = article_block_find_by_aid(700);
	const ARTICLE *p_unread = NULL;

	int ret = scan_unread_article_in_section(p_section, p_cur, &p_unread);
	check_int(ret, 1, "scan_unread finds next unread article");
	check_true(p_unread != NULL, "p_unread is not NULL");
	check_true(p_unread->aid == 701, "unread article is aid=701 (not by current user)");

	section_report(9);
}

static void test_null_pointer_checks(void)
{
	printf("Testing #10 ...\n");

	int ret;
	const ARTICLE *p_article = NULL;
	const ARTICLE *p_articles[BBS_article_limit_per_page];
	int article_count = 0;
	int page_count = 0;
	int ontop_start_offset = 0;

	ret = query_section_articles(NULL, 0, p_articles, &article_count, &page_count, &ontop_start_offset);
	check_int(ret, -1, "query_section_articles() with NULL section returns -1");

	SECTION_LIST *p_section = create_test_section(1000, "Null");
	append_article(p_section, 800, 101, "user1", "Test", 1, 0);

	ret = last_article_in_section(NULL, &p_article);
	check_int(ret, -1, "last_article_in_section() with NULL section returns -1");

	ret = last_article_in_section(p_section, NULL);
	check_int(ret, -1, "last_article_in_section() with NULL pp_article returns -1");

	ret = scan_article_in_section_by_uid(NULL, p_article, 1, 101, &p_article);
	check_int(ret, -1, "scan by uid with NULL section returns -1");

	ret = scan_article_in_section_by_username(NULL, p_article, 1, "user", &p_article);
	check_int(ret, -1, "scan by username with NULL section returns -1");

	ret = scan_article_in_section_by_title(NULL, p_article, 1, "title", &p_article);
	check_int(ret, -1, "scan by title with NULL section returns -1");

	ret = scan_unread_article_in_section(NULL, p_article, &p_article);
	check_int(ret, -1, "scan_unread with NULL section returns -1");

	section_report(10);
}

int main(void)
{
	if (log_begin("~test_sll.log", "~test_sll_error.log") < 0)
	{
		printf("log_begin() failed\n");
		return 1;
	}

	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	setup_shm();

	test_query_section_articles_empty();
	test_query_section_articles_basic();
	test_query_section_articles_with_invisible();
	test_query_section_articles_with_ontop();
	test_last_article_in_section();
	test_scan_article_by_uid();
	test_scan_article_by_username();
	test_scan_article_by_title();
	test_scan_unread_article();
	test_null_pointer_checks();

	cleanup_shm();

	log_end();
	unlink("~test_sll.log");
	unlink("~test_sll_error.log");

	printf("Done\n");

	return failed_count == 0 ? 0 : 1;
}
