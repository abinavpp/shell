#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>

#include "alias.h"
#include "util.h"
#include "jobs.h"

#define ESCAPE(c)		((c) == '\\')
#define CHILD_DELIM(c)	((c) == ' ')
#define PARENT_DELIM(c)	((c)=='|' || (c)==';' || (c)=='&')
#define GRAND_DELIM(c)	((c)=='\n')
#define QUOTE(c)		((c)=='\'' || (c)=='\"')

#define DO_REDIR	01	/* redirection */
#define DO_PP		02	/* post_process cmdline */
#define DO_UBLK		04	/* signal unblock */
#define DO_SETPG	07	/* set process group */
#define DO_ALL		(DO_REDIR|DO_PP|DO_UBLK|DO_SETPG)

void prompt();
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

/* the start of joblist */
static jobs *j_head;

/* head of the singly linked list of alias blacklist to avoid nasty
 * recurisive alias cases, like alias "ls=ls;ls" */
static alias *al_blist;

/* this marks the location at cmdline till alias blacklisting is done */
static char *al_deadend;

/* nah no ps3 or ps4 in my shell */
static char ps1[PRMT_MAX] = "ASH>";
static char ps2[PRMT_MAX] = " >";

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
		printf("\nFreed!!!\n");
		/* no blacklisting since we crossed the deadend, so freeing the list */
		al_lin_free(&al_blist);
	}

	while ((al_cur = is_alias(start_arg ))) {
		if (al_lin_src(start_arg, al_blist) == NULL) {
			/* this is not a blacklisted one, hence alias it */
			al_lin_ins(start_arg, &al_blist); /* but from now till deadend it is */
			len = strlen(start_arg);
			strcpy(al_arg, start_arg);
			**inp = *delim; /* so that repl_str works till actual '\0' of cmdline */
			repl_str(al_arg, al_cur->trans, start_arg); /* alias trans is done here */

			if (al_deadend == NULL) /* if deadend uninitialized */
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
			start_arg = (*inp) + 1; /* points to the next arg */
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
	/* printf("\n%s|%s|%s|%s\n", cmd[0], cmd[1], cmd[2], cmd[3]); */
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
				/* redir_dst can be filename or can be NULL(which will fail at redir_me) */
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
				return 1;
			}
			break;
		case 'b' :
			if (!strcmp(cmd[0], "bg")) {
				do_bgfg(&j_head, cmd, BG);
				return 1;
			}
			break;
		case 'c' :
			if (!strcmp(cmd[0], "cd")) {
				if (cmd[1]==NULL) {
					chdir(getenv("HOME"));
					return 1;
				}

				if (chdir(cmd[1])<0 )
					ERR("chdir");

				return 1;
			}
		case 'f' :
			if (!strcmp(cmd[0], "fg")) {
				do_bgfg(&j_head, cmd, FG);
				return 1;
			}
			break;
		case 'j' :
			if (!strcmp(cmd[0], "jobs")) {
				printjobs(j_head);
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

/* unqoutes quoted arg, removes backslash of escapes */
static void postproc_cmdline(char **cmd)
{
	char *tok;

	for (; *cmd != NULL; cmd=cmd+1) {
		if (QUOTE(**cmd))
			*cmd += 1;
		else {
			tok = *cmd;
			while ((tok=strchr(tok, '\\')))
				repl_str("\\","", tok++);
		}
	}
}

static void sigtstp_handler(int sig)
{
	jobs **job;

	job = get_fgjob(&j_head);
	if (job && *job) {
		if (kill(-(*job)->pid, SIGTSTP) < 0)
			ERR_EXIT("kill");
		printf("[%d] (%d) stopped by SIGTSTP\n", (*job)->jid, (*job)->pid);
		(*job)->state = ST;
	}
}

static void sigint_handler(int sig)
{
	jobs **job;

	job = get_fgjob(&j_head);
	if (job && *job) {
		if (kill(-(*job)->pid, SIGINT) < 0)
			ERR_EXIT("kill");
		printf("[%d] (%d) killed by SIGINT\n", (*job)->jid, (*job)->pid);
		deljob(&j_head, (*job)->pid);
	}
}

static void sigchld_handler(int sig)
{
	pid_t child_pid;
	jobs **job;
	int status;

	while ((child_pid = waitpid(-1, &status, WNOHANG|WUNTRACED|WCONTINUED)) > 0) {
		if (WIFEXITED(status)) {
			deljob(&j_head, child_pid);
		}
		else if (WIFSIGNALED(status)) {
			deljob(&j_head, child_pid);
			/* printf("%d Terminated by signal %d\n", child_pid, WTERMSIG(status)); */
		}
		else if (WIFSTOPPED(status)) {
			job = getjob(&j_head, child_pid, 0);
			if (job && *job)
				(*job)->state = ST;
			/* printf("%d Stopped by signal %d\n", child_pid, WSTOPSIG(status)); */
		}
		else if (WIFCONTINUED(status)) {
			job = getjob(&j_head, child_pid, 0);
			if (job && *job)
				(*job)->state = BG;
		}
	}
}

/* minimal signal init stub */
static void signal_me(int signum, int sa_flags, void (*handler)(int), 
		void(*s_action)(int sig, siginfo_t *sinf, void *context))
{
	struct sigaction action;

	action.sa_flags = sa_flags;
	sigemptyset(&action.sa_mask);

	if (sa_flags & SA_SIGINFO) {
		action.sa_handler = NULL;
		action.sa_sigaction = s_action;
	}
	else {
		action.sa_sigaction = NULL;
		action.sa_handler = handler;
	}

	if (sigaction(signum, &action, NULL) < 0)
		ERR("sigaction");
}

/* the function name explains for itself
 * facilitates pgrp and signal unblock */
static pid_t fork_and_exec(char **cmd, int flags,
		pid_t pgid, sigset_t msk)
{
	pid_t pid;

	pid = fork();
	switch (pid) {
	case -1 : 
		ERR_EXIT("fork");
		break;
	case 0  :
		if (flags & DO_REDIR)
			do_redir(cmd);

		if (flags & DO_PP)
			postproc_cmdline(cmd);

		if (flags & DO_UBLK)
			UNBLOCK_SIG(msk);

		if (flags & DO_SETPG) {
			setpgid(0, pgid);
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
	int redir_fd, ap_flag;
	char red_dst[PATH_MAX];
	char *redir_dst = red_dst;
	int status;
	sigset_t  msk;

	if (!cmd || !(*cmd) || !(**cmd))
		return;

	if (builtin(cmd))
		return;

	BLOCK_SIG(SIGCHLD, msk);
	pid = fork_and_exec(cmd, DO_ALL, 0, msk);
	job = addjob(&j_head, pid, state, cmd);

	if(state == FG) {
		UNBLOCK_SIG(msk);
		while (job && *job && (*job)->pid==pid && (*job)->state==FG) {
			sleep(1);
		}
	}

	else {
		printf("[%d] %d\t%s\n",(*job)->jid, (*job)->pid, (*job)->cmdline);
		UNBLOCK_SIG(msk);
	}
}

/* piper */
static void pipe_me(char **cmd, char **inp, char *delim)
{
	int pipe_fd[2], stdin_fd, stdout_fd, status, pipe_pid[ARG_MAX];
	int i, n;
	pid_t pgid = 0;
	sigset_t msk;
	jobs **job = NULL;

	if (!cmd || !(*cmd) || !(**cmd)) {
		return;
	}

	stdin_fd = dup(STDIN_FILENO); /* saves the orig stdin */
	stdout_fd = dup(STDOUT_FILENO); /* saves the orig stdout */

	BLOCK_SIG(SIGCHLD, msk);

	for (i=-1; *delim == '|';) { /* there is more to pipe */
		if (pipe(pipe_fd) < 0)
			ERR("pipe");

		if (dup2(pipe_fd[1], STDOUT_FILENO) < 0)
			ERR_EXIT("dup2");
		clean_up("f", pipe_fd[1]);

		if (!pgid) {
			pipe_pid[++i] = fork_and_exec(cmd, DO_ALL, 0, msk);
			pgid = pipe_pid[i];
			job = addjob(&j_head, pgid, FG, cmd);
		}
		else {
			pipe_pid[++i] = fork_and_exec(cmd, DO_ALL, pgid, msk);
		}

		while (poll(& (struct pollfd) {.fd=pipe_fd[0], .events=POLLIN},
					1, 0) != 1)
			;

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

	while (poll(& (struct pollfd) {.fd=pipe_fd[0], .events=POLLIN},
					1, 0) != 1)
			;

	pipe_pid[++i] = fork_and_exec(cmd, DO_ALL, pgid, msk);

	if (dup2(stdin_fd, STDIN_FILENO) < 0) { /* restores the orig stdin*/
		ERR_EXIT("dup2");
	}

	for (n=i, i=0; i<=n; i++) {
		while (waitpid(pipe_pid[i], &status, WNOHANG|WUNTRACED) == 0)
			;
		if (WIFSTOPPED(status)) {
			printf("[%d] (%d) stopped\n", (*job)->jid, (*job)->pid);
			(*job)->state = ST;
			goto end;
		}
	}

	deljob(&j_head, pgid);

end:

	UNBLOCK_SIG(msk);

	clean_up("ff", stdin_fd, stdout_fd);
}

/* where it all starts */
static void eval(char *inp)
{
	char *cmd[ARG_MAX]; /* main cmd array for exec */
	char delim; /* delim between cmds */

	do {
		/* printf("!"); */
		if ((delim = parse_cmd(&inp, cmd, 0)) == '|')
			pipe_me(cmd, &inp, &delim);

		else 
			exec_me(cmd, delim=='&' ? BG : FG);

	} while (!GRAND_DELIM(delim) && !GRAND_DELIM(*inp));
	/* main cmdline end in '\n' or '\0', i guess.. */
	al_deadend = NULL; /* re-initiating it for a new cmdline */
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
	char home[PATH_MAX];
	int home_l;

	strcpy(home, getenv("HOME"));
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
	*(i+1) = '\n';
	tilde_exp(inp);
}

void prompt()
{
	char cmd[LINE_MAX]; /* main command line */
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
	signal_me(SIGCHLD, SA_RESTART, sigchld_handler, NULL);
	signal_me(SIGTSTP, SA_RESTART, sigtstp_handler, NULL);
	signal_me(SIGINT, SA_RESTART, sigint_handler, NULL);
	prompt();
}
