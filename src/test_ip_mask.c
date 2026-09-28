/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_ip_mask
 *   - tester for IP address mask
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ip_mask.h"
#include <stdio.h>
#include <string.h>

// Longest expected output is "*.*.*.*" (7 bytes) + '\0'
#define TEST_STR_LEN 32

typedef struct _ip_mask_test_case_t
{
	const char *input;
	int level;
	char mask;
	const char *expected;
} IP_MASK_TEST_CASE;

static const IP_MASK_TEST_CASE test_cases[] = {
	// level <= 0: string is left as is
	{"192.168.1.100", 0, '*', "192.168.1.100"},
	{"192.168.1.100", -1, '*', "192.168.1.100"},
	{"192.168.1.100", -100, '*', "192.168.1.100"},
	// level 1: mask the last octet only
	{"192.168.1.100", 1, '*', "192.168.1.*"},
	{"127.0.0.1", 1, '*', "127.0.0.*"},
	{"0.0.0.0", 1, '*', "0.0.0.*"},
	{"10.11.12.13", 1, '#', "10.11.12.#"},
	{"10.11.12.13", 1, '%', "10.11.12.%"},
	{"192.168.1.100:8080", 1, '*', "192.168.1.*"},
	// level 2: mask the last two octets
	{"192.168.1.100", 2, '*', "192.168.*.*"},
	{"127.0.0.1", 2, '*', "127.0.*.*"},
	{"255.255.255.255", 2, '*', "255.255.*.*"},
	// level 3: mask the last three octets
	{"192.168.1.100", 3, '*', "192.*.*.*"},
	{"127.0.0.1", 3, '*', "127.*.*.*"},
	// level 4: mask all octets
	{"192.168.1.100", 4, '*', "*.*.*.*"},
	{"127.0.0.1", 4, '*', "*.*.*.*"},
	// level > 4: treated as level 4
	{"192.168.1.100", 5, '*', "*.*.*.*"},
	{"192.168.1.100", 100, '*', "*.*.*.*"},
	// Not a complete IPv4 address: nothing to mask when there are not enough '.' separators
	{"192.168.1", 1, '*', "192.168.1"},
	{"192.168.1", 2, '*', "192.168.*.*"},
	{"localhost", 1, '*', "localhost"},
	{"localhost", 2, '*', "localhost"},
	// Blank string: no '.' separator to be found, the full string is masked only when level >= 4
	{"", 0, '*', ""},
	{"", 1, '*', ""},
	{"", 4, '*', "*.*.*.*"},
};

static const int test_case_count = (int)(sizeof(test_cases) / sizeof(test_cases[0]));

static const int octet_values[] = {0, 1, 9, 10, 99, 100, 127, 128, 168, 192, 254, 255};
static const int octet_value_count = (int)(sizeof(octet_values) / sizeof(octet_values[0]));

static int check_count = 0;
static int failed_count = 0;

static void check_ip_mask(const char *input, int level, char mask, const char *expected, int verbose)
{
	char str[TEST_STR_LEN];
	const char *p;

	strncpy(str, input, sizeof(str) - 1);
	str[sizeof(str) - 1] = '\0';

	p = ip_mask(str, level, mask);

	check_count++;

	if (p != str)
	{
		failed_count++;
		printf("FAILED: ip_mask(\"%s\", %d, '%c') returned a pointer other than the input\n", input, level, mask);
	}
	else if (strcmp(str, expected) != 0)
	{
		failed_count++;
		printf("FAILED: ip_mask(\"%s\", %d, '%c') = \"%s\", expected \"%s\"\n", input, level, mask, str, expected);
	}
	else if (verbose)
	{
		printf("PASSED: ip_mask(\"%s\", %d, '%c') = \"%s\"\n", input, level, mask, str);
	}
}

static void check_ip_mask_all_levels(int a, int b, int c, int d)
{
	char input[TEST_STR_LEN];
	char expected[TEST_STR_LEN];

	snprintf(input, sizeof(input), "%d.%d.%d.%d", a, b, c, d);

	for (int level = 0; level <= 4; level++)
	{
		switch (level)
		{
		case 0:
			snprintf(expected, sizeof(expected), "%d.%d.%d.%d", a, b, c, d);
			break;
		case 1:
			snprintf(expected, sizeof(expected), "%d.%d.%d.*", a, b, c);
			break;
		case 2:
			snprintf(expected, sizeof(expected), "%d.%d.*.*", a, b);
			break;
		case 3:
			snprintf(expected, sizeof(expected), "%d.*.*.*", a);
			break;
		default:
			snprintf(expected, sizeof(expected), "*.*.*.*");
			break;
		}

		check_ip_mask(input, level, '*', expected, 0);
	}
}

int main(int argc, char *argv[])
{
	int i;

	printf("Testing #1 ...\n");

	for (i = 0; i < test_case_count; i++)
	{
		check_ip_mask(test_cases[i].input, test_cases[i].level, test_cases[i].mask, test_cases[i].expected, 1);
	}

	printf("Completed testing #1: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #2 ...\n");

	for (i = 0; i < octet_value_count; i++)
	{
		check_ip_mask_all_levels(octet_values[i],
								 octet_values[(i * 3 + 1) % octet_value_count],
								 octet_values[(i * 5 + 2) % octet_value_count],
								 octet_values[(i * 7 + 3) % octet_value_count]);
	}

	printf("Completed testing #2: %d of %d checks passed\n", check_count - failed_count, check_count);

	if (failed_count > 0)
	{
		printf("Failed: %d of %d checks\n", failed_count, check_count);

		return -1;
	}

	printf("Done\n");

	return 0;
}
