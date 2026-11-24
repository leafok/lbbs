/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_user_priv
 *   - tester for user privilege
 *
 * Copyright (C) 2004-2026  Leaflet <leaflet@leafok.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "user_priv.h"
#include <stdio.h>
#include <string.h>

static int check_count = 0;
static int failed_count = 0;

// Start a record the way load_priv() does, without needing a database connection
static void init_priv(BBS_user_priv *p_priv, int uid)
{
	memset(p_priv, 0, sizeof(*p_priv));

	p_priv->uid = uid;
	p_priv->level = (uid == 0 ? P_GUEST : P_USER);
	p_priv->g_priv = S_DEFAULT;
	p_priv->s_count = 0;
}

static void check_int(int actual, int expected, const char *what)
{
	check_count++;

	if (actual != expected)
	{
		failed_count++;
		printf("FAILED: %s = %d, expected %d\n", what, actual, expected);
	}
}

static void check_getpriv(BBS_user_priv *p_priv, int sid, int expected_priv, int expected_favor, const char *what)
{
	char msg[128];
	int favor = -1;
	int priv;

	priv = getpriv(p_priv, sid, &favor);

	snprintf(msg, sizeof(msg), "%s: getpriv(%d)", what, sid);
	check_int(priv, expected_priv, msg);

	snprintf(msg, sizeof(msg), "%s: getpriv(%d) favor", what, sid);
	check_int(favor, expected_favor, msg);

	snprintf(msg, sizeof(msg), "%s: is_favor(%d)", what, sid);
	check_int(is_favor(p_priv, sid), expected_favor, msg);
}

// s_priv_list must stay strictly ascending: search_priv() is a binary search, and any
// other order silently hides the entries that fall out of its reach
static void check_sorted(BBS_user_priv *p_priv, const char *what)
{
	char msg[128];
	int i;
	int broken = 0;

	for (i = 1; i < p_priv->s_count; i++)
	{
		if (p_priv->s_priv_list[i].sid <= p_priv->s_priv_list[i - 1].sid)
		{
			broken++;
		}
	}

	snprintf(msg, sizeof(msg), "%s: s_priv_list ascending and free of duplicates", what);
	check_int(broken, 0, msg);
}

// Every stored entry must answer with its own word, not with the global one
static void check_all_reachable(BBS_user_priv *p_priv, const char *what)
{
	char msg[128];
	int i;
	int favor = 0;
	int lost = 0;

	for (i = 0; i < p_priv->s_count; i++)
	{
		int sid = p_priv->s_priv_list[i].sid;
		int expected_favor = p_priv->s_priv_list[i].is_favor;
		int expected_priv = p_priv->s_priv_list[i].s_priv;

		if (getpriv(p_priv, sid, &favor) != expected_priv || favor != expected_favor)
		{
			lost++;
		}
	}

	snprintf(msg, sizeof(msg), "%s: every stored entry reads back", what);
	check_int(lost, 0, msg);
}

static void test_global_word(void)
{
	BBS_user_priv privs;
	int rc;

	init_priv(&privs, 1);

	// sid 0 is the global word, and an unknown sid inherits it
	check_getpriv(&privs, 0, S_DEFAULT, 0, "empty record");
	check_getpriv(&privs, 5, S_DEFAULT, 0, "empty record");
	check_getpriv(&privs, -1, S_NONE, 0, "negative sid");
	check_int(checkpriv(&privs, 0, S_DEFAULT), 1, "checkpriv(0, S_DEFAULT)");
	check_int(checkpriv(&privs, 5, S_POST), 0, "checkpriv(5, S_POST)");

	// setpriv(0) rewrites the global word and never touches s_priv_list
	rc = setpriv(&privs, 0, S_ALL, 0);
	check_int(rc, 0, "setpriv(0) return");
	check_int(privs.s_count, 0, "setpriv(0) leaves s_count");
	check_getpriv(&privs, 7, S_ALL, 0, "after setpriv(0, S_ALL)");
	check_int(checkpriv(&privs, 7, S_ADMIN), 1, "checkpriv(7, S_ADMIN)");

	// a board entry shadows the global word for that board only
	rc = setpriv(&privs, 7, S_LIST, 1);
	check_int(rc, 0, "setpriv(7) return");
	check_getpriv(&privs, 7, S_LIST, 1, "board entry overrides global");
	check_getpriv(&privs, 8, S_ALL, 0, "unknown sid still uses global");
}

