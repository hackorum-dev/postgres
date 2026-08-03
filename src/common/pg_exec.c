/*-------------------------------------------------------------------------
 *
 * pg_exec.c
 *              Functions fo execute and manage the input/output of commands
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *        src/common/pg_exec.c
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "c.h"

#include "postgres.h"
#include "common/pg_exec.h"
#include "utils/palloc.h"

#ifdef WIN32
#include "fe_utils/string_utils.h"
#endif

static int
pcommand_count_args(const char *arg)
{
	int			count = 0;
	bool		in_word = false;

	Assert(arg != NULL);

	while (*arg)
	{
		if (isspace((unsigned char) *arg))
			in_word = false;
		else if (!in_word)
		{
			count++;
			in_word = true;
		}

		arg++;
	}

	return count;
}

void
pcommand_append_arg(PCommand * cmd, const char *arg)
{
	int			args_count;

	if (arg == NULL)
		return;

	args_count = pcommand_count_args(arg);

	if (args_count == 0)
		return;

	if (cmd->argc + args_count >= cmd->nalloc)
	{
		cmd->nalloc += args_count + 1;
		cmd->argv = repalloc_array(cmd->argv, char *, cmd->nalloc);
	}

	if (args_count > 1)
	{
		pg_split_opts(cmd->argv, &cmd->argc, arg);
		cmd->argv[cmd->argc] = NULL;
	}
	else
	{

		cmd->argv[cmd->argc++] = pstrdup(arg);
		cmd->argv[cmd->argc] = NULL;
	}
}


#ifdef WIN32
int
pcommand_win(PCommand * cmd)
{
	PQExpBufferData cmd_str;
	int			i;

	initPQExpBuffer(&cmd_str);

	appendShellString(&cmd_str, cmd->path);

	for (i = 1; i < cmd->argc; i++)
	{
		appendPQExpBufferChar(&cmd_str, ' ');
		appendShellString(&cmd_str, cmd->argv[i]);
	}

	if (cmd->silent)
	{
		appendPQExpBufferStr(&cmd_str, " > ");
		appendShellString(&cmd_str, DEVNULL);
	}

	return system(cmd_str.data);
}
#endif

PCommand *
pcommand_init(char *path, char *command)
{
	PCommand   *cmd = palloc(sizeof(PCommand));

	cmd->path = path;
	cmd->command = command;

	cmd->argc = 0;
	cmd->nalloc = 2;
	cmd->argv = palloc_array(char *, cmd->nalloc);

	pcommand_append_arg(cmd, command);

	cmd->stdin_fd = -1;
	cmd->stdout_fd = -1;
	cmd->stderr_fd = -1;

	return cmd;
}

int
pcommand_exec(PCommand * cmd)
{
	if (cmd->stdin_fd >= 0 && dup2(cmd->stdin_fd, STDIN_FILENO) < 0)
		return errno;
	if (cmd->stdout_fd >= 0 && dup2(cmd->stdout_fd, STDOUT_FILENO) < 0)
		return errno;
	if (cmd->stderr_fd >= 0 && dup2(cmd->stderr_fd, STDERR_FILENO) < 0)
		return errno;

	execv(cmd->path, cmd->argv);
	return errno;
}


#ifndef WIN32
int
pcommand_wait(PCommand * cmd)
{
	int			status = -1;

	/* This should have some kind of timeout... do we have that somewhere else */
	while (true)
	{
		if (waitpid(cmd->pid, &status, 0) < 0)
		{
			if (errno == EINTR)
				continue;
			return status;
		}
		else
		{
			return status;
		}
	}
}
#endif

/*
 * psystem() is a replacement for system(3) that avoids the use of a shell
 * It's not a full replacement yet since it doesn't support pipes. For now it
 * should be used only in places where is known that no pipe is requried and the
 * commands being call are known commands.
 * This funciton shouldn't be used as a replacement for system(3) call in places
 * like RestoreArchivedFile() or shell_archive_file()
 *
 * The return value is always the return of the executed command
 */
int
psystem(PCommand * cmd)
{
#ifdef WIN32
	return pcommand_win(cmd);
#else

	cmd->pid = fork();
	if (cmd->pid < 0)
		return -1;

	if (cmd->pid == 0)
	{
		if (cmd->silent)
			cmd->stdout_fd = open(DEVNULL, O_WRONLY);

		errno = pcommand_exec(cmd);
		_exit(127);
	}

	return pcommand_wait(cmd);
#endif
}
