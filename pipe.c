#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>
#include <glob.h>

#include "mark.h"
#include "alias.h"
#include "util.h"
#include "jobs.h"

/*
 * parser specific macros
 */
#define IS_ESCAPE(c)		((c) == '\\')
#define IS_CHILD_DELIM(c)	((c) == ' ')
#define IS_BG_DELIM(c)		((c) == '&')
#define IS_PIPE_DELIM(c)	((c) == '|')
#define IS_PARENT_DELIM(c)	((c)=='|' || (c)==';' || (c)=='&')
#define IS_GRAND_DELIM(c)	((c)=='\n')
#define IS_REDIR_DELIM(c)	((c) == '>')
#define IS_REDIRTOFD_DELIM(inp)		(*(inp)=='&' && *((inp)-1)=='>')
#define IS_REDIRAPPEND_DELIM(inp)	(*(inp)=='>' && *((inp)-1)=='>')
#define IS_QUOTE(c)			((c)=='\'' || (c)=='\"')
#define IS_SUBSHELL_OPEN(c)	((c) == '(')
#define IS_SUBSHELL_CLOSE(c)	((c) == ')')

#define TERM_DELIM_INIT		{'|', ';', '&', '\n', '\0'}
#define SS_DELIM_INIT		{'(', ')'}
#define CHILD_DELIM			' '
#define SUBSHELL_OPEN		'('
#define SUBSHELL_CLOSE		')'
#define ESCAPE 				'\\'
#define GRAND_DELIM 		'\n'
#define PIPE 				'|'

/*
 * flags for parse_cmd
 */
#define PARSE_DONTFILLCMD	1 /* dont fill cmd array */
#define PARSE_WHOLELINE		2 /* parse till GRAND_DELIM only */
#define PARSE_DONTPRINT		4 /* parser wont print to stdout */

/*
 * parse_cmd return flags
 */
#define PARSERET_SUBSHELL	1
#define PARSERET_UNCLOSED	2
#define PARSERET_SYNERR		4

/*
 * preproc_cmdline flags
 */
#define PREPRO_CREND		1 /* append '\n' at end */
#define PREPRO_NULLBEG		2 /* add '\0' before start */

/* fork_and_exec flags */
#define FE_REDIR	1	/* redirection */
#define FE_PP		2	/* post_process cmdline */
#define FE_UBLK		4	/* signal unblock */
#define FE_SETPG	8	/* set process group */
#define FE_SETTERM  16	/* set terminal for process */
#define FE_ALL		(FE_REDIR|FE_PP|FE_UBLK|FE_SETPG|FE_SETTERM)

/*
 * this flags as of now is only set to signal if the shell
 * is a subshell or not ,global to jobs.c for safer term
 * control
 * */
int subshell_flag;

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

/*
 * searches from str len bytes for the first occurence
 * of any one char in array c of size n. The char is treated
 * as a special delim, so the ones in quotes and escaped ones
 * are ignored. returns the location if found, else NULL
 */
static char *strdelimvec(char *str, int len, int c[], int n)
{
	int i;
	char *end = str + len;

	for(; str && str<=end; str++) {
		if (IS_QUOTE(*str) && !IS_ESCAPE(*(str-1))) {
			if (!(str = astrchr(str+1, *str, 1)))
				break;
		}
		else {
			for (i=0; i<n; i++) {
				if (*str == c[i] && !IS_ESCAPE(*(str-1)))
					return str;
			}
		}
	}
	return NULL;
}

/* 1 if subshell closed else zero */
static int is_ss_closed(char **str)
{
	int ss_pair = 1;
	int ss_delim[] = SS_DELIM_INIT;

	for ((*str)++; (*str = strdelimvec(*str, strlen(*str),
					ss_delim,sizeof(ss_delim) / sizeof(int)));) {
		if (**str == SUBSHELL_OPEN)
			++ss_pair;
		else if (ss_pair == 1) {
			ss_pair--;
			break;
		}
		else
			ss_pair--;
		(*str)++;
	}

	return ss_pair==0 ? 1 : 0;
}