static void test_insert_order(void)
{
	BBS_user_priv privs;
	static const int sids[] = {10, 20, 5, 30, 1, 15};
	const int sid_count = (int)(sizeof(sids) / sizeof(sids[0]));
	int i;

	// the pair that used to collapse onto offset 0
	init_priv(&privs, 1);
	setpriv(&privs, 10, S_LIST, 0);
	setpriv(&privs, 20, S_POST, 0);
	check_int(privs.s_count, 2, "ascending pair count");
	check_int(privs.s_priv_list[0].sid, 10, "ascending pair first slot");
	check_int(privs.s_priv_list[1].sid, 20, "ascending pair second slot");
	check_getpriv(&privs, 10, S_LIST, 0, "ascending pair");
	check_getpriv(&privs, 20, S_POST, 0, "ascending pair");

	init_priv(&privs, 1);
	setpriv(&privs, 20, S_POST, 0);
	setpriv(&privs, 10, S_LIST, 0);
	check_int(privs.s_priv_list[0].sid, 10, "descending pair first slot");
	check_int(privs.s_priv_list[1].sid, 20, "descending pair second slot");
	check_getpriv(&privs, 10, S_LIST, 0, "descending pair");
	check_getpriv(&privs, 20, S_POST, 0, "descending pair");

	// a single entry must not absorb the second insert either
	init_priv(&privs, 1);
	setpriv(&privs, 5, S_LIST, 0);
	setpriv(&privs, 5, S_POST, 0);
	check_int(privs.s_count, 1, "repeat insert count");
	check_getpriv(&privs, 5, S_POST, 0, "repeat insert replaces the entry");

	init_priv(&privs, 1);
	for (i = 0; i < sid_count; i++)
	{
		setpriv(&privs, sids[i], sids[i] * 8 + S_LIST, sids[i] == 15);
	}
	check_sorted(&privs, "mixed order");
	check_all_reachable(&privs, "mixed order");
	check_int(privs.s_count, sid_count, "mixed order count");
	for (i = 0; i < sid_count; i++)
	{
		check_getpriv(&privs, sids[i], sids[i] * 8 + S_LIST, sids[i] == 15, "mixed order");
	}
}

// Sweep the sid range in several arrival orders, the way load_priv() meets them:
// unordered section_master rows, then ascending section_config rows
static void test_insert_orders(void)
{
	BBS_user_priv privs;
	static const int steps[] = {1, 3, 7, 11, 13, 37, 61};
	const int step_count = (int)(sizeof(steps) / sizeof(steps[0]));
	const int sid_limit = 100;
	int s;
	int i;

	for (s = 0; s < step_count; s++)
	{
		char tag[32];

		snprintf(tag, sizeof(tag), "step %d", steps[s]);

		init_priv(&privs, 1);
		for (i = 0; i < sid_limit; i++)
		{
			int sid = (i * steps[s]) % sid_limit + 1;
			setpriv(&privs, sid, sid, (sid % 3) == 0);
		}
		check_int(privs.s_count, sid_limit, tag);
		check_sorted(&privs, tag);
		check_all_reachable(&privs, tag);

		// the same set arriving backwards
		init_priv(&privs, 1);
		for (i = sid_limit - 1; i >= 0; i--)
		{
			int sid = (i * steps[s]) % sid_limit + 1;
			setpriv(&privs, sid, sid, (sid % 3) == 0);
		}
		check_int(privs.s_count, sid_limit, tag);
		check_sorted(&privs, tag);
		check_all_reachable(&privs, tag);
	}
}

