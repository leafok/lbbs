/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_user_stat
 *   - tester for user related statistics
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bbs.h"
#include "log.h"
#include "section_list.h"
#include "trie_dict.h"
#include "user_list.h"
#include "user_stat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char ARTICLE_BLOCK_SHM_FILE[] = "~test_user_stat_article_block_shm.dat";
static const char SECTION_LIST_SHM_FILE[] = "~test_user_stat_section_list_shm.dat";
static const char TRIE_DICT_SHM_FILE[] = "~test_user_stat_trie_dict_shm.dat";
static const char USER_LIST_SHM_FILE[] = "~test_user_stat_user_list_shm.dat";

// uid and visible of the articles one section is filled with
static const int32_t article_uid[] = {1, 1, 2, 3, 4};
static const int8_t article_visible[] = {1, 1, 0, 1, 1};
static const int article_count = 5;

// the map a test run starts from: uid 5 has no article, uid 4 is not in the map
static const int32_t map_uid[] = {1, 2, 3, 5};
static const int map_count = 4;

static int check_count = 0;
static int failed_count = 0;

static USER_STAT_MAP *p_map = NULL;

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

static void build_map(int32_t last_uid, int last_article_index)
{
	static const int cnt_list[] = {0, 5, 0, 7};
	int i;

	p_map->user_count = map_count;
	p_map->last_uid = last_uid;
	p_map->last_article_index = last_article_index;

	for (i = 0; i < map_count; i++)
	{
		p_map->stat_list[i].uid = map_uid[i];
		p_map->stat_list[i].article_count = cnt_list[i];
	}
}

// reading a count must not depend on the search settling on an unusable slot
static void check_cnt(int32_t uid, long expected, const char *tag)
{
	const USER_STAT *p_stat = NULL;
	char msg[128];

	snprintf(msg, sizeof(msg), "%s: stat of uid %d", tag, uid);

	if (user_stat_get(p_map, uid, &p_stat) != 1)
	{
		check_true(0, msg);
		return;
	}

	check_int(p_stat->article_count, expected, msg);
}

static void test_map_init(void)
{
	check_int(user_stat_map_init(NULL), -1, "user_stat_map_init(NULL)");

	p_map->user_count = 123;
	p_map->last_uid = 456;
	p_map->last_article_index = 789;

	check_int(user_stat_map_init(p_map), 0, "user_stat_map_init()");
	check_int(p_map->user_count, 0, "map_init: user_count");
	check_int(p_map->last_uid, -1, "map_init: last_uid");
	check_int(p_map->last_article_index, -1, "map_init: last_article_index");
}

static void test_lookup(void)
{
	const USER_STAT *p_stat = NULL;

	build_map(100, -1);

	// every stored uid is reachable, and only its own count comes back
	check_cnt(1, 0, "get uid 1");
	check_cnt(2, 5, "get uid 2");
	check_cnt(3, 0, "get uid 3");
	check_cnt(5, 7, "get uid 5, the last entry");

	// the neighbours of a stored uid are absent, whatever the search settles on
	check_int(user_stat_get(p_map, 0, &p_stat), 0, "get uid 0, below the first");
	check_true(p_stat == NULL, "get uid 0 reports no record");
	check_int(user_stat_get(p_map, 4, &p_stat), 0, "get uid 4, in a gap");
	check_true(p_stat == NULL, "get uid 4 reports no record");
	check_int(user_stat_get(p_map, 6, &p_stat), 0, "get uid 6, after the last");
	check_true(p_stat == NULL, "get uid 6 reports no record");
	check_int(user_stat_get(p_map, 1000000, &p_stat), 0, "get uid far above");
	check_true(p_stat == NULL, "get uid far above reports no record");
	check_int(user_stat_get(p_map, -1, &p_stat), 0, "get a negative uid");

	// a single entry is both the first and the last of the list
	p_map->user_count = 1;
	check_int(user_stat_get(p_map, 1, &p_stat), 1, "get of a single entry");
	check_int(user_stat_get(p_map, 2, &p_stat), 0, "get past a single entry");
	check_int(user_stat_get(p_map, 3, &p_stat), 0, "get far past a single entry");

	// counting up follows the same search, and reports a miss instead of writing one
	build_map(100, -1);
	check_int(user_stat_article_cnt_inc(p_map, 3, 1), 1, "inc uid 3");
	check_int(user_stat_article_cnt_inc(p_map, 3, 4), 1, "inc uid 3 again");
	check_cnt(3, 5, "increments accumulate");
	check_int(user_stat_article_cnt_inc(p_map, 1, -2), 1, "inc uid 1 by a negative");
	check_cnt(1, -2, "the count went negative");

	check_int(user_stat_article_cnt_inc(p_map, 4, 1), 0, "inc of an absent uid");
	check_int(user_stat_article_cnt_inc(p_map, 6, 1), 0, "inc after the last uid");
	check_int(user_stat_article_cnt_inc(p_map, 0, 1), 0, "inc of uid 0 with no entry");
	check_int(p_map->user_count, map_count, "an absent uid adds no entry");
	check_cnt(2, 5, "an absent uid left uid 2 alone");
	check_cnt(5, 7, "an absent uid left uid 5 alone");

	check_int(user_stat_get(NULL, 1, &p_stat), -1, "user_stat_get(NULL)");
	check_int(user_stat_article_cnt_inc(NULL, 1, 1), -1, "user_stat_article_cnt_inc(NULL)");
}

