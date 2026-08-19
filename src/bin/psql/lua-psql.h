#ifndef LUA_PSQL_H
#define LUA_PSQL_H

#include "postgres_fe.h"
#include "fe_utils/simple_list.h"
#include <lua.h>

typedef struct
{
	const char	   *syntax;
	const char	   *desc;
} HelpStruct;

typedef struct PsqlScanStateData *PsqlScanState;

LUALIB_API int luaopen_psql(lua_State *L);

extern int exec_lua_command(lua_State *L, PsqlScanState scan_state, bool active_branch, const char *cmd);

extern int lua_custom_commands(lua_State *L, SimpleStringList *commands);
extern int lua_global_functions(lua_State *L, SimpleStringList *functions);
extern int lua_help_strings(lua_State *L, SimplePtrList *help_strings);

#endif