/*-------------------------------------------------------------------------
 * Exec commands for backend/frontend programs
 *
 * Copyright (c) 2018-2026, PostgreSQL Global Development Group
 *
 * src/include/common/pg_exec.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMON_PG_EXEC_H
#define COMMON_PG_EXEC_H

typedef struct PCommand
{
	pid_t		pid;
	char	   *path;
	char	   *command;
	char	   *command_win;
	char	  **argv;
	int			argc;
	int			nalloc;
	bool		silent;
	int			stdin_fd;
	int			stdout_fd;
	int			stderr_fd;
}			PCommand;

extern PCommand * pcommand_init(char *path, char *command);
extern void pcommand_append_arg(PCommand * cmd, const char *arg);
#ifdef WIN32
extern int pcommand_win(PCommand *cmd);
#endif
#ifndef WIN32
extern int pcommand_wait(PCommand *cmd);
#endif
extern int	pcommand_exec(PCommand * cmd);
extern int	psystem(PCommand * command);

#endif
