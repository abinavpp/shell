#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <glob.h>

#include "alias.h"
#include "util.h"
#include "jobs.h"

#define ESCAPE(c)		((c) == '\\')
#define CHILD_DELIM(c)	((c) == ' ')
#define PARENT_DELIM(c)	((c)=='|' || (c)==';' || (c)=='&')
#define GRAND_DELIM(c)	((c)=='\n')
#define QUOTE(c)		((c)=='\'' || (c)=='\"')

#define DO_REDIR	1	/* redirection */
#define DO_PP		2	/* post_process cmdline */
#define DO_UBLK		4	/* signal unblock */
#define DO_SETPG	8	/* set process group */
#define DO_SETTERM  16	/* set terminal for process */
#define DO_ALL		(DO_REDIR|DO_PP|DO_UBLK|DO_SETPG|DO_SETTERM)

static void prompt();
static void tokenize(char **inp, char *delim);
static void aliasize(char *start_arg, char *delim, char **inp);
static char parse_cmd(char **inp, char **cmd, int start_ind);
static void do_redir(char **cmd);
static void redir_me(int redir_fd, char *redir_dst, int ap_flag);
static int builtin(char **cmd);
static void exec_me(char **cmd, int state);
static void pipe_me(char **cmd, char **inp, char *delim);
static void eval(char *inp);
static int is_quoted(char *str);
static void postproc_cmdline(char **cmd);
static void preproc_cmdline(char *inp);

/* head of the singly linked list of alias blacklist to avoid nasty
 * recurisive alias cases, like alias "ls=ls;ls" */
static alias *al_blist;

/* this marks the location at cmdline till alias blacklisting is done */
static char *al_deadend;

/* nah no ps3 or ps4 in my shell */
static char ps1[PRMT_MAX] = "ASH>";
static char ps2[PRMT_MAX] = " >";

/* globbing global member, freed in builtins */
static glob_t glob_res;

