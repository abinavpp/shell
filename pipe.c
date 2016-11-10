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

/* parser specific macros */
#define IS_ESCAPE(c)		((c) == '\\')
#define IS_CHILD_DELIM(c)	((c) == ' ')
#define IS_BG_DELIM(c)		((c) == '&')
#define IS_PIPE_DELIM(c)	((c) == '|')
#define IS_PARENT_DELIM(c)	((c)=='|' || (c)==';' || (c)=='&')
#define IS_GRAND_DELIM(c)	((c)=='\n')
#define IS_QUOTE(c)			((c)=='\'' || (c)=='\"')
#define IS_SUBSHELL_OPEN(c)	((c) == '(')
#define IS_SUBSHELL_CLOSE(c)	((c) == ')')
#define IS_REDIR_DELIM(c)	((c) == '>')
#define IS_REDIRTOFD_DELIM(inp)		(*(inp)=='&' && *((inp)-1)=='>')
#define IS_REDIRAPPEND_DELIM(inp)	(*(inp)=='>' && *((inp)-1)=='>')

#define TERM_DELIM			{'|', ';', '&', '\n'}
#define CHILD_DELIM			' '
#define SUBSHELL_OPEN		'('
#define SUBSHELL_CLOSE		')'
#define ESCAPE 				'\\'
#define GRAND_DELIM 		'\n'
#define PIPE 				'|'

/* parser return flags */
#define PARSERET_SUBSHELL 1

/* preproc_cmdline flags */
#define PREPRO_CREND	1
#define PREPRO_TILDEXP	2


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

/* pushes inp to end of token and nullifies that byte */
static void tokenize(char **inp, char *delim)
{

	for (; ; (*inp)++) {
		if (IS_QUOTE(**inp) && !IS_ESCAPE(*(*inp-1))) {
			*inp = astrchr(*inp+1, **inp, 1);
		}

		if (IS_CHILD_DELIM(**inp) || IS_PARENT_DELIM(**inp) || IS_GRAND_DELIM(**inp)) {
			*delim = **inp;
			**inp = '\0'; /* crafted the token */
			break;
		}
	}
}

static char *strdelimvec(char *str, int c[], int n)
{
	int i;

	for(; str; str++) {
		if (IS_QUOTE(*str) && !IS_ESCAPE(*(str-1))) {
			str = astrchr(str+1, *str, 1);
		}
		else {
			for (i=0; i<n; i++)
				if (*str == c[i] && !IS_ESCAPE(*(str-1)))
					return str;
		}
	}
	return NULL;
}

