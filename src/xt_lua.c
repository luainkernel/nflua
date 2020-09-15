/*
 * Copyright (C) 2017-2019  CUJO LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/version.h>

#include <linux/netfilter/x_tables.h>
#include <net/netns/generic.h>

#include <lua.h>

#include "xt_lua.h"
#include "nf_util.h"
#include "kpi_compat.h"
#include "luapacket.h"
#include "luautil.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CUJO LLC <opensource@cujo.com>");

MODULE_DESCRIPTION("Xtables: Lua packet match and target");

extern int luaopen_packet(lua_State *);

static const luaL_Reg libs[] = {
	{"packet", luaopen_packet},
	{NULL, NULL}
};

luaU_id nflua_ctx;
luaU_id nflua_sock;

static int xt_lua_net_id __read_mostly;

struct xt_lua_net *xt_lua_pernet(struct net *net)
{
	return net_generic(net, xt_lua_net_id);
}

static void nflua_mt_destroy(const struct xt_mtdtor_param *par)
{
	struct xt_lua_mtinfo *info = par->matchinfo;

	if (info->state != NULL)
		lunatik_putstate(info->state);
}

static int nflua_mt_checkentry(const struct xt_mtchk_param *par)
{
	struct xt_lua_mtinfo *info = par->matchinfo;
	lunatik_State *s;
	const luaL_Reg *lib;

	if ((s = lunatik_netstatelookup(info->name, par->net)) == NULL)
		return -EPERM;

	if (!lunatik_getstate(s))
		return -ESTALE;

	for (lib = libs; lib->name != NULL; lib++) {
		luaL_requiref(s->L, lib->name, lib->func, 1);
		lua_pop(s->L, 1);
	}

	info->state = s;
	return 0;
}

enum mode {
	NFLUA_MATCH,
	NFLUA_TARGET
};

static int nflua_docall(lua_State *L)
{
	const char *func = lua_touserdata(L, 1);
	struct sk_buff *skb = lua_touserdata(L, 2);
	int hooknum = lua_tointeger(L, 3);
	int mode = lua_tointeger(L, 4);
	int error;

	lua_settop(L, 0);

	luapacket_new(L, skb, hooknum);

	if (lua_getglobal(L, func) != LUA_TFUNCTION)
		return luaL_error(L, "couldn't find function: %s\n", func);

	lua_pushvalue(L, 1);
	error = lua_pcall(L, 1, 1, 0);

	if (mode == NFLUA_TARGET && lua_isstring(L, -1) &&
	    strcmp(lua_tostring(L, -1), "stolen") == 0)
		luapacket_stolen(L, 1);
	else
		luapacket_unref(L, 1);

	if (error)
		return lua_error(L);

	return 1;
}

static unsigned int string_to_tg(const char *s)
{
	struct target_pair {
		const char *k;
		int v;
	};
	static struct target_pair targets[] = {
		{"drop", NF_DROP},
		{"accept", NF_ACCEPT},
		{"stolen", NF_STOLEN},
		{"queue", NF_QUEUE},
		{"repeat", NF_REPEAT},
		{"stop", NF_STOP}
	};
	int i;

	for (i = 0; i < sizeof(targets) / sizeof(*targets); i++)
		if (strcmp(targets[i].k, s) == 0)
			return targets[i].v;

	return XT_CONTINUE;
}

union call_result {
	bool mt;
	unsigned int tg;
};

static union call_result nflua_call(struct sk_buff *skb,
    struct xt_action_param *par, int mode)
{
	const struct xt_lua_mtinfo *info = par->matchinfo;
	lua_State *L = info->state->L;
	union call_result r;
	int base;

	switch (mode) {
	case NFLUA_MATCH:
		r.mt = false;
		break;
	case NFLUA_TARGET:
		r.tg = XT_CONTINUE;
		break;
	}

	spin_lock(&info->state->lock);
	if (L == NULL) {
		pr_err("invalid lua state");
		goto unlock;
	}

	base = lua_gettop(L);
	lua_pushcfunction(L, nflua_docall);
	lua_pushlightuserdata(L, (void *)info->func);
	lua_pushlightuserdata(L, skb);
	lua_pushinteger(L, kpi_xt_hooknum(par));
	lua_pushinteger(L, mode);
	if (luaU_pcall(L, 4, 1)) {
		pr_err("%s\n", lua_tostring(L, -1));
		goto cleanup;
	}

	switch (mode) {
	case NFLUA_MATCH:
		if (lua_isboolean(L, -1))
			r.mt = lua_toboolean(L, -1);
		else if (lua_isstring(L, -1) &&
		         strcmp(lua_tostring(L, -1), "hotdrop") == 0)
			par->hotdrop = true;
		else
			pr_warn("invalid match return");
		break;
	case NFLUA_TARGET:
		if (lua_isstring(L, -1))
			r.tg = string_to_tg(lua_tostring(L, -1));
		break;
	}

cleanup:
	lua_settop(L, base);
unlock:
	spin_unlock(&info->state->lock);
	return r;
}

static bool nflua_match(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_lua_mtinfo *info = par->matchinfo;

	if ((info->flags & XT_NFLUA_TCP_PAYLOAD) &&
	    tcp_payload_length(skb) <= 0)
		return false;

	return nflua_call((struct sk_buff *)skb, par, NFLUA_MATCH).mt;
}

static void nflua_tg_destroy(const struct xt_tgdtor_param *par)
{
	struct xt_lua_mtinfo *info = par->targinfo;

	if (info->state != NULL)
		lunatik_putstate(info->state);
}

static int nflua_tg_checkentry(const struct xt_tgchk_param *par)
{
	struct xt_lua_mtinfo *info = par->targinfo;
	lunatik_State *s;
	const luaL_Reg *lib;

	s = lunatik_netstatelookup(info->name, par->net);
	if (s == NULL)
		return -ENOENT;

	if (!lunatik_getstate(s))
		return -ESTALE;

	for (lib = libs; lib->name != NULL; lib++) {
		luaL_requiref(s->L, lib->name, lib->func, 1);
		lua_pop(s->L, 1);
	}

	info->state = s;
	return 0;
}

static unsigned int nflua_target(struct sk_buff *skb,
		const struct xt_action_param *par)
{
	return nflua_call(skb, (struct xt_action_param *)par, NFLUA_TARGET).tg;
}

static struct xt_match nflua_mt_reg __read_mostly = {
	.name       = "lua",
	.revision   = 0,
	.family     = NFPROTO_UNSPEC,
	.match      = nflua_match,
	.checkentry = nflua_mt_checkentry,
	.destroy    = nflua_mt_destroy,
	.matchsize  = sizeof(struct xt_lua_mtinfo),
#ifdef KPI_XT_MATCH_USERSIZE
	.usersize   = offsetof(struct xt_lua_mtinfo, state),
#endif
	.me         = THIS_MODULE
};

static struct xt_target nflua_tg_reg __read_mostly = {
	.name       = "LUA",
	.revision   = 0,
	.family     = NFPROTO_UNSPEC,
	.target     = nflua_target,
	.checkentry = nflua_tg_checkentry,
	.destroy    = nflua_tg_destroy,
	.targetsize = sizeof(struct xt_lua_mtinfo),
#ifdef KPI_XT_MATCH_USERSIZE
	.usersize   = offsetof(struct xt_lua_mtinfo, state),
#endif
	.me         = THIS_MODULE
};

static int __init xt_lua_init(void)
{
	int ret;

	pr_debug("initializing module\n");

	if ((ret = xt_register_match(&nflua_mt_reg))) {
		return ret;
	}

	if ((ret = xt_register_target(&nflua_tg_reg))) {
		xt_unregister_match(&nflua_mt_reg);
		return ret;
	}

	return ret;
}

static void __exit xt_lua_exit(void)
{
	pr_debug("unloading module\n");
	xt_unregister_match(&nflua_mt_reg);
	xt_unregister_target(&nflua_tg_reg);
}

module_init(xt_lua_init);
module_exit(xt_lua_exit);