// The user list lives in shared memory and is only filled from the database, so in a test
// run no user is ever loaded: last_uid has to stay where the caller left it
static void test_update_without_users(void)
{
	int scanned = article_block_article_count();

	check_int(user_stat_map_update(NULL), -1, "user_stat_map_update(NULL)");

	user_stat_map_init(p_map);

	check_int(user_stat_map_update(p_map), 0, "update of a fresh map");
	check_int(p_map->user_count, 0, "update: no user loaded");
	check_int(p_map->last_uid, -1, "update: last_uid of a map that never saw a user");
	check_int(p_map->last_article_index, scanned - 1, "update: article cursor");

	// the steady state of a periodic run over a map that is already loaded
	build_map(100, -1);
	check_int(user_stat_map_update(p_map), 0, "update over an empty pool");
	check_int(p_map->user_count, map_count, "update over an empty pool keeps user_count");
	check_int(p_map->last_uid, 100, "update over an empty pool keeps last_uid");

	// every visible article of the scan counted once, in the map, into the right uid
	check_cnt(1, 2, "scan");
	check_cnt(2, 5, "scan: its only article is hidden");
	check_cnt(3, 1, "scan");
	check_cnt(5, 7, "scan: uid 5 has no article at all");
	check_int(p_map->last_article_index, scanned - 1, "scan: article cursor");

	// a pass over no new article changes nothing
	check_int(user_stat_map_update(p_map), 0, "update with nothing new");
	check_int(p_map->user_count, map_count, "nothing new: user_count");
	check_int(p_map->last_uid, 100, "nothing new: last_uid");
	check_int(p_map->last_article_index, scanned - 1, "nothing new: article cursor");
	check_cnt(1, 2, "nothing new");
	check_cnt(3, 1, "nothing new");

	// an author above last_uid waits for the pass that loads it
	build_map(2, -1);
	check_int(user_stat_map_update(p_map), 0, "update with a low last_uid");
	check_int(p_map->last_uid, 2, "low last_uid: still nothing loaded");
	check_cnt(1, 2, "low last_uid");
	check_cnt(2, 5, "low last_uid: its only article is hidden");
	check_cnt(3, 0, "low last_uid: uid 3 is above it, so its article is skipped");
	check_cnt(5, 7, "low last_uid: uid 5 has no article");
}

static void test_update_at_the_end(void)
{
	int scanned = article_block_article_count();

	build_map(100, -1);

	// a map that already reached the end of the article block recounts nothing
	p_map->last_article_index = scanned - 1;
	p_map->stat_list[0].article_count = 41;

	check_int(user_stat_map_update(p_map), 0, "update at the end");
	check_int(p_map->stat_list[0].article_count, 41, "update at the end recounts nothing");
	check_int(p_map->stat_list[3].article_count, 7, "update at the end leaves the last entry");
	check_int(p_map->user_count, map_count, "update at the end: user_count");
	check_int(p_map->last_uid, 100, "update at the end: last_uid");
	check_int(p_map->last_article_index, scanned - 1, "update at the end: cursor");
}

static int shm_file_create(const char *filename)
{
	FILE *fp;

	if ((fp = fopen(filename, "w")) == NULL)
	{
		log_error("fopen(%s) error", filename);
		return -1;
	}

	fclose(fp);

	return 0;
}

