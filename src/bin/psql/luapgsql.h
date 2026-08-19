#ifndef LUAPGSQL_H
#define LUAPGSQL_H

#include <lua.h>
#include <libpq-fe.h>

typedef struct con_t {
	PGconn *ptr;
	bool shared;
	bool open;
} con_t;

typedef struct rs_t {
	PGresult *ptr;
	bool open;
	int row;
} rs_t;

extern con_t *lua_check_pgconn(lua_State *L, int i);
extern rs_t *lua_check_pgresult(lua_State *L, int i);
extern void lua_push_pgconn(lua_State *L, PGconn *con, bool shared);
extern void lua_push_pgresult(lua_State *L, PGresult *rs);

/** module registration **/

LUALIB_API int luaopen_pgsql(lua_State *L);

#endif
