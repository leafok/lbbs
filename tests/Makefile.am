# test_ssh_server binds a port and loops forever, so it is built but never run by check
unit_TESTS = test_trie_dict test_file_loader test_section_list test_lml test_article_favor test_article_view_log \
			 test_memory_pool test_bbs test_bwf test_hash_dict test_ip_mask test_user_priv test_str_process \
			 test_user_stat test_menu test_editor test_article_cache test_common test_bbs_cmd test_init test_io test_log \
			 test_section_list_loader test_user_list

TESTS = $(unit_TESTS)
test_PROGRAMS = $(unit_TESTS) test_ssh_server

testdir = ${prefix}/test

SRC = ${top_srcdir}/src

test_trie_dict_SOURCES = ${SRC}/log.c test_trie_dict.c ${SRC}/trie_dict.c
test_file_loader_SOURCES = ${SRC}/file_loader.c ${SRC}/log.c ${SRC}/str_process.c test_file_loader.c
test_section_list_SOURCES = ${SRC}/bbs.c ${SRC}/common.c ${SRC}/database.c ${SRC}/log.c ${SRC}/section_list.c test_section_list.c \
							${SRC}/trie_dict.c ${SRC}/user_list.c ${SRC}/user_priv.c ${SRC}/user_stat.c
test_lml_SOURCES = ${SRC}/lml.c ${SRC}/log.c ${SRC}/str_process.c test_lml.c ${SRC}/trie_dict.c
test_ssh_server_SOURCES = ${SRC}/log.c test_ssh_server.c
test_article_favor_SOURCES = ${SRC}/article_favor.c ${SRC}/bbs.c ${SRC}/common.c ${SRC}/database.c ${SRC}/log.c ${SRC}/section_list.c \
							 test_article_favor.c ${SRC}/trie_dict.c ${SRC}/user_list.c ${SRC}/user_priv.c ${SRC}/user_stat.c
test_article_view_log_SOURCES = ${SRC}/article_view_log.c ${SRC}/database.c ${SRC}/log.c test_article_view_log.c
test_memory_pool_SOURCES = ${SRC}/log.c ${SRC}/memory_pool.c test_memory_pool.c
test_bbs_SOURCES = ${SRC}/bbs.c ${SRC}/log.c test_bbs.c ${SRC}/user_priv.c
test_bwf_SOURCES = ${SRC}/bwf.c ${SRC}/log.c test_bwf.c
test_hash_dict_SOURCES = ${SRC}/hash_dict.c ${SRC}/log.c ${SRC}/memory_pool.c test_hash_dict.c
test_ip_mask_SOURCES = ${SRC}/ip_mask.c test_ip_mask.c
test_user_priv_SOURCES = ${SRC}/log.c test_user_priv.c ${SRC}/user_priv.c
test_str_process_SOURCES = ${SRC}/log.c ${SRC}/str_process.c test_str_process.c
test_user_stat_SOURCES = ${SRC}/bbs.c ${SRC}/common.c ${SRC}/database.c ${SRC}/log.c ${SRC}/section_list.c test_user_stat.c \
						 ${SRC}/trie_dict.c ${SRC}/user_list.c ${SRC}/user_priv.c ${SRC}/user_stat.c
test_menu_SOURCES = ${SRC}/log.c ${SRC}/menu.c test_menu.c ${SRC}/trie_dict.c ${SRC}/user_priv.c
test_editor_SOURCES = ${SRC}/editor.c ${SRC}/log.c ${SRC}/memory_pool.c ${SRC}/str_process.c test_editor.c
test_article_cache_SOURCES = ${SRC}/article_cache.c ${SRC}/lml.c ${SRC}/log.c ${SRC}/str_process.c test_article_cache.c ${SRC}/trie_dict.c
test_common_SOURCES = ${SRC}/common.c test_common.c
test_bbs_cmd_SOURCES = ${SRC}/bbs_cmd.c ${SRC}/common.c ${SRC}/log.c test_bbs_cmd.c ${SRC}/trie_dict.c
test_init_SOURCES = ${SRC}/init.c ${SRC}/log.c test_init.c
test_io_SOURCES = ${SRC}/common.c ${SRC}/io.c ${SRC}/log.c test_io.c
test_log_SOURCES = ${SRC}/log.c test_log.c
test_section_list_loader_SOURCES = ${SRC}/article_cache.c ${SRC}/bbs.c ${SRC}/common.c ${SRC}/database.c ${SRC}/ip_mask.c ${SRC}/log.c \
								   ${SRC}/lml.c ${SRC}/section_list.c ${SRC}/section_list_loader.c ${SRC}/str_process.c \
								   test_section_list_loader.c ${SRC}/trie_dict.c ${SRC}/user_list.c ${SRC}/user_priv.c ${SRC}/user_stat.c
test_user_list_SOURCES = ${SRC}/bbs.c ${SRC}/common.c ${SRC}/database.c ${SRC}/log.c ${SRC}/section_list.c test_user_list.c \
						 ${SRC}/trie_dict.c ${SRC}/user_list.c ${SRC}/user_priv.c ${SRC}/user_stat.c

AM_CFLAGS = -I${top_srcdir}/include -I${top_srcdir}/utils/game ${LIBSSH_CFLAGS} ${MYSQLCLIENT_CFLAGS} ${SYSTEMD_CFLAGS} ${LIBPCRE2_CFLAGS} ${ICONV_CFLAGS} \
			-std=c17 -Wall -Werror -Wformat -Wpedantic -Wformat-security -Wconversion \
			-Wdeprecated -finline-functions
LIBS = ${LIBSSH_LIBS} ${MYSQLCLIENT_LIBS} ${SYSTEMD_LIBS} ${LIBPCRE2_LIBS} ${ICONV_LIBS} -ldl
