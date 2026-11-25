/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * shm_name
 *   - POSIX shared memory names of the data pools
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifndef _SHM_NAME_H_
#define _SHM_NAME_H_

#include "common.h"

#include <libgen.h>
#include <stdio.h>
#include <string.h>

// The name comes from the pool prefix and the base file name. macOS accepts POSIX
// shared memory names of at most 31 characters, so there the two are folded into a
// 48-bit FNV-1a hash of the name other platforms spell out.
static inline void shm_name_set(char *shm_name, size_t name_size, const char *prefix, const char *filename)
{
	char filepath[FILE_PATH_LEN];
#if defined(__APPLE__)
	char full_name[FILE_NAME_LEN];
	const char *p;
	unsigned long long hash = 14695981039346656037ULL; // FNV-1a offset basis
#endif

	strncpy(filepath, filename, sizeof(filepath) - 1);
	filepath[sizeof(filepath) - 1] = '\0';

#if defined(__APPLE__)
	snprintf(full_name, sizeof(full_name), "/%s_%s", prefix, basename(filepath));

	for (p = full_name; *p != '\0'; p++)
	{
		hash = (hash ^ (unsigned long long)(unsigned char)*p) * 1099511628211ULL; // FNV-1a prime
	}

	snprintf(shm_name, name_size, "/LBBS_%012llX", hash & 0xFFFFFFFFFFFFULL);
#else
	snprintf(shm_name, name_size, "/%s_%s", prefix, basename(filepath));
#endif
}

#endif // #ifndef _SHM_NAME_H_