int main(int argc, char *argv[])
{
	SECTION_LIST *p_section = NULL;
	ARTICLE article;
	int block_count = 2;
	int i;

	(void)argc;
	(void)argv;

	if (log_begin("~test_user_stat.log", "~test_user_stat_error.log") < 0)
	{
		printf("Open log error\n");
		return -1;
	}

	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	p_map = calloc(1, sizeof(USER_STAT_MAP));
	if (p_map == NULL)
	{
		log_error("calloc(%lu) error", (unsigned long)sizeof(USER_STAT_MAP));
		return -1;
	}

	if (shm_file_create(ARTICLE_BLOCK_SHM_FILE) < 0 ||
		shm_file_create(SECTION_LIST_SHM_FILE) < 0 ||
		shm_file_create(TRIE_DICT_SHM_FILE) < 0 ||
		shm_file_create(USER_LIST_SHM_FILE) < 0)
	{
		return -1;
	}

	if (trie_dict_init(TRIE_DICT_SHM_FILE, TRIE_NODE_PER_POOL) < 0)
	{
		log_error("trie_dict_init(%s) error", TRIE_DICT_SHM_FILE);
		return -2;
	}

	if (article_block_init(ARTICLE_BLOCK_SHM_FILE, block_count) < 0)
	{
		log_error("article_block_init(%s, %d) error", ARTICLE_BLOCK_SHM_FILE, block_count);
		return -2;
	}

	if (section_list_init(SECTION_LIST_SHM_FILE) < 0)
	{
		log_error("section_list_init(%s) error", SECTION_LIST_SHM_FILE);
		return -2;
	}

	if (user_list_pool_init(USER_LIST_SHM_FILE) < 0)
	{
		log_error("user_list_pool_init(%s) error", USER_LIST_SHM_FILE);
		return -2;
	}

	// one section holding the articles the update pass walks over
	if (section_list_rw_lock(NULL) < 0)
	{
		printf("section_list_rw_lock(NULL) error\n");
		return -3;
	}

	p_section = section_list_create(1, "UserStat", " Test Section ", "sysadm");
	if (p_section == NULL)
	{
		printf("section_list_create() error\n");
		return -3;
	}

	for (i = 0; i < article_count; i++)
	{
		memset(&article, 0, sizeof(article));

		article.aid = i + 1; // strictly ascending, the block insists on it
		article.cid = article.aid;
		article.sid = 1;
		article.uid = article_uid[i];
		article.visible = article_visible[i];

		if (section_list_rw_lock(p_section) < 0)
		{
			printf("section_list_rw_lock(%d) error\n", p_section->sid);
			return -3;
		}

		if (section_list_append_article(p_section, &article) < 0)
		{
			printf("section_list_append_article(%d) error\n", i);
			section_list_rw_unlock(p_section);
			return -3;
		}

		if (section_list_rw_unlock(p_section) < 0)
		{
			printf("section_list_rw_unlock(%d) error\n", p_section->sid);
			return -3;
		}
	}

	check_int(article_block_last_aid(), article_count, "articles appended");
	check_int(article_block_article_count(), article_count, "article count of the blocks");

	if (section_list_rw_unlock(NULL) < 0)
	{
		printf("section_list_rw_unlock(NULL) error\n");
	}

	printf("Testing #1: user_stat_map_init ...\n");
	test_map_init();
	printf("Completed testing #1: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #2: user_stat_get and user_stat_article_cnt_inc ...\n");
	test_lookup();
	printf("Completed testing #2: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #3: user_stat_map_update ...\n");
	test_update_without_users();
	printf("Completed testing #3: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #4: user_stat_map_update at the end of the scan ...\n");
	test_update_at_the_end();
	printf("Completed testing #4: %d of %d checks passed\n", check_count - failed_count, check_count);

	user_list_pool_cleanup();
	section_list_cleanup();
	article_block_cleanup();
	trie_dict_cleanup();

	free(p_map);

	if (unlink(USER_LIST_SHM_FILE) < 0)
	{
		log_error("unlink(%s) error", USER_LIST_SHM_FILE);
	}

	if (unlink(TRIE_DICT_SHM_FILE) < 0)
	{
		log_error("unlink(%s) error", TRIE_DICT_SHM_FILE);
	}

	if (unlink(ARTICLE_BLOCK_SHM_FILE) < 0)
	{
		log_error("unlink(%s) error", ARTICLE_BLOCK_SHM_FILE);
	}

	if (unlink(SECTION_LIST_SHM_FILE) < 0)
	{
		log_error("unlink(%s) error", SECTION_LIST_SHM_FILE);
	}

	log_end();

	if (failed_count > 0)
	{
		printf("Failed: %d of %d checks\n", failed_count, check_count);

		return -1;
	}

	printf("Done\n");

	unlink("~test_user_stat.log");
	unlink("~test_user_stat_error.log");
	return 0;
}