/* reinitializes the blacklisting helpers fresh aliasizing */
static inline void al_reinit_blist()
{
	al_deadend = NULL;
	if (al_blist)
		al_lin_free(&al_blist);
}

/* the start of iterative alias expansion fixed at start_arg location.
 * inp and delim are from parse_cmd since this is closely tied with it */
static int aliasize(char *start_arg, char *delim, char **inp)
{
	int len;
	alias *al_cur;
	char al_arg[ARG_MAX] = {0};

	if (start_arg >= al_deadend) {
		/* no blacklisting since we crossed the deadend,
		 * so freeing the list and al_deadend reinitialized */
		al_reinit_blist();
	}

	if ((al_cur = is_alias(start_arg )) &&
		 (al_lin_src(al_blist, start_arg) == NULL)) {
		/* this is not a blacklisted one, hence alias it */
		/* but from now till deadend it is */
		al_lin_ins(&al_blist, start_arg);

		len = strlen(start_arg);
		astrcpy(al_arg, start_arg, len, 1);
		**inp = *delim; /* so that repl_str works till actual
						   delim of cmdline */
		repl_str(al_arg, al_cur->trans, start_arg);

		if (al_deadend == NULL)
			al_deadend = start_arg + strlen(al_cur->trans);
		else
			al_deadend = al_deadend + ALIAS_DIFF(al_cur);
	}
	else {
		goto dont_aliasize;
	}

	return 0;

dont_aliasize :
	return -1;
}

/*
 * the god-forsaken parser!!!
 * inp references the main input string, cmd is the array to be filled
 * for execve from index start_ind. plflags are parser flags, (see macros
 * above), and rflags are written as per the outcome of the parsing.
 * Quoted args have leading quote in the cmd argument as an FYI for
 * postproc_cmdline. If subshell, then cmd[0] will point to the whole
 * subshell line, from '(' to ')', as an FYI for fe_or_ss.
 * Aliasize is also called by this. If everything goes A-ok, then
 * it returns the delim else -1. Check rflags and return value
 * all the time.
 */
static char parse_cmd(char **inp, char **cmd, int start_ind,
		int pflags, int *rflags)
{
	int i = start_ind;
	char delim;
	char *start_arg;

	if (!inp || !(*inp) || !(**inp)) {
		printf("Godnamit\n");
		goto hell;
	}

	while (**inp == ' ')
		(*inp)++;

	*rflags = 0;
	for (start_arg=*inp; *inp; (*inp)++) {
		if (IS_QUOTE(**inp)) {
			delim = **inp;
			/* keeping the leading quote in cmd[i] for post_proc as
			 * a signal to ignore special characters */
			if (!(pflags & PARSE_DONTFILLCMD))
				cmd[i++] = start_arg;
			else
				i++;

			if ((*inp = astrchr((*inp)+1, delim, 1)))
				**inp = '\0';
			else
				goto unclosed;
			start_arg = *inp + 1;
		}
		else if (IS_ESCAPE(**inp)) {
			if (!IS_GRAND_DELIM(*(*inp+1)))
				*inp += 1;
			else
				goto unclosed;
		}
		else if (IS_SUBSHELL_OPEN(**inp)) {
			char *ss_close;
			int term_delim[] = TERM_DELIM_INIT;

			if (i || *(*inp-1)) {
				/* die if this not a start of new command */
				if (i) {
					goto hell;
				}

				/* die if prev_char is an escaped */
				else if (*(*inp-2) && IS_ESCAPE(*(*inp-2)))
					goto hell;

				/* die if prev_char is not proper delim */
				else if (!(IS_PARENT_DELIM(*(*inp-1))
								|| IS_CHILD_DELIM(*(*inp-1)) ))
					goto hell;
			}

			ss_close = *inp;
			if (is_ss_closed(&ss_close)) {
				*(ss_close++) = ' ';
				if (!(ss_close = strdelimvec(ss_close, strlen(ss_close),
								term_delim,
								sizeof(term_delim) / sizeof(int))))
					goto unclosed;

				(*rflags) |= PARSERET_SUBSHELL;

				delim = *ss_close;
				*ss_close = '\n'; /* to make subshell happy */

				/* signals that this is a subshell by the leading '('
				 * from now on , i MUST BE ZERO!!! */
				if (!(pflags & PARSE_DONTFILLCMD))
					cmd[i++] = *inp;
				else
					i++;
				*inp = ss_close + 1;
				goto done;
			}
			else
				goto unclosed;
		}
		else if (IS_CHILD_DELIM(**inp) || IS_PARENT_DELIM(**inp) || IS_GRAND_DELIM(**inp)) {

			if (IS_REDIRTOFD_DELIM(*inp))
				continue;

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
					if (aliasize(start_arg, &delim, inp) == 0) {
						*inp = start_arg - 1;
						continue;
					}
				}
				if (!(pflags & PARSE_DONTFILLCMD))
					cmd[i++] = start_arg;
				else
					i++;
			}

			if (IS_PARENT_DELIM(delim) || IS_GRAND_DELIM(delim)) {

				if (!(pflags&PARSE_WHOLELINE) || IS_GRAND_DELIM(delim)) {
					(*inp)++;
					goto done;
				}
			}
			start_arg = *inp + 1;
		}
	}
