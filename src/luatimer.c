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
#include <linux/export.h>
#include <linux/module.h>
#include <linux/timer.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <lunatik.h>

#include "luautil.h"
#include "kpi_compat.h"

#define NFLUA_TIMER "ltimer"

struct nftimer_ctx {
	struct timer_list timer;
	lunatik_State *state;
};

static bool state_get(lunatik_State *s)
{
	if (!try_module_get(THIS_MODULE))
		return false;

	if (!lunatik_getstate(s)) {
		module_put(THIS_MODULE);
		return false;
	}

	return true;
}

static void state_put(lunatik_State *s)
{
	if (WARN_ON(s == NULL))
		return;

	lunatik_putstate(s);
	module_put(THIS_MODULE);
}

static void timeout_cb(struct timer_list *l)
{
	struct nftimer_ctx *ctx = from_timer(ctx, l, timer);
	int base;

	spin_lock(&ctx->state->lock);
	if (ctx->state->L == NULL) {
		pr_err("invalid lua state");
		goto unlock;
	}
	base = lua_gettop(ctx->state->L);

	/* check if ltimer_destroy was called for this timer */
	if (WARN_ON(!luaU_pushudata(ctx->state->L, ctx))) goto cleanup;
	luaU_unregisterudata(ctx->state->L, ctx);

	lua_getuservalue(ctx->state->L, -1);
	if (lua_pcall(ctx->state->L, 0, 0, 0) != 0) {
		pr_err("%s\n", lua_tostring(ctx->state->L, -1));
		goto cleanup;
	}

cleanup:
	lua_settop(ctx->state->L, base);
unlock:
	spin_unlock(&ctx->state->lock);
	state_put(ctx->state);
}

static int ltimer_create(lua_State *L)
{
	struct nftimer_ctx *ctx;
	unsigned long msecs = luaL_checkinteger(L, 1);

	luaL_checktype(L, 2, LUA_TFUNCTION);

	ctx = lua_newuserdata(L, sizeof(struct nftimer_ctx));
	luaL_setmetatable(L, NFLUA_TIMER);

	ctx->state = lunatik_getenv(L);
	if (!state_get(ctx->state))
		return luaL_error(L, "error incrementing state reference count");

	kpi_timer_setup(&ctx->timer, timeout_cb, ctx);
	if (mod_timer(&ctx->timer, jiffies + msecs_to_jiffies(msecs))) {
		state_put(ctx->state);
		return luaL_error(L, "error setting timer");
	}

	/* set callback function */
	lua_pushvalue(L, 2);
	lua_setuservalue(L, -2);

	luaU_registerudata(L, -1);

	return 1;
}

static int ltimer_destroy(lua_State *L)
{
	struct nftimer_ctx *ctx = luaL_checkudata(L, 1, NFLUA_TIMER);

	if (!luaU_pushudata(L, ctx)) {
		lua_pushnil(L);
		lua_pushstring(L, "timer already destroyed");
		return 2;
	}

	luaU_unregisterudata(L, ctx);
	del_timer(&ctx->timer);
	state_put(ctx->state);
	lua_pushboolean(L, true);

	return 1;
}

static const luaL_Reg timerlib[] = {
	{"create", ltimer_create},
	{"destroy", ltimer_destroy},
	{NULL, NULL}
};

int luaopen_timer(lua_State *L)
{
	luaL_newmetatable(L, NFLUA_TIMER);
	lua_pop(L, 1);

	luaL_newlib(L, timerlib);
	return 1;
}
EXPORT_SYMBOL(luaopen_timer);
