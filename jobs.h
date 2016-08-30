#ifndef JOBSH
#define JOBSH

#include "util.h"
#include <sys/types.h>

typedef struct jobs {
	pid_t pid;
	int jid;
	int state;
	char cmdline[LINE_MAX];
	struct jobs *next;
} jobs;

#define BG 1
#define FG 2
#define ST 4

extern int getjid(jobs *j_head);
extern void printjobs(jobs *j_head);

extern jobs **getjob(jobs **j_head, pid_t pid, int jid);
extern jobs **addjob(jobs **j_head, pid_t pid, int state, char **cmd);
extern void deljob(jobs **j_head, pid_t pid);
extern void do_bgfg(jobs **j_head, char **cmd, int state);
extern jobs **get_fgjob(jobs **j_head);
extern jobs **get_lastjob(jobs **j_head);

#endif
