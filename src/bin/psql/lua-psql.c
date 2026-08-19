#include "lua-psql.h"

#include "postgres_fe.h"
#include "command.h"
#include "common.h"
#include "settings.h"
#include "common/logging.h"
#include "psqlscanslash.h"

#include <lua.h>
#include <lauxlib.h>

#include "luapgsql.h"

LUALIB_API int L_psql_exec(lua_State *L);
LUALIB_API int L_psql_printQuery(lua_State *L);
LUALIB_API int L_psql_connect(lua_State *L);
LUALIB_API int L_psql_scan_slash_option(lua_State *L);
LUALIB_API int L_psql_downcase_identifier(lua_State *L);
LUALIB_API int L_psql_registerCommand(lua_State *L);
LUALIB_API int L_psql_log_error(lua_State *L);

#define TYPE_PSQL_SCANSTATE "psql.ScanState"

typedef struct PsqlScanState_t
{
	PsqlScanState ptr;
} PsqlScanState_t;

#define lua_to_psql_ScanState(L, i) ((PsqlScanState_t *)(lua_touserdata(L, i)))
#define lua_new_psql_ScanState(L) ((PsqlScanState_t *)(lua_newuserdata(L, sizeof(PsqlScanState_t))))

/* open the library - used by require() */
LUALIB_API int
luaopen_psql(lua_State *L)
{
	luaL_Reg luapsql[] = {
		{"exec", L_psql_exec},
		{"connect", L_psql_connect},
		{"printQuery", L_psql_printQuery},
		{"scanSlashOption", L_psql_scan_slash_option},
		{"downcaseIdentifier", L_psql_downcase_identifier},
		{"registerCommand", L_psql_registerCommand},
		{"logError", L_psql_log_error},
		{NULL, NULL}
	};

	struct
	{
		char	   *name;
		int			value;
	} psql_enums[] = {
		{"OT_NORMAL", OT_NORMAL},
		{"OT_SQLID", OT_SQLID},
		{"OT_SQLIDHACK", OT_SQLIDHACK},
		{"OT_FILEPIPE", OT_FILEPIPE},
		{"OT_WHOLE_LINE", OT_WHOLE_LINE},
		{"PSQL_CMD_SEND", PSQL_CMD_SEND},
		{"PSQL_CMD_SKIP_LINE", PSQL_CMD_SKIP_LINE},
		{"PSQL_CMD_TERMINATE", PSQL_CMD_TERMINATE},
		{"PSQL_CMD_NEWEDIT", PSQL_CMD_NEWEDIT},
		{"PSQL_CMD_ERROR", PSQL_CMD_ERROR},
		{ NULL, 0 }
	};

	int			i;

	luaL_newlib(L, luapsql);

	i = 0;
	while (psql_enums[i].name)
	{
		lua_pushstring(L, psql_enums[i].name);
		lua_pushnumber(L, (double) psql_enums[i].value);
		lua_settable(L, -3);
		i++;
	}

	lua_pushliteral(L, "_handlers");
	lua_newtable(L);
	lua_settable(L, -3);

	lua_pushliteral(L, "_help_strings");
	lua_newtable(L);
	lua_settable(L, -3);

	lua_pushliteral(L, "_tab_complete_handlers");
	lua_newtable(L);
	lua_settable(L, -3);

	luaL_newmetatable(L, TYPE_PSQL_SCANSTATE);
	lua_pop(L, 1);

	return 1;
}

/*
 * psql.printQuery
 */
LUALIB_API int
L_psql_printQuery(lua_State *L)
{
	rs_t *rs = lua_check_pgresult(L, 1);
	if (PQresultStatus(rs->ptr) == PGRES_TUPLES_OK)
	{
		printQuery(rs->ptr, &pset.popt, pset.queryFout, false, pset.logfile);
		fflush(pset.queryFout);
		if (ferror(pset.queryFout))
		{
			pg_log_error("could not print result table: %m");
		}
	}

	return 0;
}

/*
 * psql.exec(query)
 */
