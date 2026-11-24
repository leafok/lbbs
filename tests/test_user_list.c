/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_user_list
 *   - tester for user list query operations
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bbs.h"
#include "log.h"
#include "trie_dict.h"
#include "user_list.h"
#include "user_stat.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char USER_LIST_SHM_FILE[] = "~user_list_shm.dat";
static const char TRIE_DICT_SHM_FILE[] = "~trie_dict_shm.dat";

int main(int argc, char *argv[])
{
	USER_INFO user_info;
	USER_INFO users[BBS_user_limit_per_page];
	USER_ONLINE_INFO online_users[BBS_user_limit_per_page];
	int user_count;
	int page_count;
	int ret;
	int32_t uid_list[10];
	char username_list[10][BBS_username_max_len + 1];
	FILE *fp;

	if (log_begin("~test_user_list.log", "~test_user_list_error.log") < 0)
	{
		printf("Open log error\n");
		return -1;
	}

	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	if ((fp = fopen(TRIE_DICT_SHM_FILE, "w")) == NULL)
	{
		log_error("fopen(%s) error", TRIE_DICT_SHM_FILE);
		return -1;
	}
	fclose(fp);

	if ((fp = fopen(USER_LIST_SHM_FILE, "w")) == NULL)
	{
		log_error("fopen(%s) error", USER_LIST_SHM_FILE);
		return -1;
	}
	fclose(fp);

	if (trie_dict_init(TRIE_DICT_SHM_FILE, TRIE_NODE_PER_POOL) < 0)
	{
		printf("trie_dict_init failed\n");
		return -1;
	}

	if (user_list_pool_init(USER_LIST_SHM_FILE) < 0)
	{
		log_error("user_list_pool_init(%s) error", USER_LIST_SHM_FILE);
		return -2;
	}

	printf("Testing #1 ...\n");

	// Test NULL pointer handling
	ret = query_user_list(0, NULL, &user_count, &page_count);
	if (ret != -1)
	{
		printf("query_user_list() with NULL p_users should return -1, got %d\n", ret);
		return -3;
	}

	ret = query_user_list(0, users, NULL, &page_count);
	if (ret != -1)
	{
		printf("query_user_list() with NULL p_user_count should return -1, got %d\n", ret);
		return -3;
	}

	ret = query_user_list(0, users, &user_count, NULL);
	if (ret != -1)
	{
		printf("query_user_list() with NULL p_page_count should return -1, got %d\n", ret);
		return -3;
	}

	ret = query_user_info(0, NULL);
	if (ret != -1)
	{
		printf("query_user_info() with NULL p_user should return -1, got %d\n", ret);
		return -3;
	}

	ret = query_user_info_by_uid(0, NULL, NULL, 0);
	if (ret != -1)
	{
		printf("query_user_info_by_uid() with NULL p_user should return -1, got %d\n", ret);
		return -3;
	}

	ret = query_user_online_list(0, NULL, &user_count, &page_count);
	if (ret != -1)
	{
		printf("query_user_online_list() with NULL p_online_users should return -1, got %d\n", ret);
		return -3;
	}

	ret = query_user_online_info(0, NULL);
	if (ret != -1)
	{
		printf("query_user_online_info() with NULL p_user should return -1, got %d\n", ret);
		return -3;
	}

	printf("Testing #2 ...\n");

	// Test empty list queries
	ret = query_user_list(0, users, &user_count, &page_count);
	if (ret != 0)
	{
		printf("query_user_list() on empty list should return 0, got %d\n", ret);
		return -4;
	}

	if (user_count != 0)
	{
		printf("Empty list should have user_count=0, got %d\n", user_count);
		return -4;
	}

	if (page_count != 0)
	{
		printf("Empty list should have page_count=0, got %d\n", page_count);
		return -4;
	}

	ret = query_user_info(0, &user_info);
	if (ret != 0)
	{
		printf("query_user_info() on empty list should return 0, got %d\n", ret);
		return -4;
	}

	ret = query_user_info_by_uid(1, &user_info, NULL, 0);
	if (ret != 0)
	{
		printf("query_user_info_by_uid() on empty list should return 0, got %d\n", ret);
		return -4;
	}

	ret = query_user_online_list(0, online_users, &user_count, &page_count);
	if (ret != 0)
	{
		printf("query_user_online_list() on empty list should return 0, got %d\n", ret);
		return -4;
	}

	printf("Testing #3 ...\n");

	// Test invalid page_id on empty list - returns 0 because empty list check happens first
	ret = query_user_list(-1, users, &user_count, &page_count);
	if (ret != 0)
	{
		printf("query_user_list() with negative page_id on empty list should return 0, got %d\n", ret);
		return -5;
	}

	ret = query_user_list(100, users, &user_count, &page_count);
	if (ret != 0)
	{
		printf("query_user_list() with out-of-range page_id on empty list should return 0, got %d\n", ret);
		return -5;
	}

	printf("Testing #4 ...\n");

	// Test user_article_cnt operations - these require a populated user_list
	// which needs database access, so we skip this test in the unit test suite
	// The functions are tested in integration tests with actual database data
	printf("Skipping user_article_cnt tests (require database-populated user_list)\n");

	printf("Testing #5 ...\n");

	// Test get_user_list_count
	int total_users;
	ret = get_user_list_count(&total_users);
	if (ret < 0)
	{
		printf("get_user_list_count() failed with ret=%d\n", ret);
		return -7;
	}

	if (total_users != 0)
	{
		printf("Empty list should have total_users=0, got %d\n", total_users);
		return -7;
	}

	ret = get_user_list_count(NULL);
	if (ret != -1)
	{
		printf("get_user_list_count() with NULL should return -1, got %d\n", ret);
		return -7;
	}

	printf("Testing #6 ...\n");

	// Test get_user_online_list_count
	int online_users_count, guest_count;
	ret = get_user_online_list_count(&online_users_count, &guest_count);
	if (ret < 0)
	{
		printf("get_user_online_list_count() failed with ret=%d\n", ret);
		return -8;
	}

	if (online_users_count != 0 || guest_count != 0)
	{
		printf("Empty online list should have counts=0, got online=%d guest=%d\n", online_users_count, guest_count);
		return -8;
	}

	ret = get_user_online_list_count(NULL, &guest_count);
	if (ret != -1)
	{
		printf("get_user_online_list_count() with NULL p_user_cnt should return -1, got %d\n", ret);
		return -8;
	}

	ret = get_user_online_list_count(&online_users_count, NULL);
	if (ret != -1)
	{
		printf("get_user_online_list_count() with NULL p_guest_cnt should return -1, got %d\n", ret);
		return -8;
	}

	printf("Testing #7 ...\n");

	// Test get_user_login_count
	int login_count;
	ret = get_user_login_count(&login_count);
	if (ret < 0)
	{
		printf("get_user_login_count() failed with ret=%d\n", ret);
		return -9;
	}

	if (login_count != 0)
	{
		printf("Initial login_count should be 0, got %d\n", login_count);
		return -9;
	}

	ret = get_user_login_count(NULL);
	if (ret != -1)
	{
		printf("get_user_login_count() with NULL should return -1, got %d\n", ret);
		return -9;
	}

	printf("Testing #8 ...\n");

	// Test query_user_info_by_username
	ret = query_user_info_by_username("test", 10, uid_list, username_list);
	if (ret < 0)
	{
		printf("query_user_info_by_username() failed with ret=%d\n", ret);
		return -10;
	}

	if (ret != 0)
	{
		printf("Empty list should return 0 matches, got %d\n", ret);
		return -10;
	}

	ret = query_user_info_by_username(NULL, 10, uid_list, username_list);
	if (ret != -1)
	{
		printf("query_user_info_by_username() with NULL username should return -1, got %d\n", ret);
		return -10;
	}

	ret = query_user_info_by_username("test", 10, NULL, username_list);
	if (ret != -1)
	{
		printf("query_user_info_by_username() with NULL uid_list should return -1, got %d\n", ret);
		return -10;
	}

	ret = query_user_info_by_username("test", 10, uid_list, NULL);
	if (ret != -1)
	{
		printf("query_user_info_by_username() with NULL username_list should return -1, got %d\n", ret);
		return -10;
	}

	printf("Testing #9 ...\n");

	// Test get_user_id_list
	int32_t uid_array[100];
	int uid_count;
	ret = get_user_id_list(uid_array, &uid_count, 0);
	if (ret < 0)
	{
		printf("get_user_id_list() failed with ret=%d\n", ret);
		return -11;
	}

	if (uid_count != 0)
	{
		printf("Empty list should return uid_count=0, got %d\n", uid_count);
		return -11;
	}

	ret = get_user_id_list(NULL, &uid_count, 0);
	if (ret != -1)
	{
		printf("get_user_id_list() with NULL p_uid_list should return -1, got %d\n", ret);
		return -11;
	}

	ret = get_user_id_list(uid_array, NULL, 0);
	if (ret != -1)
	{
		printf("get_user_id_list() with NULL p_user_cnt should return -1, got %d\n", ret);
		return -11;
	}

	printf("Testing #10 ...\n");

	// Test query_user_online_info_by_uid
	USER_ONLINE_INFO online_info_array[10];
	int online_count;
	ret = query_user_online_info_by_uid(100, online_info_array, &online_count, 0);
	if (ret < 0)
	{
		printf("query_user_online_info_by_uid() failed with ret=%d\n", ret);
		return -12;
	}

	if (online_count != 0)
	{
		printf("Empty online list should return online_count=0, got %d\n", online_count);
		return -12;
	}

	ret = query_user_online_info_by_uid(100, NULL, &online_count, 0);
	if (ret != -1)
	{
		printf("query_user_online_info_by_uid() with NULL p_users should return -1, got %d\n", ret);
		return -12;
	}

	ret = query_user_online_info_by_uid(100, online_info_array, NULL, 0);
	if (ret != -1)
	{
		printf("query_user_online_info_by_uid() with NULL p_user_cnt should return -1, got %d\n", ret);
		return -12;
	}

	printf("All tests passed!\n");

	user_list_pool_cleanup();
	trie_dict_cleanup();

	if (unlink(USER_LIST_SHM_FILE) < 0)
	{
		log_error("unlink(%s) error", USER_LIST_SHM_FILE);
		return -1;
	}

	if (unlink(TRIE_DICT_SHM_FILE) < 0)
	{
		log_error("unlink(%s) error", TRIE_DICT_SHM_FILE);
		return -1;
	}

	log_end();
	unlink("~test_user_list.log");
	unlink("~test_user_list_error.log");
	return 0;
}
