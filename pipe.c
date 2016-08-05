#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>
#include "alias.h"
#include "util.h"

static void is_bg(char *inp);
static void tokenize(char **inp, char *delim);
static char parse_cmd(char **inp, char **cmd, int start_ind);
static char is_redir(char **cmd, char **dst, char *ap_flag);
static void redir_me(char **cmd, int redir_fd, char *redir_dst, char ap_flag);
static int builtin(char **cmd);
static void exec_me(char **cmd);
static void pipe_me(char **cmd, char **inp, char *delim);
static void eval(char *inp);
static int is_quoted(char *str);

static int bg_flag = 0;

static void is_bg(char *inp)
{
	for (inp = inp+(strlen(inp)-2); *inp==' '; inp--)
		;/* going back from end till non-space char */
	bg_flag = *inp=='&' ? 1 : 0;
	if (bg_flag == 1)
		*inp = ' ';
}

static void tokenize(char **inp, char *delim)
{
	char *start;

	for (start=*inp; **inp != '\0'; (*inp)++) {
		if (**inp==' ' || **inp=='|' || **inp=='\n' || **inp==';') {
			*delim = **inp;
			**inp = '\0'; /* crafted the token */
			break;
		}
	}
}

static char parse_cmd(char **inp, char **cmd, int start_ind)
{
	int i, len;
	char delim; /* deals with delimiter for args/cmds of inp */
	char *start_arg; /* keeps track of args/cmds of inp */
	char temp_arg[4096]; /* used in alias */
	alias **al_cur; /* alias pointer */

	i = start_ind; /* no FUCKING clue why i have to do this, can't use 
					  start_ind, it fucks up on piping */
	if (!(**inp)) /* NULL string */
		return 0;

	while (**inp == ' ') /* Eating leading spaces */
		(*inp)++;

	for (start_arg=*inp; **inp!='\0'; (*inp)++) {
		/* quoted arg is taken as one */
		if (**inp == '\"' || **inp == '\'') {
			delim = **inp;
			cmd[i++] = ++(*inp);
			*inp = strchr(*inp, delim);
			**inp = '\0'; /* cmd[i] points till here */
			start_arg = *inp + 1; /* points to the next arg/cmd */
		}
		if (**inp==' ' || **inp=='|' || **inp=='\n' || **inp==';') {
			delim = **inp;
			**inp = '\0'; /* now cmd[i] can point till here */

			/* start_arg can be NULL if multiple spaces are dealt with
			 * in which the above line nullify start_arg itself which
			 * is an 'ok' way to skip the bloody spaces */

			if (*start_arg) {
				while (*(al_cur = al_src(start_arg))) {
					len = strlen(start_arg); /* if strlen is used for below's exp
												massive coredump will follow,
											   	mysteries of OS here */
					strcpy(temp_arg, start_arg);
					**inp = delim; /* so that repl_str works till actual '\0' of cmdline */
					*inp = *inp - len; /* goes back to start_arg */
					repl_str(temp_arg, (*al_cur)->trans, inp); /* alias trans is done here */
					tokenize(inp, &delim); /* the modified token after alias is delimited with
											  '\0'and its following delim becomes the new delim,
											  inp points to end of token */
					if (!strcmp((*al_cur)->name, start_arg))
						break; /* so that it doesn't alias the same over and over again */
				}
				cmd[i++] = start_arg;
			}
			if (delim == '|' || delim == '\n' || delim == ';') {
				(*inp)++; /* so that main inp str points to the next */
				break; /* reached the end of ONE command */
			}
			start_arg = *inp + 1; /* points to the next arg/cmd */
		}
	}

	cmd[i] = NULL; /* to make argvp happy */
	return delim; /* for verifying if this to be piped or bg'd etc */
}

static char is_redir(char **cmd, char **dst, char *ap_flag)
{
	char ret;

	while (*cmd != NULL) {
		ret = **cmd;
		if (ret=='1' || ret=='2')
			if (*(*cmd+1) == '>') {
				*dst = *(cmd+1); /* the next arg */
				*ap_flag = (*(*cmd+2) == '>') ? 1 : 0;
				/* append flag and dst set */
				*cmd = NULL; /* so that execvp will be happy */
				return ret - 0x30; /* ascii to dec */
			}
		cmd = cmd+1; /* next arg */
	}
	return -1; /* no redir here */
}