LUALIB_API int
L_psql_exec(lua_State *L)
{
	const char *query = (const char*)luaL_checkstring(L, 1);
	PGresult *rs;

	rs = PSQLexec(query);

	if (rs)
	{
		ExecStatusType status = PQresultStatus(rs);
		if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
			lua_push_pgresult(L, rs);
			lua_pushnil(L);
		} else {
			lua_pushnil(L);
			lua_pushstring(L, PQresultErrorMessage(rs));
			PQclear(rs);
		}
	}
	else
	{
		lua_pushnil(L);
		lua_pushliteral(L, "FATAL error");
	}

	return 2;
}

/*
 * psql.connect()
 */
LUALIB_API int
L_psql_connect(lua_State *L)
{
	if (!pset.db)
	{
		pg_log_error("You are currently not connected to a database.");
		lua_pushnil(L);
	}

	lua_push_pgconn(L, pset.db, true);

	return 1;
}

static PsqlScanState_t *
lua_check_psql_ScanState(lua_State *L, int i)
{
	luaL_checkudata(L, i, TYPE_PSQL_SCANSTATE);
	return lua_to_psql_ScanState(L, i);
}

static void
lua_push_ScanState(lua_State *L, PsqlScanState state)
{
	PsqlScanState_t *p = lua_new_psql_ScanState(L);

	luaL_getmetatable(L, TYPE_PSQL_SCANSTATE);
	lua_setmetatable(L, -2);
	p->ptr = state;
}

LUALIB_API int
L_psql_scan_slash_option(lua_State *L)
{
	PsqlScanState_t *p = lua_check_psql_ScanState(L, 1);
	double ot = lua_tonumber(L, 2);
	bool semicolon = lua_toboolean(L, 3);

	char	   *result;
	char	   quote;

	result = psql_scan_slash_option(p->ptr, (int) ot, &quote, semicolon);

	if (result)
	{
		lua_pushstring(L, result);
		lua_pushfstring(L, "%c", quote);
		free(result);
	}
	else
	{
		lua_pushnil(L);
		lua_pushnil(L);
	}

	return 2;
}

LUALIB_API int
L_psql_downcase_identifier(lua_State *L)
{
	const char	   *str = lua_tostring(L, 1);
	bool		downcase = lua_toboolean(L, 2);

	char	   *aux = strdup(str);

	dequote_downcase_identifier(aux, downcase, pset.encoding);

	lua_pushstring(L, aux);
	free(aux);

	return 1;
}

LUALIB_API int
L_psql_registerCommand(lua_State *L)
{
	const char   *name;

	const char   *help_string_syntax = NULL;
	const char   *help_string_desc = NULL;

	lua_pushliteral(L, "help_syntax");
	lua_gettable(L, 1);
	if (lua_isstring(L, -1))
	{
		help_string_syntax = lua_tostring(L, -1);
	}

	lua_pushliteral(L, "help_desc");
	lua_gettable(L, 1);
	if (lua_isstring(L, -1))
	{
		help_string_desc = lua_tostring(L, -1);
	}

	if (!help_string_syntax && help_string_desc)
	{
		lua_pushliteral(L, "incorrect argument, command description without command syntax");
		lua_error(L);
	}

	lua_getglobal(L, "psql");
	lua_getfield(L, -1, "_handlers");

	if (!lua_istable(L, 1))
	{
		lua_pushliteral(L, "incorrect argument");
		lua_error(L);
	}

	lua_pushliteral(L, "name");
	lua_gettable(L, 1);
	if (!lua_isstring(L, -1))
	{
		lua_pushliteral(L, "incorrect argument, missing \"name\"");
		lua_error(L);
	}

	name = lua_tostring(L, -1);

	lua_pushliteral(L, "handler");
	lua_gettable(L, 1);

	if (lua_isnil(L, -1))
	{
		lua_pushliteral(L, "incorrect argument, missing \"handler\"");
		lua_error(L);
	}

	if (!lua_isfunction(L, -1))
	{
		lua_pushliteral(L, "incorrect argument, handler is not a function");
		lua_error(L);
	}

	lua_settable(L, -3);

	if (help_string_syntax)
	{
		lua_getglobal(L, "psql");
		lua_getfield(L, -1, "_help_strings");
		lua_pushstring(L, help_string_syntax);

		if (help_string_desc)
		{
			lua_pushstring(L, help_string_desc);
		}
		else
			lua_pushnil(L);

		lua_settable(L, -3);
	}

	/*
	 * tab complete handler is optional
	 */
	lua_getglobal(L, "psql");
	lua_getfield(L, -1, "_tab_complete_handlers");

	lua_pushstring(L, name);

	lua_pushliteral(L, "tab_complete_handler");
	lua_gettable(L, 1);
	if (!lua_isnil(L, -1))
	{
		if (!lua_isfunction(L, -1))
		{
			lua_pushliteral(L, "incorrect argument, tab complete handler is not a function");
			lua_error(L);
		}

		lua_settable(L, -3);
	}

	return 0;
}