// A section master of two boards must keep S_MAN_M on both of them
static void test_section_master(void)
{
	BBS_user_priv privs;
	static const int masters[] = {20, 10};
	const int master_count = (int)(sizeof(masters) / sizeof(masters[0]));
	const int masked = (S_MAN_M | S_DEFAULT) & (~S_LIST);
	int favor;
	int i;

	init_priv(&privs, 1);

	for (i = 0; i < master_count; i++)
	{
		int priv = getpriv(&privs, masters[i], &favor);
		setpriv(&privs, masters[i], priv | S_MAN_M, favor);
	}
	check_getpriv(&privs, 20, S_MAN_M | S_DEFAULT, 0, "section master");
	check_getpriv(&privs, 10, S_MAN_M | S_DEFAULT, 0, "section master");

	// the section_config pass, in its ORDER BY SID order, masks S_LIST off every board
	for (i = 1; i <= 30; i++)
	{
		int priv = getpriv(&privs, i, &favor);
		setpriv(&privs, i, priv & (~S_LIST), favor);
	}
	check_int(privs.s_count, 30, "section status count");
	check_sorted(&privs, "section status");
	check_all_reachable(&privs, "section status");
	check_getpriv(&privs, 20, masked, 0, "master after masking");
	check_getpriv(&privs, 10, masked, 0, "master after masking");
	check_getpriv(&privs, 25, S_GETEXP, 0, "plain board after masking");
	check_int(checkpriv(&privs, 20, S_MAN_M), 1, "checkpriv(20, S_MAN_M)");
	check_int(checkpriv(&privs, 10, S_MAN_M), 1, "checkpriv(10, S_MAN_M)");
	check_int(checkpriv(&privs, 25, S_LIST), 0, "checkpriv(25, S_LIST)");
	check_int(checkpriv(&privs, 25, S_GETEXP), 1, "checkpriv(25, S_GETEXP)");
}

static void test_favor(void)
{
	BBS_user_priv privs;

	init_priv(&privs, 1);

	check_int(is_favor(&privs, 7), 0, "is_favor(7) before any entry");

	setpriv(&privs, 7, S_LIST, 1);
	check_int(is_favor(&privs, 7), 1, "is_favor(7) after favoring");

	setpriv(&privs, 7, S_LIST | S_POST, 1);
	check_int(is_favor(&privs, 7), 1, "is_favor(7) survives a privilege update");

	setpriv(&privs, 7, S_LIST | S_POST, 0);
	check_int(is_favor(&privs, 7), 0, "is_favor(7) after clearing");
	check_getpriv(&privs, 7, S_LIST | S_POST, 0, "board keeps its word when the favor bit goes");
}

static void test_full_list(void)
{
	BBS_user_priv privs;
	int rc;
	int i;

	init_priv(&privs, 1);

	// fill to capacity from the top down, so placement relies on the search
	for (i = BBS_max_section; i > 0; i--)
	{
		rc = setpriv(&privs, i, S_LIST, 0);
		check_int(rc, 0, "setpriv within capacity");
	}
	check_int(privs.s_count, BBS_max_section, "full list count");
	check_sorted(&privs, "full list");
	check_all_reachable(&privs, "full list");

	// beyond capacity the entry is refused, and nothing already stored is disturbed
	rc = setpriv(&privs, BBS_max_section + 1, S_POST, 0);
	check_int(rc, -1, "setpriv beyond capacity");
	check_int(privs.s_count, BBS_max_section, "rejected insert leaves s_count");
	check_getpriv(&privs, BBS_max_section + 1, S_DEFAULT, 0, "beyond capacity falls back to global");
	check_getpriv(&privs, 100, S_LIST, 0, "existing entry survives a rejected insert");
	check_getpriv(&privs, BBS_max_section, S_LIST, 0, "last entry survives a rejected insert");

	// the global word is never capacity-limited
	rc = setpriv(&privs, 0, S_ALL, 0);
	check_int(rc, 0, "setpriv(0) on a full list");
	check_getpriv(&privs, BBS_max_section + 1, S_ALL, 0, "unknown sid uses the global word when full");
}

