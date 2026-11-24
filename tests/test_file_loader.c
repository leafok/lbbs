/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_file_loader
 *   - tester for shared memory based file loader
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "file_loader.h"
#include "log.h"
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *file_names[] = {
	"data/welcome.txt",
	"data/copyright.txt",
	"data/register.txt",
	"data/license.txt",
	"data/login_error.txt",
	"data/read_help.txt"};

#define FILES_CAP 6
#define PATH_CAP 512
static const char *files[FILES_CAP];

int files_cnt = FILES_CAP;

static void resolve_files(void)
{
	const char *srcdir = getenv("srcdir");
	char buf[PATH_CAP];
	int i;

	if (srcdir == NULL)
	{
		srcdir = ".";
	}

	for (i = 0; i < files_cnt; i++)
	{
		snprintf(buf, sizeof(buf), "%s/../%s", srcdir, file_names[i]);
		files[i] = strdup(buf);
	}
}

int main(int argc, char *argv[])
{
	int i;
	void *p_shm;
	size_t data_len;
	long line_total;
	const void *p_data;
	const long *p_line_offsets;

	if (log_begin("~test_file_loader.log", "~test_file_loader_error.log") < 0)
	{
		printf("Open log error\n");
		return -1;
	}

	log_common_redir(STDOUT_FILENO);
	log_error_redir(STDERR_FILENO);

	resolve_files();

	printf("Testing #1\n");

	for (i = 0; i < files_cnt; i++)
	{
		if (load_file(files[i]) < 0)
		{
			printf("Load file %s error\n", files[i]);
		}
	}

	for (i = 0; i < files_cnt; i++)
	{
		if ((p_shm = get_file_shm_readonly(files[i], &data_len, &line_total, &p_data, &p_line_offsets)) == NULL)
		{
			printf("Get file shm %s error\n", files[i]);
		}
		else
		{
			printf("File: %s size: %ld lines: %ld\n", files[i], data_len, line_total);

			for (int j = 0; j < line_total; j++)
			{
				printf("Line %d: %ld ~ %ld\n", j, p_line_offsets[j], p_line_offsets[j + 1]);
			}

			if (detach_file_shm(p_shm) < 0)
			{
				log_error("detach_file_shm(%s) error", files[i]);
			}
		}
	}

	printf("Testing #2\n");

	for (i = 0; i < files_cnt; i++)
	{
		if (unload_file(files[i]) < 0)
		{
			printf("Unload file %s error\n", files[i]);
		}
	}

	for (i = 0; i < files_cnt; i++)
	{
		if (load_file(files[i]) < 0)
		{
			printf("Load file %s error\n", files[i]);
		}
	}

	printf("Testing #3\n");

	for (i = 0; i < files_cnt; i++)
	{
		if (i % 2 == 0)
		{
			if (unload_file(files[i]) < 0)
			{
				printf("Unload file %s error\n", files[i]);
			}
		}
	}

	for (i = 0; i < files_cnt; i++)
	{
		if (i % 2 != 0)
		{
			if ((p_shm = get_file_shm_readonly(files[i], &data_len, &line_total, &p_data, &p_line_offsets)) == NULL)
			{
				printf("Get file shm %s error\n", files[i]);
			}
			else
			{
				printf("File: %s size: %ld lines: %ld\n", files[i], data_len, line_total);

				if (detach_file_shm(p_shm) < 0)
				{
					log_error("detach_file_shm(%s) error", files[i]);
				}
			}
		}
	}

	log_end();

	unlink("~test_file_loader.log");
	unlink("~test_file_loader_error.log");
	return 0;
}
