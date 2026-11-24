bin_PROGRAMS = bbsd

lib_LTLIBRARIES = libbbs.la
libbbs_la_SOURCES = bbs_net.c

bbsd_SOURCES = ${srcdir}/../include/*.h article_cache.c article_del.c article_favor.c article_favor_display.c article_op.c \
			   article_post.c article_view_log.c bbs.c bbs_cmd.c bbs_main.c bwf.c common.c data_file_mgr.c database.c \
			   editor.c file_loader.c hash_dict.c init.c io.c ip_mask.c lml.c log.c login.c memory_pool.c menu.c main.c \
			   menu_proc.c net_server.c screen.c section_list.c section_list_display.c section_list_loader.c str_process.c \
			   trie_dict.c user_list.c user_info_display.c user_info_update.c user_list_display.c user_priv.c user_stat.c

if STATIC_LINK
bbsd_LDADD = libbbs.la ../utils/game/libgame.la
endif

bbsd_LDFLAGS=-export-dynamic

AM_CFLAGS = -I${srcdir}/../include -I${srcdir}/../utils/game ${LIBSSH_CFLAGS} ${MYSQLCLIENT_CFLAGS} ${SYSTEMD_CFLAGS} ${LIBPCRE2_CFLAGS} ${ICONV_CFLAGS} \
			-std=c17 -Wall -Werror -Wformat -Wpedantic -Wformat-security -Wconversion \
			-Wdeprecated -finline-functions
LIBS = ${LIBSSH_LIBS} ${MYSQLCLIENT_LIBS} ${SYSTEMD_LIBS} ${LIBPCRE2_LIBS} ${ICONV_LIBS} -ldl

libdir = ${prefix}/lib