static char *strdelim(char *str, int c)
{
	for(; str; str++) {
		if (IS_QUOTE(*str) && !IS_ESCAPE(*(str-1)))
			str = astrchr(str+1, *str, 1);
		else if (*str == c && !IS_ESCAPE(*(str-1)))
			return str;
	}
	return NULL;
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
			astrcpy(al_arg, start_arg, len, 1);
			**inp = *delim; /* so that repl_str works till actual '\0' of cmdline */
			repl_str(al_arg, al_cur->trans, start_arg);

			if (al_deadend == NULL)
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

/*
 * the god-forsaken parser!!!
 * inp references the main input string and this function populates
 * cmd from start_ind. Quoted args of inp will have their leading quote
 * in cmd as an FYI for post_proc, rflags or return flags are not used
 * yet, ss_inp is the future 'inp' of subshell, and if subshell then cmd[0]
 * will have a leading '(' to signal that its a subshell and IT DOESNT
 * POPULATE THE CMD in that case, but makes ss_inp point correctly
 */
static char parse_cmd(char **inp, char **cmd, int start_ind,
		int *rflags, char **ss_inp)
{
	int i = start_ind;
	char delim;
	char *start_arg;

	if (!inp || !(*inp) || !(**inp))
		return 0;

	while (**inp == ' ')
		(*inp)++;

	*rflags = 0;
	for (start_arg=*inp; **inp!='\0'; (*inp)++) {
		if (IS_QUOTE(**inp)) {
			delim = **inp;
			/* keeping the leading quote in cmd[i] for post_proc as
			 * a signal to ignore special characters */
			cmd[i++] = (*inp);
			*inp = astrchr((*inp)+1, delim, 1);
			**inp = '\0';
			start_arg = *inp + 1;
		}
		if (IS_ESCAPE(**inp)) {
			if (!IS_GRAND_DELIM(*(*inp+1)))
				*inp += 1;
			continue;
		}
		if (IS_SUBSHELL_OPEN(**inp)) {
			char *ss_close;
			int term_delim[] = TERM_DELIM;

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

			ss_close = strdelim(*inp, SUBSHELL_CLOSE);
			if (ss_close) {
				*(ss_close++) = ' ';
				ss_close = strdelimvec(ss_close, term_delim,
					   	sizeof(term_delim) / sizeof(int));

				(*rflags) |= PARSERET_SUBSHELL;

				*ss_inp = *(inp)+1; /* skips leading '(' */
				delim = *ss_close;
				*ss_close = '\n'; /* to make subshell happy */

				/* signals that this is a subshell by the leading '('
				 * from now on , i MUST BE ZERO!!! */
				cmd[i] = *inp;
				*inp = ss_close + 1;
				return delim;
			}
		}
		if (IS_CHILD_DELIM(**inp) || IS_PARENT_DELIM(**inp) || IS_GRAND_DELIM(**inp)) {

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
					aliasize(start_arg, &delim, inp);
				}
				cmd[i++] = start_arg;
			}

			if (IS_PARENT_DELIM(delim) || IS_GRAND_DELIM(delim)) {
				(*inp)++;
				break;
			}
			start_arg = *inp + 1;
		}
	}
	cmd[i] = NULL; /* to make argvp happy */
	return delim; /* for verifying if this to be piped or bg'd etc */

hell:
	fprintf(stderr, "SHELL syntax error \n");
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
		if (redir_fd >= 1) {
			if (IS_REDIR_DELIM(*(redir_tok++))) {

				if (IS_REDIRTOFD_DELIM(redir_tok)) {
					int_till_txt(redir_tok+1, &redir_tofd);
					if (!redir_tofd || dup2(redir_tofd, redir_fd) < 0)
						ERRMSG("Error redirecting\n");
				}
				else {
				/* redir_dst can be filename or can be
				 * NULL(which will fail at redir_me) */
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
	pid_t p = 0;

	if (subshell_flag) {
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

		execvp(cmd[0], cmd);
		ERR_EXIT("execvp");
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

static pid_t fe_or_ss(char **cmd, char *ss_inp,
		int flags, pid_t pgid, sigset_t msk)
{
	if (!NOTNULL(cmd))
		return -1;
	if (*cmd[0] == SUBSHELL_OPEN) {
		return run_subshell(ss_inp, flags, pgid);
	}
	else {
		return fork_and_exec(cmd, flags, pgid, msk);
	}
}

/* regular fork and exec routine with a shell twist */
static void exec_me(char **cmd, char *ss_inp, int state)
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

	pid = fe_or_ss(cmd, ss_inp,
			(state == FG) ? FE_ALL : (FE_ALL^FE_SETTERM), 0, msk);
	job = addjob(pid, state, cmd);

	if(state == FG) {
		wait_fg(job);
	}
	else {
		printf("[%d] %d\t%s\n",(*job)->jid, (*job)->pid[0], (*job)->cmdline);
	}

	MASK_SIG(SIG_UNBLOCK, SIGCHLD, msk);
}

/* piper */
static void pipe_me(char **cmd, char **inp, char *ss_inp, char *delim)
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
			pgid = fe_or_ss(cmd, ss_inp, FE_ALL, 0, msk);
			job = addjob(pgid, FG, cmd); /* so (*job)->pid[0] = pgid */
		}
		else {
			(*job)->pid[++i] = fe_or_ss(cmd, ss_inp, FE_ALL^FE_SETTERM, pgid, msk);
			strcat((*job)->cmdline, " | ");
			stringify(pipe_cmds, cmd);
			strcat((*job)->cmdline, pipe_cmds);
		}
		/* sets up for 'read from pipe' for next child in loop */
		if (dup2(pipe_fd[0], STDIN_FILENO) < 0)
			ERR_EXIT("dup2");
		clean_up("f", pipe_fd[0]);

		*delim = parse_cmd(inp, cmd, 0, &parse_rflags, &ss_inp);
	}
	/* restores the orig stdout for the final exec */
	if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
		ERR_EXIT("dup2");
	}
	(*job)->pid[++i] = fe_or_ss(cmd, ss_inp, FE_ALL^FE_SETTERM, pgid, msk);
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
	char *ss_inp; /* the 'inp' for subshell if any */
	char delim; /* delim between cmds */
	int parse_rflags = 0; /* maybe useful in the future */

	do {
		ss_inp = NULL;
		if ((delim = parse_cmd(&inp, cmd, 0, &parse_rflags, &ss_inp)) == PIPE) {
			pipe_me(cmd, &inp, ss_inp, &delim);
		}
		else if (delim == -1) {
			goto fin;
		}
		else {
			exec_me(cmd, ss_inp,IS_BG_DELIM(delim) ? BG : FG);
		}

	} while (!IS_GRAND_DELIM(delim) && !IS_GRAND_DELIM(*inp));
	/* main cmdline end in '\n' , i guess.. */
