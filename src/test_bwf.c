/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_bwf
 *   - tester for bad words filter
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bwf.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *str_in[] = {
	"fuck",
	"fuck ",
	" fUck ",
	"f u c k",
	"法轮",
	"法 轮",
	" 法  轮 ",
	"大法轮",
	"法轮小",
	"大法轮小",
	"法\n轮",
	"fuckrape",
	"1 rApe  \n FuCK   2",
};

const int str_cnt = sizeof(str_in) / sizeof(const char *);

int main(int argc, char *argv[])
{
	char str[256];
	char conf_path[512];
	const char *srcdir;
	int i;
	int ret;

	if (log_begin("~test_bwf.log", "~test_bwf_error.log") < 0)
	{
		printf("Open log error\n");
		return -1;
	}

	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	srcdir = getenv("srcdir");
	if (srcdir == NULL)
	{
		srcdir = ".";
	}
	snprintf(conf_path, sizeof(conf_path), "%s/../conf/badwords.conf.default", srcdir);

	// Load BWF config
	if (bwf_load(conf_path) < 0)
	{
		return -2;
	}

	if (bwf_compile() < 0)
	{
		return -2;
	}

	for (i = 0; i < str_cnt; i++)
	{
		strncpy(str, str_in[i], sizeof(str) - 1);
		str[sizeof(str) - 1] = '\0';
		printf("Input(len=%ld): %s\n", strlen(str), str);

		ret = check_badwords(str, '*');
		if (ret < 0)
		{
			printf("Error: %d", ret);
			break;
		}
		else if (ret == 0)
		{
			printf("=== Unmatched ===\n");
		}
		else // ret > 0
		{
			printf("Output(len=%ld): %s\n", strlen(str), str);
		}
	}

	bwf_cleanup();

	log_end();

	unlink("~test_bwf.log");
	unlink("~test_bwf_error.log");
	return 0;
}