done :
	if (!(pflags & PARSE_DONTFILLCMD))
		cmd[i] = NULL; /* to make argvp happy */
#ifdef DEBUG_ON
	for (i=0; cmd[i]; i++)
		printf("%s|", cmd[i]);
	printf("%s with delim %d\n", "NULL", delim);
#endif
	return delim; /* for verifying if this to be piped or bg'd etc */

unclosed :
	(*rflags) |= PARSERET_UNCLOSED;
	if (!(pflags & PARSE_DONTPRINT))
		fprintf(stderr, "SHELL : cmdline unclosed\n");
	return -1;

hell:
	(*rflags) |= PARSERET_SYNERR;
	if (!(pflags & PARSE_DONTPRINT))
		fprintf(stderr, "SHELL : syntax error\n");
	return -1;
}

/* unqoutes quoted arg, removes backslash of escapes, globs wildcards */
static void postproc_cmdline(char **cmd)
{
	int i;
	char *tok, **after_glob;

	for (; NOTNULL(cmd); cmd=cmd+1) {
		if (IS_QUOTE(**cmd)) {
			*cmd += 1;
		}

		else {
			glob(*cmd, GLOB_TILDE | GLOB_BRACE, NULL, &glob_res);
			if (glob_res.gl_pathc > 0) {
				/* we got matching files/dir with wildcard */
				for (i=1, after_glob = cmd+glob_res.gl_pathc;
					   	NOTNULL(cmd+i);	i++, after_glob += 1) {
					/* args after glob exp are copied to end to make
					 * room for glob expansion in cmd[][] */
					*after_glob = *(cmd + i);
				}
				*after_glob = NULL; /* the way execvp likes it */

				/*  glob api maintains the results in the heap segment
				 * so making cmd point to it is not the worst idea i guess */
				for (i=0; i<glob_res.gl_pathc; i++, cmd++)
					*cmd = glob_res.gl_pathv[i];
			}

			else {
				/* removing \ */
				for (tok=*cmd; (tok=strchr(tok, ESCAPE)); tok++)
					repl_str("\\","", tok);
			}
		}
	}
}

/* @ startup of shell */
static void shell_init()
{
	setbuf(stdout, NULL);
	if (dup2(1, TERMFD) < 0)
		ERR_EXIT("dup2");
	if (fcntl(TERMFD, F_SETFD, FD_CLOEXEC) < 0)
		ERR_EXIT("fcntl");
	signal_init();
	setpgid(0, 0);
	tcsetpgrp(TERMFD, getpid());
}

