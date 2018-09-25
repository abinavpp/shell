#include <stdarg.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>

#include "util.h"
#include "jobs.h"

extern int subshell_flag;

static jobs *j_head;

/*
 * the 'thy shall NOT pass' sections are the critical sections
 * take extra care messing with & using that block by using
 * sync mechansim like SIGBLOCK, semaphore etc(whichever apt)
 */

static int getjid()
{
	jobs *walk;

	if (!j_head) /* empty job list */
		return START_JOBID;

	for (walk=j_head; walk->next; walk=walk->next)
		; /* last job */
	return (walk->jid + 1);
}

/*
 * get job by pid or jid, not by both
 * pid = 0, search by jid
 * pid = -1 || jid = -1, for ins at tail
 */
static jobs **getjob(pid_t pid, int jid)
{
	jobs **walk;

	if (pid) {
		for (walk=&j_head; *walk && (*walk)->pid[0]!= pid;
				walk=&((*walk)->next))
			;
		return walk;
	}
	if (jid) {
		for (walk=&j_head; *walk && (*walk)->jid!= jid;
				walk=&((*walk)->next))
			;
		return walk;
	}
	return NULL;
}

/* return pointer to current foreground job */
static jobs **get_fgjob()
{
	jobs **walk;

	for (walk=&j_head; *walk && (*walk)->state!= FG;
			walk=&((*walk)->next))
		;
	return walk;
}

/* returns pointer to last job */
static jobs **get_lastjob()
{
	jobs **walk;

	for (walk=&j_head; *walk && (*walk)->next;
			walk=&((*walk)->next))
		;
	return walk;
}

/* what >job runs */
void printjobs()
{
	jobs *walk;

	for (walk=j_head; walk; walk=walk->next) {
		printf("[%d] %d %s\t %s\n", walk->jid, walk->pid[0],
				walk->state==ST ? "stopped":"running",
				walk->cmdline);
	}
}

/*
 * ins job in joblist
 * pid[0] is actually the pgid for job, ie pid in the fn arg
 * pid[i]=0 is end of pidlist for job
 */
jobs **addjob(pid_t pid, int state, char **cmd)
{
	int jid;
	jobs **job;
	char cmdline[LINE_MAX];
	char *eocmd;

	jid = getjid();
	job = getjob(-1, 0);
	stringify(cmdline, cmd);

	if ((*job = (jobs *)malloc(sizeof(jobs))) == NULL)
		ERR_EXIT("malloc");
	(*job)->pid[0] = pid; (*job)->pid[1] = 0; /* val 0 = end of pids */
	(*job)->jid = jid;
	(*job)->state = state;
	/* this is bloody important, NEVER rely on the fact that next will
	 * point to NULL , mallocs are not zeroed! >:( */
	(*job)->next = NULL;
	astrcpy((*job)->cmdline, cmdline, strlen(cmdline), 1);
	/* \n appears if subshell, so replacing it with ')' */
	if ((eocmd = chrtochr((*job)->cmdline, '\n', ')')))
		*(eocmd+1) = '\0';

	return job;
}

/* removes job from the joblist by pid,
 * silently returns if job absent */
void deljob(pid_t pid)
{
	jobs **job, *target;

	job = getjob(pid, 0);

	/* getjob can return (job **)NULL or &(job *)NULL */
	if (NOTNULL(job)) {
		target = *job;
		/* order is IMPORTANT, we dont want to free neighbour's next
		 * so update neighbour first then kill the target */
		*job = (*job)->next;
		free(target);
	}
}

/* frees all the job entries from j_head */
void job_free()
{
	jobs *walk, *target;

	for (walk=j_head; walk; walk=walk->next ,free(target)) {
		target = walk;
	}
	j_head = NULL;
}

/* prints only if not a subshell */
void shell_printf(int flags, const char *fmt, ...)
{
	va_list ap;

	if (flags & SHELLPRINT_CHKSS) {
		if (subshell_flag)
			return;
	}
	if (flags & SHELLPRINT_CHKTERM) {
		if (!isatty(STDIN_FILENO))
			return;
	}

	va_start(ap, fmt);
	vprintf(fmt, ap);
}

/*
 * the basic wait for foreground used by fg cmd and basic
 * non bg cmd in the shell. Make sure SIGCHLD is blocked
 * to avoid race conditions that make the joblist unreliable.
 * No terminal control or printing if subshell
 * */
void wait_fg(jobs **job)
{
	int status, i;

	/* thy shall NOT pass */

	for (i=0; NOTNULL(job); i++) {
		if (!subshell_flag && tcsetpgrp(TERMFD, (*job)->pid[0]) < 0)
			perror("tcsetpgrp");
		while (waitpid((*job)->pid[i], &status, WUNTRACED) == 0)
			;

		if (WIFEXITED(status)) {
			if (!subshell_flag && tcsetpgrp(TERMFD, getpid()) < 0)
				perror("tcsetpgrp");
			if ((*job)->pid[i+1] == 0) {
				deljob((*job)->pid[0]);
				break;
			}
		}

		else if (WIFSIGNALED(status)) {
			if (!subshell_flag && tcsetpgrp(TERMFD, getpid()) < 0)
				perror("tcsetpgrp");
			shell_printf(SHELLPRINT_CHKSS | SHELLPRINT_CHKTERM,
					"[%d] (%d) killed by signal %d\n", (*job)->jid, (*job)->pid[0],
					WTERMSIG(status));
			deljob((*job)->pid[0]);
			break;
		}

		else if (WIFSTOPPED(status)) {
			if (!subshell_flag && tcsetpgrp(TERMFD, getpid()) < 0)
				perror("tcsetpgrp");
			shell_printf(SHELLPRINT_CHKSS | SHELLPRINT_CHKTERM,
					"[%d] (%d) stopped by signal %d\n", (*job)->jid,
					(*job)->pid[0], WSTOPSIG(status));
			(*job)->state = ST;
			break;
		}
	}
	/* thy shall pass */
}