fin :
	al_deadend = NULL; /* re-initiating it for a new cmdline */
	if (al_blist)
		al_lin_free(&al_blist);
}

/* for checking if PS2 is required, returns true on unclosed quotes,
 * subshells, escaped \n */
static int is_closed(char *str)
{
	for (; !IS_GRAND_DELIM(*str); str++) {
		if (IS_QUOTE(*str) && !IS_ESCAPE(*(str-1))) {
			if ((str=astrchr(str+1, *str, 1)) == NULL)
				return 0;
		}
		else if (IS_SUBSHELL_OPEN(*str) && !IS_ESCAPE(*(str-1))) {
			if ((str=astrchr(str+1, SUBSHELL_CLOSE, 1)) == NULL)
				return 0;
		}
	}
	return IS_ESCAPE(*(str-1)) ? 0 : 1;
}

/* ~ becomes $HOME not quoted ~ or \~ */
static void tilde_exp(char *inp)
{
	char home[PATH_MAX] = {0};
	int home_l;

	astrcpy(home, getenv("HOME"), strlen(getenv("HOME")), 1);
	home_l = strlen(home);

	for (; NOTNULL(inp); inp++) {
		if (IS_QUOTE(*inp) && !IS_ESCAPE(*(inp-1))) {
			inp = astrchr(inp+1, *inp, 1);
			continue;
		}
		if (*inp == '~' && !IS_ESCAPE(*(inp-1))) {
			repl_str("~", home, inp);
			inp += home_l - 1;
		}
	}
}

/* eats trailing spaces and calls tilde expansion */
static void preproc_cmdline(char *inp, int flags)
{
	char *i;

	if (flags & PREPRO_CREND) {
		for (i=inp + (strlen(inp)-2); *i==' '; i--)
			;
		*(i+1) = GRAND_DELIM;
	}
	if (flags & PREPRO_TILDEXP)
		tilde_exp(inp);
}

static void prompt()
{
	char cmdline[LINE_MAX] = {0}; /* main command line */
	char *inp; /* points to where input is done on main command line */

	inp = cmdline;

	while (1) {
		printf("%s", ps1);
		fgets(inp, LINE_MAX, stdin);
		while (!is_closed(cmdline)) {
			if ((inp = strchr(inp, GRAND_DELIM)) && IS_ESCAPE(*(inp-1)))
				inp--;
			printf("%s", ps2);
			fgets(inp, LINE_MAX, stdin);
		}
		inp = cmdline;
		preproc_cmdline(inp, PREPRO_TILDEXP|PREPRO_CREND);
		eval(inp);
		memset(inp, 0, LINE_MAX);
	}

}

void main()
{
	shell_init();
	prompt();
	shell_cleanup();
	exit(EXIT_FAILURE);
}