/* @ death of shell */
static void shell_cleanup()
{
	al_free();
	mark_free();
	job_free();
	clean_up("f", TERMFD);
}

/* redir_fd is redirected to redir_dst for append or not as per
 *  ap_flag */
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

/* sets up the fds for the current cmd to be exec'd */
static void do_redir(char **cmd)
{
	int redir_fd, redir_tofd, ap_flag, null_flag = 1;
	char *redir_tok, *redir_dst;

	while (NOTNULL(cmd)) {
		redir_tok = int_till_txt(*cmd, &redir_fd);
		if (redir_fd >= 1 && IS_REDIR_DELIM(*(redir_tok++))) {

			if (IS_REDIRTOFD_DELIM(redir_tok)) {
				int_till_txt(redir_tok+1, &redir_tofd);

				if (!redir_tofd || dup2(redir_tofd, redir_fd) < 0)
					ERRMSG("Error redirecting\n");
			}
			/* redir_dst can be filename or can be
			 * NULL(which will fail at redir_me) */
			else {
				redir_dst = *(cmd+1);
				ap_flag = IS_REDIRAPPEND_DELIM(redir_tok)
					? 1 : 0;
				/* append flag and dst set */
				redir_me(redir_fd, redir_dst, ap_flag);
			}
			if (null_flag) {
				*cmd = NULL;
				null_flag = 0;
			}
		}
		cmd = cmd+1; /* next arg */
	}
}

/* what cd tuns */
static void do_cd(char **cmd)
{
	if (!cmd[1]) {
		chdir(getenv("HOME"));
		return;
	}
	postproc_cmdline(cmd);
	if (chdir(cmd[1]) < 0)
		ERR("chdir");
	globfree(&glob_res);
}