/* what >bg and >fg run */
void do_bgfg(char **cmd, int state)
{
	jobs **job;
	sigset_t msk;

	MASK_SIG(SIG_BLOCK, SIGCHLD, msk);

	if (!cmd[1]) {
		job = get_lastjob();
	}
	else if (*cmd[1]=='%' && atoi(cmd[1]+1)) {
		job = getjob(0, atoi(cmd[1]+1));
	}
	else {
		ERRMSG("Invalid argument\n");
		return;
	}

	/* thy shall NOT pass */

	if (NOTNULL(job)) {
		(*job)->state = state;

		if (kill(-(*job)->pid[0], SIGCONT) < 0)
			ERR_EXIT("kill");

		if (state == BG) {
			shell_printf(SHELLPRINT_CHKSS | SHELLPRINT_CHKTERM,
					"[%d] (%d) %s\n", (*job)->jid, (*job)->pid[0], (*job)->cmdline);
		}
		else {
			shell_printf(SHELLPRINT_CHKSS | SHELLPRINT_CHKTERM,
					"%s\n", (*job)->cmdline);
			wait_fg(job);
		}
	}
	else {
		ERRMSG("Invalid job\n");
	}

	/* thy shall pass */
	MASK_SIG(SIG_UNBLOCK, SIGCHLD, msk);
}

/*
 * All signal handlers of shell solely signals to the jobs, it
 * should not modify the joblist data structure since the handler
 * might be done in a critical section. All signal handlers have
 * SIG_BLOCK mask set to all signals.
 */

/* ctrl+z */
static void sigtstp_handler(int sig)
{
	jobs **job;

	job = get_fgjob();

	if (NOTNULL(job)) {
		if (kill(-(*job)->pid[0], SIGTSTP) < 0)
			ERR_EXIT("kill");
	}
}

/* ctrl+c */
static void sigint_handler(int sig)
{
	jobs **job;

	job = get_fgjob();

	if (NOTNULL(job)) {
		if (kill(-(*job)->pid[0], SIGINT) < 0)
			ERR_EXIT("kill");
	}
}

/* state change of children handled here */
static void sigchld_handler(int sig)
{
	pid_t child_pid;
	jobs **job;
	int status;

	/*
	 * There can be multiple SIGCHLD at once, not necessarily they will all be handled,
	 * so we reap all the children in non-blocking mode that takes care of the pending child
	 * whose SIGCHLD is not attended
	 */

	/* SIGCHLD should be blocked in critical section of modifying joblist */
	/* thy shall NOT pass */
	while ((child_pid = waitpid(-1, &status, WNOHANG|WUNTRACED|WCONTINUED)) > 0) {
		if (WIFEXITED(status)) {
			deljob(child_pid);
		}

		else if (WIFSIGNALED(status)) {
			job = getjob(child_pid, 0);
			if (NOTNULL(job)) {
				shell_printf(SHELLPRINT_CHKSS | SHELLPRINT_CHKTERM,
						TERMSTR_GREEN("SIGCHLD: ")"[%d] (%d) killed by signal %d\n",
						(*job)->jid, (*job)->pid[0], WTERMSIG(status));
				deljob(child_pid);
			}
		}

		else if (WIFSTOPPED(status)) {
			job = getjob(child_pid, 0);
			if (NOTNULL(job)) {
				(*job)->state = ST;
				shell_printf(SHELLPRINT_CHKSS | SHELLPRINT_CHKTERM,
						TERMSTR_GREEN("SIGCHLD : ")"[%d] (%d) stopped by signal %d\n",
						(*job)->jid, (*job)->pid[0], WSTOPSIG(status));
			}
		}

		else if (WIFCONTINUED(status)) {
			job = getjob(child_pid, 0);
			if (NOTNULL(job))
				(*job)->state = BG;
		}
	}
	/* thy shall pass */
}

/* minimal signal init stub */
static void signal_me(int signum, int sa_flags, void (*handler)(int),
		void(*s_action)(int sig, siginfo_t *sinf, void *context))
{
	struct sigaction action;

	/* sighandler block all signals */
	action.sa_flags = sa_flags;
	sigfillset(&action.sa_mask);

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

/* called initially by the shell */
void signal_init()
{
	signal_me(SIGCHLD, SA_RESTART, sigchld_handler, NULL);
	signal_me(SIGTSTP, SA_RESTART, sigtstp_handler, NULL);
	signal_me(SIGINT, SA_RESTART, sigint_handler, NULL);
	SIG_TGL(SIGTTOU, SIG_IGN);
	SIG_TGL(SIGTTIN, SIG_IGN);
}