/* pushes inp to end of token and nullifies that byte */
static void tokenize(char **inp, char *delim)
{

	for (; ; (*inp)++) {
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
 * inp and delim will point accordingly but start_arg remains */
static void aliasize(char *start_arg, char *delim, char **inp)
{
	int len;
	alias *al_cur;
	char al_arg[ARG_MAX] = {0};

	if (start_arg >= al_deadend) {
		/* no blacklisting since we crossed the deadend, so freeing the list */
		al_lin_free(&al_blist);
	}

	while (al_cur = is_alias(start_arg )) {
		if (al_lin_src(al_blist, start_arg) == NULL) {
			/* this is not a blacklisted one, hence alias it */
			al_lin_ins(&al_blist, start_arg); /* but from now till deadend it is */
			len = strlen(start_arg);
			strncpy(al_arg, start_arg, len);
			**inp = *delim; /* so that repl_str works till actual '\0' of cmdline */
			repl_str(al_arg, al_cur->trans, start_arg);

			if (al_deadend == NULL) /* if deadend uninitialized */
				al_deadend = start_arg + strlen(al_cur->trans);
			else
				al_deadend = al_deadend + ALIAS_DIFF(al_cur);

			/* this MUST happen so that we leave things how 
			 * the parser wants after expansion */
			*inp = *inp - len; 
			/* 
			 * the MODIFIED token after alias is delimited with
			 * '\0'and its following delim becomes the new delim,
			 *  inp points to end of first token after alias transition 
			*/
			tokenize(inp, delim); 
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

	i = start_ind;

	if (!inp || !(*inp) || !(**inp)) /* NULL string */
		return 0;

	while (**inp == ' ') /* Eating leading spaces */
		(*inp)++;

	for (start_arg=*inp; **inp!='\0'; (*inp)++) {
		/* quoted arg is taken as one */
		if (QUOTE(**inp)) {
			delim = **inp;
			/* keeping the leading quote in cmd[i] for post_proc as 
			 * a signal to ignore special characters */
			cmd[i++] = (*inp);
			*inp = strchr((*inp)+1, delim);
			**inp = '\0'; /* cmd[i] points till here */
			start_arg = *inp + 1; /* points to the next arg */
		}
		if (ESCAPE(**inp)) { /* skips the escaped chars */
			if (!GRAND_DELIM(*(*inp+1)))
				*inp += 1;
			continue;
		}
		if (CHILD_DELIM(**inp) || PARENT_DELIM(**inp) || GRAND_DELIM(**inp)) {
			/* now this might be one cmd or an arg of a cmd */
			delim = **inp;
			**inp = '\0'; /* now cmd[i] can point till here */

			/* 
			 * start_arg can be NULL, for eg if multiple spaces are dealt
			 * with in which the above line nullify start_arg itself which
			 * is an 'ok' way to skip the bloody spaces 
			 * */

			if (*start_arg) {
				/* alias only the cmd, ie cmd[0], not its args */
				if (!i && is_alias(start_arg)) {
					aliasize(start_arg, &delim, inp);
				}
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
	/* printf("|%s|%s|%s|%s\n", cmd[0], cmd[1], cmd[2], cmd[3]); */
	return delim; /* for verifying if this to be piped or bg'd etc */
}

/* return type of redir and sets ap_flag (append flag) */
static void do_redir(char **cmd)
{
	int redir_fd, ap_flag, null_flag;
	char *redir_tok, *redir_dst;

	null_flag = 1;
	while (cmd && *cmd) {
		redir_tok = int_till_txt(*cmd, &redir_fd);
		if (redir_fd >= 1) {
			if (*redir_tok == '>') {
				/* redir_dst can be filename or can be 
				 * NULL(which will fail at redir_me) */
				redir_dst = *(cmd+1);
				ap_flag = (*(redir_tok+1) == '>') ? 1 : 0;
				/* append flag and dst set */
				redir_me(redir_fd, redir_dst, ap_flag);
				if (null_flag) {
					*cmd = NULL;
					null_flag = 0;
				}
			}
		}
		cmd = cmd+1; /* next arg */
	}
}

/* sets up fd to do the redir */
static void redir_me(int redir_fd, char *redir_dst, int ap_flag)
{
	int dst_fd;

	if (!redir_dst) {
		ERRMSG("Error redirecting\n");
		return;
	}

	if (!ap_flag) {/* don't append the output */
		if ((dst_fd=creat(redir_dst, 0644)) < 0) {
			ERR("creat");
			return;
		}
	}

	else {/* append the output */
		if ((dst_fd=open(redir_dst, O_RDWR | O_APPEND)) < 0) {
			ERR("open");
			return;
		}
	}

	/* now exec can be done */
	if (dup2(dst_fd, redir_fd) < 0) { 
		ERR_EXIT("dup2");
	}

	clean_up("f", dst_fd);
}

/* does shell builtins, hopefully */
static int builtin(char **cmd)
{
	int err;

	if (cmd[0]) {
		switch (*cmd[0]) {
		case 'a' :
			if (!strcmp(cmd[0], "alias")) {
				postproc_cmdline(cmd);
				alias_me(cmd);
				globfree(&glob_res);
				return 1;
			}
			break;
		case 'b' :
			if (!strcmp(cmd[0], "bg")) {
				do_bgfg(cmd, BG);
				return 1;
			}
			break;
		case 'c' :
			if (!strcmp(cmd[0], "cd")) {
				if (cmd[1]==NULL) {
					chdir(getenv("HOME"));
					return 1;
				}

				postproc_cmdline(cmd);

				if (chdir(cmd[1])<0 )
					ERR("chdir");

				globfree(&glob_res);
				return 1;
			}
		case 'f' :
			if (!strcmp(cmd[0], "fg")) {
				do_bgfg(cmd, FG);
				return 1;
			}
			break;
		case 'j' :
			if (!strcmp(cmd[0], "jobs")) {
				printjobs();
				return 1;
			}
			break;
		case 'v' :
			if (!strcmp(cmd[0], "var")) {
				return 1;
			}
			break;
		case 'e' :
			if (!strcmp(cmd[0], "exit")) {
				fprintf(stdout, "Exiting\n");
				exit(EXIT_SUCCESS);
			}
		}
	}
	return 0;
}

/* unqoutes quoted arg, removes backslash of escapes, globs wildcards */
static void postproc_cmdline(char **cmd)
{
	int i;
	char *tok, **after_glob;

	for (; NOTNULL(cmd); cmd=cmd+1) {
		if (QUOTE(**cmd)) {
			*cmd += 1;
		}

		else {
			glob(*cmd, 0, NULL, &glob_res);
			if (glob_res.gl_pathc > 0) {
				/* we got matching files/dir with wildcard */
				for (i=1, after_glob = cmd+glob_res.gl_pathc;
					   	NOTNULL(cmd+i);	i++, after_glob += 1) {
					/* args after glob exp are copied to end to make 
					 * room for glob expansion in cmd[][] */
					*after_glob = *(cmd + i);
				}
				*after_glob = NULL; /* the way execvp likes it */

				/* apparent glob api maintains the results in the data segment
				 * so pointing to cmd pointing to it is not the worst idea i guess */
				for (i=0; i<glob_res.gl_pathc; i++, cmd++)
					*cmd = glob_res.gl_pathv[i];
			}

			else {
				/* removing \ */
				for (tok=*cmd; (tok=strchr(tok, '\\')); tok++)
					repl_str("\\","", tok);
			}
		}
	}
}

/* the function name explains for itself
 * facilitates pgrp and signal unblock */
static pid_t fork_and_exec(char **cmd, int flags,
		pid_t pgid, sigset_t msk)
{
	pid_t pid;
	pid_t p = 0;

	pid = fork();
	switch (pid) {
	case -1 : 
		ERR_EXIT("fork");
		break;
	case 0  :
		SIG_TGL(SIGTTOU, SIG_DFL);
		SIG_TGL(SIGTTIN, SIG_DFL);
		pid = getpid();
		if (flags & DO_REDIR)
			do_redir(cmd);

		if (flags & DO_PP)
			postproc_cmdline(cmd);

		if (flags & DO_UBLK)
			MASK_ALLSIG(SIG_UNBLOCK, msk);

		if (flags & DO_SETPG) {
			 setpgid(0, pgid);
		}
		if (flags & DO_SETTERM) {
			if (!pgid) {
				while (tcgetpgrp(TERMFD) != pid)
					; /* tcpgrp will be pid itseld then */
			}
			else {
				while (tcgetpgrp(TERMFD) != pgid)
					; /* tcpgrp as per arg in function */
			}
		}
		execvp(cmd[0], cmd);
		ERR_EXIT("execvp");
		break;
	default :
		break;
	}
	return pid;
}

/* regular fork and exec routine with a shell twist */
static void exec_me(char **cmd, int state)
{
	pid_t pid;
	jobs **job;
	int status;
	sigset_t  msk;

	if (!cmd || !(*cmd) || !(**cmd))
		return;

	if (builtin(cmd))
		return;

	MASK_SIG(SIG_BLOCK, SIGCHLD, msk);

	pid = fork_and_exec(cmd, (state == FG) ? 
			DO_ALL : (DO_ALL^DO_SETTERM), 0, msk);
	job = addjob(pid, state, cmd);

	if(state == FG) {
		wait_fg(job);
	}
	else {
		printf("[%d] %d\t%s\n",(*job)->jid, (*job)->pid, (*job)->cmdline);
	}

	MASK_SIG(SIG_UNBLOCK, SIGCHLD, msk);
}

/* piper */
static void pipe_me(char **cmd, char **inp, char *delim)
{
	int pipe_fd[2], stdin_fd, stdout_fd, i;
	char pipe_cmds[ARG_MAX];
	pid_t pgid = 0;
	sigset_t msk;
	jobs **job;

	if (!cmd || !(*cmd) || !(**cmd)) {
		return;
	}

	stdin_fd = dup(STDIN_FILENO); /* saves the orig stdin */
	stdout_fd = dup(STDOUT_FILENO); /* saves the orig stdout */

	MASK_SIG(SIG_BLOCK, SIGCHLD, msk);

	for (i=0; *delim == '|';) { /* there is more to pipe */
		if (pipe(pipe_fd) < 0)
			ERR("pipe");

		if (dup2(pipe_fd[1], STDOUT_FILENO) < 0)
			ERR_EXIT("dup2");
		clean_up("f", pipe_fd[1]);

		if (!pgid) {
			pgid = fork_and_exec(cmd, DO_ALL, 0, msk);
			job = addjob(pgid, FG, cmd); /* so (*job)->pid[0] = pgid */
		}
		else {
			(*job)->pid[++i] = fork_and_exec(cmd, DO_ALL^DO_SETTERM, pgid, msk);
			strcat((*job)->cmdline, " | ");
			stringify(pipe_cmds, cmd);
			strcat((*job)->cmdline, pipe_cmds);
		}
		/* sets up for 'read from pipe' for next child in loop */
		if (dup2(pipe_fd[0], STDIN_FILENO) < 0)
			ERR_EXIT("dup2");
		clean_up("f", pipe_fd[0]);

		*delim = parse_cmd(inp, cmd, 0);
	}
	/* restores the orig stdout for the final exec */
	if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
		ERR_EXIT("dup2");
	}
	(*job)->pid[++i] = fork_and_exec(cmd, DO_ALL^DO_SETTERM, pgid, msk);
	strcat((*job)->cmdline, " | ");
	stringify(pipe_cmds, cmd);
	strcat((*job)->cmdline, pipe_cmds);
	(*job)->pid[++i] = 0;

	if (dup2(stdin_fd, STDIN_FILENO) < 0) { /* restores the orig stdin*/
		ERR_EXIT("dup2");
	}
	wait_fg(job);
	MASK_SIG(SIG_UNBLOCK, SIGCHLD, msk);

	clean_up("ff", stdin_fd, stdout_fd);
}

/* where it all starts */
static void eval(char *inp)
{
	char *cmd[ARG_MAX] = {0}; /* main cmd array for exec */
	char delim; /* delim between cmds */

	do {
		if ((delim = parse_cmd(&inp, cmd, 0)) == '|')
			pipe_me(cmd, &inp, &delim);

		else 
			exec_me(cmd, delim=='&' ? BG : FG);

	} while (!GRAND_DELIM(delim) && !GRAND_DELIM(*inp));
	/* main cmdline end in '\n' , i guess.. */
	al_deadend = NULL; /* re-initiating it for a new cmdline */
	al_lin_free(&al_blist);
}

/* checks if PS2 is required */
static int is_quoted(char *str)
{
	while (*str != '\0') {
		while (*str!='\0' && !QUOTE(*str))
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
	char home[PATH_MAX] = {0};
	int home_l;

	strncpy(home, getenv("HOME"), strlen(getenv("HOME")));
	home_l = strlen(home);

	for (; *inp!='\0'; inp++) {
		if (QUOTE(*inp)) {
			inp = strchr(inp+1, *inp);
			continue;
		}
		if (*inp == '~' && !ESCAPE(*(inp-1))) {
			repl_str("~", home, inp);
			inp += home_l - 1;
		}
	}
}

/* eats trailing spaces and calls tilde expansion */
static void preproc_cmdline(char *inp)
{
	char *i;

	for (i=inp + (strlen(inp)-2); *i==' '; i--)
		;
	int len_pat , len_rep, len_diff;
	*(i+1) = '\n';
	tilde_exp(inp);
}

static void prompt()
{
	char cmd[LINE_MAX] = {0}; /* main command line */
	char *inp; /* points to where input is done on main command line */

	inp = cmd;

	while (1) {
		printf("%s", ps1);
		fgets(inp, LINE_MAX, stdin);
		while (!is_quoted(cmd)) {
			inp = strchr(inp, '\n');
			printf("%s", ps2);
			fgets(inp, LINE_MAX, stdin);
		}
		inp = cmd;
		preproc_cmdline(inp);
		eval(inp);
		memset(inp, 0, LINE_MAX);
	}

}

void main()
{
	setbuf(stdout, NULL);
	dup2(1, TERMFD);
	signal_init();
	prompt();
	exit(EXIT_FAILURE);
}