/* does shell builtins, hopefully */
static int builtin(char **cmd)
{
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
				do_cd(cmd);
				return 1;
			}
			break;

		case 'g' :
			if (!strcmp(cmd[0], "gt")) {
				goto_mark(cmd);
				return 1;
			}
			break;

		case 'm' :
			if (!strcmp(cmd[0], "mk")) {
				mark_me(cmd);
				return 1;
			}
			break;

		case 'e' :
			if (!strcmp(cmd[0], "exit")) {
				shell_cleanup();
				fprintf(stdout, "Exiting\n");
				exit(EXIT_SUCCESS);
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

		case 'u' :
			if (!strcmp(cmd[0], "unalias")) {
				postproc_cmdline(cmd);
				unalias_me(cmd);
				globfree(&glob_res);
				return 1;
			}
			if (!strcmp(cmd[0], "unmk")) {
				unmark_me(cmd);
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

/* the function name explains for itself
 * facilitates pgrp, tc, signal unblock */
static pid_t fork_and_exec(char **cmd, int flags,
		pid_t pgid, sigset_t msk)
{
	pid_t pid;

	if (subshell_flag) {
		/* if subshell then every forks belong to the same pg
		 * sharing the same terminal */
		flags &= ~(FE_SETTERM|FE_SETPG);
	}
	pid = fork();
	switch (pid) {
	case -1 :
		ERR_EXIT("fork");
		break;

	case 0  :
		SIG_TGL(SIGTTOU, SIG_DFL);
		SIG_TGL(SIGTTIN, SIG_DFL);
		pid = getpid();

		if (flags & FE_SETTERM) {
			if (!pgid) {
				while (tcgetpgrp(TERMFD) != pid)
					; /* tcpgrp will be pid itseld then */
			}
			else {
				while (tcgetpgrp(TERMFD) != pgid)
					; /* tcpgrp as per arg in function */
			}
		}
		if (flags & FE_REDIR)
			do_redir(cmd);

		if (flags & FE_PP)
			postproc_cmdline(cmd);

		if (flags & FE_UBLK)
			MASK_ALLSIG(SIG_UNBLOCK, msk);

		if (flags & FE_SETPG) {
			 if (setpgid(0, pgid) < 0)
				 perror("setpgid");
		}

		if (cmd[0]) {
			if (execvp(cmd[0], cmd) < 0)
				ERR_EXIT("execvp");
		}
		else{
			_exit(EXIT_SUCCESS);
		}
		break;

	default :
		/*
		 * force setpgid here, sometimes the parent, ie the shell,
		 * goes in a hurry and the child won't have a chance to setpgid
		 * and that would create havoc especially in pipelines
		 * */
		if (flags & FE_SETPG) {
			 if (setpgid(pid, pgid ?
						 pgid : pid) < 0)
				 perror("setpgid");
		}
		/* so that child don't spin too much with tcsetpgrp */
		if (flags & FE_SETTERM) {
			tcsetpgrp(TERMFD, pgid ? pgid : pid);
		}
		break;
	}
	return pid;
}

static void eval(char *);
/* subshell is just a simple fork and a call to eval,
 * the forked shell will have the global subshell_flag set to one */
static pid_t run_subshell(char *ss_inp, int flags, pid_t pgid)
{
	pid_t pid;

	pid = fork();
	switch (pid) {
	case -1 :
		ERR_EXIT("fork");
		break;

	case 0 :
		subshell_flag = 1;
		pid = getpid();
		SIG_TGL(SIGTSTP, SIG_DFL);
		SIG_TGL(SIGINT, SIG_DFL);

		if (flags & FE_SETTERM) {
			if (!pgid) {
				while (tcgetpgrp(TERMFD) != pid)
					;
			}
			else {
				while (tcgetpgrp(TERMFD) != pgid)
					;
			}
		}
		if (flags & FE_SETPG) {
			if (setpgid(0, pgid) < 0)
				perror("setpgid");
		}
		eval(ss_inp);
		exit(EXIT_SUCCESS);

	default :
		if (flags & FE_SETPG) {
			if (setpgid(pid, pgid ?
						pgid : pid) < 0)
				perror("setpgid");
		}
		if (flags & FE_SETTERM) {
			tcsetpgrp(TERMFD, pgid ? pgid : pid);
		}
		break;
	}
	return pid;
}

/* fork_and_exec or run_subshell as per the 1st char of cmd[0] */
static pid_t fe_or_ss(char **cmd,
		int flags, pid_t pgid, sigset_t msk)
{
	if (!NOTNULL(cmd))
		return -1;
	if (*cmd[0] == SUBSHELL_OPEN) {
		return run_subshell(cmd[0] + 1, flags, pgid);
	}
	else {
		return fork_and_exec(cmd, flags, pgid, msk);
	}
}

/* regular fork and exec routine with a shell twist */
static void exec_me(char **cmd, int state)
{
	pid_t pid;
	jobs **job;
	sigset_t  msk;

	if (!cmd || !(*cmd) || !(**cmd))
		return;

	if (builtin(cmd))
		return;

	MASK_SIG(SIG_BLOCK, SIGCHLD, msk);

	pid = fe_or_ss(cmd,
			(state == FG) ? FE_ALL : (FE_ALL^FE_SETTERM), 0, msk);
	job = addjob(pid, state, cmd);

	if(state == FG) {
		wait_fg(job);
	}
	else {
		shell_printf(SHELLPRINT_CHKSS | SHELLPRINT_CHKTERM,
				"[%d] %d\t%s\n",(*job)->jid, (*job)->pid[0], (*job)->cmdline);
	}

	MASK_SIG(SIG_UNBLOCK, SIGCHLD, msk);
}

/* piper */
static void pipe_me(char **cmd, char **inp, char *delim)
{
	int pipe_fd[2], stdin_fd, stdout_fd, i, parse_rflags = 0;
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

	for (i=0; IS_PIPE_DELIM(*delim);) { /* there is more to pipe */
		if (pipe(pipe_fd) < 0)
			ERR("pipe");

		if (dup2(pipe_fd[1], STDOUT_FILENO) < 0)
			ERR_EXIT("dup2");
		clean_up("f", pipe_fd[1]);

		if (!pgid) {
			pgid = fe_or_ss(cmd, FE_ALL, 0, msk);
			job = addjob(pgid, FG, cmd); /* so (*job)->pid[0] = pgid */
		}
		else {
			(*job)->pid[++i] = fe_or_ss(cmd, FE_ALL^FE_SETTERM, pgid, msk);
			strcat((*job)->cmdline, " | ");
			stringify(pipe_cmds, cmd);
			strcat((*job)->cmdline, pipe_cmds);
		}
		/* sets up for 'read from pipe' for next child in loop */
		if (dup2(pipe_fd[0], STDIN_FILENO) < 0)
			ERR_EXIT("dup2");
		clean_up("f", pipe_fd[0]);

		*delim = parse_cmd(inp, cmd, 0, 0, &parse_rflags);
	}
	/* restores the orig stdout for the final exec */
	if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
		ERR_EXIT("dup2");
	}
	(*job)->pid[++i] = fe_or_ss(cmd, FE_ALL^FE_SETTERM, pgid, msk);
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
	int parse_rflags = 0; /* maybe useful in the future */

#ifdef DEBUG_ON
	printf("%d evaluating ", getpid());
	prints(inp);
#endif
	do {
		parse_rflags = 0;
		if ((delim = parse_cmd(&inp, cmd, 0, 0, &parse_rflags)) == PIPE) {
			pipe_me(cmd, &inp, &delim);
		}
		else if (delim == -1) {
			break;
		}
		else {
			exec_me(cmd,IS_BG_DELIM(delim) ? BG : FG);
		}

	} while (!IS_GRAND_DELIM(delim) && !IS_GRAND_DELIM(*inp));
	/* main cmdline end in '\n' , i guess.. */

	al_reinit_blist();
}

/* for checking if PS2 is required, returns true on unclosed quotes,
 * subshells, escaped \n */
static int is_closed(char *str)
{

	char cmd[LINE_MAX + 1] = {0};
	char *cmdline = cmd + 1;
	int parse_rflags = 0;

	astrcpy(cmdline, str, strlen(str), 1);
	al_reinit_blist();
	if (parse_cmd(&cmdline, NULL, 0, PARSE_DONTFILLCMD|PARSE_WHOLELINE
				|PARSE_DONTPRINT,&parse_rflags) == -1) {
		if (parse_rflags & PARSERET_UNCLOSED)
			return 0;
	}
	return 1;
}

/* eats trailing spaces and calls tilde expansion */
static void preproc_cmdline(char *inp, int flags)
{
	char *i;

	if (flags & PREPRO_CREND) {
		for (i=inp + (strlen(inp)-1); *i==' '; i--)
			;
		if (*i)
			*(i+1) = GRAND_DELIM;
		else
			*i = GRAND_DELIM;
	}

	if (flags & PREPRO_NULLBEG) {
		*(inp - 1) = 0;
	}
}

static void prompt()
{
	char cmdline[LINE_MAX + 1] = {0}; /* main command line */
	char *inp; /* points to where input is done on main command line */

	inp = cmdline + 1; /* ok, I am scared about parsing */

	while (1) {
		if (isatty(STDIN_FILENO))
			printf("%s", ps1);

		fgets(inp, LINE_MAX, stdin);
		while (!is_closed(cmdline + 1)) {
			if (!isatty(STDIN_FILENO))
				break;
			if ((inp = strchr(inp, GRAND_DELIM)) && IS_ESCAPE(*(inp-1)))
				inp--;
			printf("%s", ps2);
			fgets(inp, LINE_MAX, stdin);
		}
		inp = cmdline + 1;
		preproc_cmdline(inp, PREPRO_CREND | PREPRO_NULLBEG);
		eval(inp);
		memset(cmdline, 0, LINE_MAX);

		if (!isatty(STDIN_FILENO))
			break;
	}

}

int main()
{
	shell_init();
	prompt();
	shell_cleanup();
	exit(EXIT_FAILURE);
}