static void test_reload(void)
{
	BBS_user_priv privs;
	int rc;
	int i;

	init_priv(&privs, 1);
	for (i = 1; i <= 12; i++)
	{
		setpriv(&privs, i, i * 8 + S_POST, i == 3);
	}
	check_int(privs.s_count, 12, "populated before reload");

	// load_priv() bails out without a connection, but the record still has to come back clean
	rc = load_priv(NULL, &privs, 0);
	check_int(rc, 1, "load_priv(NULL) return");
	check_int(privs.uid, 0, "reload resets uid");
	check_int(privs.level, P_GUEST, "reload resets level");
	check_int(privs.g_priv, S_DEFAULT, "reload resets g_priv");
	check_int(privs.s_count, 0, "reload resets s_count");

	// the entries themselves must be gone, not only their count
	check_int(privs.s_priv_list[0].sid, 0, "reload clears the first slot");
	check_int(privs.s_priv_list[2].is_favor, 0, "reload clears the favor bit");
	check_int(privs.s_priv_list[11].s_priv, 0, "reload clears the last slot");

	for (i = 1; i <= 12; i++)
	{
		check_getpriv(&privs, i, S_DEFAULT, 0, "entry after reload");
	}

	// a smaller reload must not meet leftovers on its way up
	init_priv(&privs, 1);
	for (i = 1; i <= 12; i++)
	{
		setpriv(&privs, i, i * 8 + S_POST, i == 3);
	}
	load_priv(NULL, &privs, 5);
	for (i = 1; i <= 5; i++)
	{
		setpriv(&privs, i, S_GETEXP, 0);
	}
	check_int(privs.s_count, 5, "partial reload count");
	check_sorted(&privs, "partial reload");
	check_all_reachable(&privs, "partial reload");
	check_int(privs.level, P_USER, "reload for a non-zero uid gives P_USER");

	rc = load_priv(NULL, &privs, 0);
	check_int(rc, 1, "load_priv(NULL) for uid 0");
	check_int(privs.level, P_GUEST, "reload for uid 0 gives P_GUEST");
}

static void test_checklevel(void)
{
	BBS_user_priv privs;

	init_priv(&privs, 1);

	// checklevel() is a mask test, checklevel2() is a numeric one
	privs.level = P_MAN_M;
	check_int(checklevel(&privs, P_GUEST), 1, "checklevel(P_GUEST)");
	check_int(checklevel2(&privs, P_GUEST), 1, "checklevel2(P_GUEST)");
	check_int(checklevel(&privs, P_MAN_M), 1, "checklevel(P_MAN_M) exact bit");
	check_int(checklevel(&privs, P_MAN_S), 0, "checklevel(P_MAN_S) unrelated bit");
	check_int(checklevel2(&privs, P_MAN_S), 1, "checklevel2(P_MAN_S) lower rank");
	check_int(checklevel2(&privs, P_ADMIN_S), 0, "checklevel2(P_ADMIN_S) higher rank");

	privs.level = P_ADMIN_S | P_MAN_M;
	check_int(checklevel(&privs, P_MAN_M), 1, "combined level, high bit");
	check_int(checklevel(&privs, P_MAN_S), 0, "combined level, absent bit");
	check_int(checklevel(&privs, P_ADMIN_S | P_MAN_M), 1, "combined level, both bits");
	check_int(checklevel2(&privs, P_ADMIN_M), 0, "combined level, rank below P_ADMIN_M");
	check_int(checklevel2(&privs, P_ADMIN_S), 1, "combined level, rank at P_ADMIN_S");

	privs.level = P_ADMIN_M;
	check_int(checklevel(&privs, P_MAN_M), 0, "top level has no P_MAN_M bit");
	check_int(checklevel2(&privs, P_MAN_M), 1, "top level outranks P_MAN_M");
}

int main(int argc, char *argv[])
{
	printf("Testing #1 ...\n");
	test_global_word();
	printf("Completed testing #1: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #2 ...\n");
	test_insert_order();
	printf("Completed testing #2: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #3 ...\n");
	test_insert_orders();
	printf("Completed testing #3: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #4 ...\n");
	test_section_master();
	printf("Completed testing #4: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #5 ...\n");
	test_favor();
	printf("Completed testing #5: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #6 ...\n");
	test_full_list();
	printf("Completed testing #6: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #7 ...\n");
	test_reload();
	printf("Completed testing #7: %d of %d checks passed\n", check_count - failed_count, check_count);

	printf("Testing #8 ...\n");
	test_checklevel();
	printf("Completed testing #8: %d of %d checks passed\n", check_count - failed_count, check_count);

	if (failed_count > 0)
	{
		printf("Failed: %d of %d checks\n", failed_count, check_count);

		return -1;
	}

	printf("Done\n");

	return 0;
}
