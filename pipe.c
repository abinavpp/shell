#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>
#include "alias.h"
#include "util.h"

#define CHILD_DELIM(c)	((c) == ' ')
#define PARENT_DELIM(c)	((c)=='|' || (c)==';')
#define GRAND_DELIM(c)	((c)=='\n')
#define QUOTE(c)		((c)=='\'' || (c)=='\"')

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
static void aliasize(char *start_arg, char *delim, char **inp);

static int bg_flag = 0; /* To bg or not to , thats the question */

/* this marks the location at cmdline till alias blacklisting is done */
static char *al_deadend;
/* head of the singly linked list of alias blacklist to avoid nasty
 * recurisive alias cases, like alias "ls=ls;ls" */
static alias *al_blist;

static char ps1[PRMT_MAX] = "ASH>";
static char ps2[PRMT_MAX] = " >";
/* nah no ps3 or ps4 in my shell */

/* sets the global bg_flag as per inp, ie cmdline */
static void is_bg(char *inp)
{
	for (inp = inp+(strlen(inp)-2); *inp==' '; inp--)
		;/* going back from end till non-space char */
	bg_flag = *inp=='&' ? 1 : 0;
	if (bg_flag == 1)
		*inp = ' ';
}

/* pushes inp to end of token and nullifies that byte */
static void tokenize(char **inp, char *delim)
{

	for (; **inp != '\0'; (*inp)++) {
		if (QUOTE(**inp)) {
			*inp = strchr(*inp+1, **inp);
		}
		if (CHILD_DELIM(**inp) || PARENT_DELIM(**inp) || GRAND_DELIM(**inp)) {
			*delim = **inp;
			**inp = '\0'; /* crafted the token */
			break;
		}
	}
}

/* the start of iterative alias expansion fixed at start_arg location 
 * inp will point accordingly but start_arg remains */
static void aliasize(char *start_arg, char *delim, char **inp)
{
	int len;
	alias *al_cur;
	char al_arg[ARG_MAX];

	if (start_arg >= al_deadend) {
		/* no blacklisting since we crossed the deadend, so freeing the list */
		al_lin_free(&al_blist);
	}

	while (al_cur = is_alias(start_arg )) {
		if (al_lin_src(start_arg, al_blist) == NULL) {
			/* this is not a blacklisted one, hence alias it */
			al_lin_ins(start_arg, &al_blist); /* but from now till deadend it is */
			len = strlen(start_arg);
			strcpy(al_arg, start_arg);
			**inp = *delim; /* so that repl_str works till actual '\0' of cmdline */
			repl_str(al_arg, al_cur->trans, start_arg); /* alias trans is done here */

			if (al_deadend == NULL)
				al_deadend = start_arg + strlen(al_cur->trans);
			else
				al_deadend = al_deadend + ALIAS_DIFF(al_cur);
			
			/* this MUST happen so that we leave things how the parser wants after expansion */
			*inp = *inp - len; 
			tokenize(inp, delim); /* the MODIFIED token after alias is delimited with
							  '\0'and its following delim becomes the new delim,
							  inp points to end of first token after alias transition */
		}
		/* break if aliasing the blacklisted alias */
		else {
			break;
		}
	}
}

