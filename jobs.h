#ifndef JOBSH
#define JOBSH

#include "util.h"
#include <sys/types.h>
#include <signal.h>

#define PIPE_MAX		256

#define BG 1
#define FG 2
#define ST 4

#define START_JOBID 1

#define TERMFD 255

#define SHELLPRINT_CHKSS	1
#define SHELLPRINT_CHKTERM	1

typedef struct jobs {
	pid_t pid[PIPE_MAX];
	int jid;
	int state;
	char cmdline[LINE_MAX];
	struct jobs *next;
} jobs;

#define MASK_SIG(how, sig, msk)	do {				\
	sigemptyset(&(msk));							\
	sigaddset(&(msk), (sig));						\
	if (sigprocmask((how), &(msk), NULL) < 0)		\
		ERR_EXIT("sigprocmask");					\
	} while (0);

#define MASK_ALLSIG(how, msk) do {					\
	sigfillset(&(msk));								\
	if (sigprocmask((how), &(msk), NULL) < 0)		\
		ERR_EXIT("sigprocmask");					\
	} while (0);

#define SIG_TGL(sig, act) do {						\
	if (sigaction((sig), &(struct sigaction){		\
				.sa_handler=(act)}, NULL) < 0)		\
		ERR_EXIT("sigaction");						\
	}while(0);


extern void printjobs();

extern jobs **addjob(pid_t pid, int state, char **cmd);
extern void deljob(pid_t pid);
extern void job_free();

extern void wait_fg(jobs **job);
extern void do_bgfg(char **cmd, int state);

extern void signal_init();

extern void shell_printf(int flags, const char *fmt, ...);

#endif