static void redir_me(char **cmd, int redir_fd, char *redir_dst, char ap_flag)
{
	int dst_fd;

	if (!ap_flag) /* don't append the output */
		dst_fd = creat(redir_dst, 0644);
	else /* append the output */
		dst_fd = open(redir_dst, O_RDWR | O_APPEND);
	dup2(dst_fd, redir_fd); /* now exec can be done */
}

static int builtin(char **cmd)
{
	int err;

	if (cmd[0]) {
		switch (*cmd[0]) {
		case 'a' :
			if (!strcmp(cmd[0], "alias")) {
				alias_me(cmd);
				return 1;
			}
			break;
		case 'c' :
			if (!strcmp(cmd[0], "cd")) {
				if (cmd[1]==NULL) {
					chdir(getenv("HOME"));
					return 1;
				}

				if (chdir(cmd[1])<0 && ((err=errno) != ENOENT)) {
					ERR("chdir");
				}

				if (strcmp(cmd[1], "..") && err==ENOENT) {
					fprintf(stderr, "cd : Not here..\n");
					return 1;
				}
				
				return 1;
			}
			break;
		}
	}
	return 0;
}

static void exec_me(char **cmd)
{
	int pid;
	char redir_fd, ap_flag;
	char *redir_dst;
	

	if (builtin(cmd))
		return;

	if ((pid = fork()) < 0)
		ERR("fork");
	if (!pid) {
		/* check if this cmd need redirection */
		if ((redir_fd=is_redir(cmd, &redir_dst, &ap_flag)) != -1)
			redir_me(cmd, redir_fd, redir_dst, ap_flag);
		execvp(cmd[0], cmd);
		ERR("execvp");
	}
	if(!bg_flag) {
		/* sleep(1); */
		if (wait(NULL) < 0) /* waits for child */
			ERR("wait");
	}
	else {
		printf("\nAdded to FG\n");
	}
}

static void pipe_me(char **cmd, char **inp, char *delim)
{
	int pipe_fd[2];

	int stdin_fd, stdout_fd;

	dup2(STDIN_FILENO, stdin_fd); /* saves orig stdin of parent */

	while (*delim == '|') { /* there is more to pipe */
		pipe(pipe_fd);
		if (!fork()) {
			dup2(pipe_fd[1], STDOUT_FILENO);
			execvp(cmd[0], cmd); /* writes to pipe */
		}
		/* The child is the reader and the writer of the pipe */
		wait(NULL); /* waits for child */
		dup2(pipe_fd[0], STDIN_FILENO); /* sets up for read from pipe */
		close(pipe_fd[1]); /* so read from pipe by next child is fine */
		*delim = parse_cmd(inp, cmd, 0);
	}

	exec_me(cmd); /* the final pipe cmd prints to STDOUT */
	dup2(stdin_fd, STDIN_FILENO); /* restores the orig stdin for parent */
}


static void eval(char *inp)
{
	char *cmd[256];
	char delim;

	do {
		if ((delim = parse_cmd(&inp, cmd, 0)) == '|')
			pipe_me(cmd, &inp, &delim);
		else
			exec_me(cmd); /* this will handle redir as well */
	} while (delim != '\0' && delim != '\n');
	/* cmds end in '\n' or '\0', i guess */
}

static int is_quoted(char *str)
{
	while (*str != '\0') {
		while (*str!='\0' && *str!='\"' && *str!='\'')
			str++;
		if (str)
			if ((str=strchr(str+1, *str)) == NULL)
				return 0;
			else
				str++;
	}
	return 1;
}

int main()
{
	char *inp;
	char cmd[MAX_LINE];
	int i;

	inp = cmd;
	setbuf(stdout, NULL);
	while (1) {
		printf("SH>");
		fgets(cmd, MAX_LINE, stdin);
		while (!is_quoted(cmd)) {
			inp = strchr(inp, '\n');
			printf(" >");
			fgets(inp, MAX_LINE, stdin);
		}
		inp = cmd;
		is_bg(cmd);
		eval(cmd);
		memset(cmd, 0, MAX_LINE);
	}
	
}