/* the god-forsaken parser!!! */
static char parse_cmd(char **inp, char **cmd, int start_ind)
{
	int i; /* main index for cmd array for exec */
	char delim; /* deals with delimiter for args/cmds of inp */
	char *start_arg; /* keeps track of args/tokens of inp */

	i = start_ind; /* no EFFING clue why i have to do this, can't use 
					  start_ind, it screws up on piping */
	if (!(**inp)) /* NULL string */
		return 0;

	while (**inp == ' ') /* Eating leading spaces */
		(*inp)++;

	for (start_arg=*inp; **inp!='\0'; (*inp)++) {
		/* quoted arg is taken as one */
		if (QUOTE(**inp)) {
			delim = **inp;
			cmd[i++] = ++(*inp);
			*inp = strchr(*inp, delim);
			**inp = '\0'; /* cmd[i] points till here */
			start_arg = *inp + 1; /* points to the next arg */
		}
		if (CHILD_DELIM(**inp) || PARENT_DELIM(**inp) || GRAND_DELIM(**inp)) {
		/* now this might be one cmd or an arg of a cmd */
			delim = **inp;
			**inp = '\0'; /* now cmd[i] can point till here */

			/* start_arg can be NULL, for eg if multiple spaces are dealt with
			 * in which the above line nullify start_arg itself which
			 * is an 'ok' way to skip the bloody spaces */

			if (*start_arg) {
				if (is_alias(start_arg))
					aliasize(start_arg, &delim, inp);
				cmd[i++] = start_arg;
			}

			if (PARENT_DELIM(delim) || GRAND_DELIM(delim)) {
				(*inp)++; /* so that main inp str points to the next */
				/* break doesn't do the for loop increment, hence the above stmt */
				break; /* reached the end of ONE command, job well done people */
			}
			/* if child delimiter */
			start_arg = *inp + 1; /* points to the next arg/token */
		}
	}

	cmd[i] = NULL; /* to make argvp happy */
	return delim; /* for verifying if this to be piped or bg'd etc */
}

/* return type of redir and sets ap_flag (append flag) */
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

/* sets up fd to do the redir */
static void redir_me(char **cmd, int redir_fd, char *redir_dst, char ap_flag)
{
	int dst_fd;

	if (!ap_flag) /* don't append the output */
		dst_fd = creat(redir_dst, 0644);
	else /* append the output */
		dst_fd = open(redir_dst, O_RDWR | O_APPEND);
	dup2(dst_fd, redir_fd); /* now exec can be done */
}

/* does shell builtins, hopefully */
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
		case 'v' :
			if (!strcmp(cmd[0], "var")) {
				return 1;
			}
			break;
		}
	}
	return 0;
}

/* regular fork and exec routine with a shell twist */
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

/* piper */
static void pipe_me(char **cmd, char **inp, char *delim)
{
	int pipe_fd[2];

	int stdin_fd;

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

/* where it all starts */
static void eval(char *inp)
{
	char *cmd[ARG_MAX]; /* main cmd array for exec */
	char delim; /* delim between cmds */

	do {
		if ((delim = parse_cmd(&inp, cmd, 0)) == '|')
			pipe_me(cmd, &inp, &delim);
		else	
			exec_me(cmd); /* this will handle redir as well */
	} while (delim != '\0' && delim != '\n');
	/* main cmdline end in '\n' or '\0', i guess.. */
	al_deadend = NULL; /* re-initiating it for a new cmdline */
}

/* checks if PS2 is required */
static int is_quoted(char *str)
{
	while (*str != '\0') {
		while (*str!='\0' && *str!='\"' && *str!='\'')
			str++;
		if (str) {
			if ((str=strchr(str+1, *str)) == NULL)
				return 0;
			else
				str++;
		}
	}
	return 1;
}

/* ~ becomes $HOME */
static void tilde_exp(char *inp)
{
	char home[PATH_MAX];
	int home_l;

	strcpy(home, getenv("HOME"));
	home_l = strlen(home);

	for (; *inp!='\0'; inp++) {
		if (QUOTE(*inp)) {
			inp = strchr(inp+1, *inp);
			continue;
		}
		if (*inp == '~') {
			repl_str("~", home, inp);
			inp += home_l - 1;
		}
	}
}

int main()
{
	char *inp; /* points to where input is done on main command line */
	char cmd[LINE_MAX]; /* main command line */

	inp = cmd;

	setbuf(stdout, NULL);

	while (1) {
		printf("%s", ps1);
		fgets(inp, LINE_MAX, stdin);
		while (!is_quoted(cmd)) {
			inp = strchr(inp, '\n');
			printf("%s", ps2);
			fgets(inp, LINE_MAX, stdin);
		}
		inp = cmd;
		tilde_exp(inp);
		is_bg(inp);
		eval(inp);
		memset(inp, 0, LINE_MAX);
	}
	
}