LUALIB_API int
L_psql_log_error(lua_State *L)
{
	const char	   *str = lua_tostring(L, 1);

	pg_log_error("%s", str);

	return 0;
}

int
exec_lua_command(lua_State *L,
				 PsqlScanState scan_state,
				 bool active_branch,
				 const char *cmd)
{
	int			result;
	char	   *buffer;
	bool		verbose;

	buffer = strdup(cmd);

	if (buffer[strlen(buffer) - 1] == '+')
	{
		verbose = true;
		buffer[strlen(buffer) - 1] = '\0';
	}
	else
		verbose = false;

	lua_getglobal(L, "psql");
	lua_getfield(L, -1, "_handlers");
	if (lua_isnil(L, -1))
	{
		pg_log_error("table psql._handlers is not available");
		free(buffer);
		return PSQL_CMD_ERROR;
	}

	lua_pushstring(L, buffer);
	lua_gettable(L, -2);
	if (lua_isnil(L, -1))
	{
		lua_pop(L, 1);
		free(buffer);
		return PSQL_CMD_UNKNOWN;
	}

	if (!lua_isfunction(L, -1))
	{
		pg_log_error("handler is not Lua function");
		lua_pop(L, 1);
		free(buffer);
		return PSQL_CMD_ERROR;
	}

	lua_push_ScanState(L, scan_state);
	lua_pushboolean(L, active_branch);
	lua_pushstring(L, cmd);

	lua_pushboolean(L, verbose);

	result = lua_pcall(L, 4, 1, 0);

	if (result != LUA_OK)
	{
		pg_log_error("Error running lua: %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		free(buffer);
		return PSQL_CMD_ERROR;
	}

	result = (int) lua_tonumber(L, -1);
	lua_pop(L, 1);
	free(buffer);

	return result;
}

int
lua_custom_commands(lua_State *L, SimpleStringList *commands)
{
	char		buffer[1024];
	int			nfields = 0;

	lua_getglobal(L, "psql");
	lua_getfield(L, -1, "_handlers");

	lua_pushnil(L);
	while (lua_next(L, -2) != 0)
	{
		snprintf(buffer, sizeof(buffer), "\\%s", lua_tostring(L, -2));
		simple_string_list_append(commands, buffer);
		nfields++;
		lua_pop(L, 1);
	}

	return nfields;
}

int
lua_global_functions(lua_State *L, SimpleStringList *functions)
{
	int			n = 0;

	lua_pushglobaltable(L);
	lua_pushnil(L);
	while (lua_next(L, -2) != 0)
	{
		if (lua_isfunction(L, -1))
		{
			simple_string_list_append(functions, lua_tostring(L, -2));
			n++;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return n;
}

int
lua_help_strings(lua_State *L, SimplePtrList *help_strings)
{
	int			nfields = 0;

	lua_getglobal(L, "psql");
	lua_getfield(L, -1, "_help_strings");

	lua_pushnil(L);
	while (lua_next(L, -2) != 0)
	{
		HelpStruct *hlp = pg_malloc(sizeof(HelpStruct));

		hlp->syntax = lua_tostring(L, -2);
		hlp->desc = lua_tostring(L, -1);
		simple_ptr_list_append(help_strings, hlp);
		nfields++;
		lua_pop(L, 1);
	}

	return nfields;
}
