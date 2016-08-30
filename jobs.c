#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include "util.h"
#include "jobs.h"

static int joblsz;

int getjid(jobs *j_head)
{
	int i;

	if (!j_head) 
		return 1;

	for (i=0; i<(joblsz-1); i++, j_head=j_head->next)
		;
	return (j_head->jid + 1);
}

void printjobs(jobs *j_head)
{
	for (; j_head!=NULL; j_head=j_head->next) {
		printf("[%d] %d %s\t %s\n", j_head->jid, j_head->pid,
				j_head->state==ST ? "stopped":"running", 
				j_head->cmdline);
	}
}


jobs **getjob(jobs **j_head, pid_t pid, int jid)
{
	jobs **walk;

	if (pid) {
		for (walk=j_head; *walk!=NULL && (*walk)->pid!= pid;
				walk=&((*walk)->next))
			;
		return walk;
	}
	if (jid) {
		for (walk=j_head; *walk!=NULL && (*walk)->jid!= jid;
				walk=&((*walk)->next))
			;
		return walk;
	}
	return NULL;
}

jobs **addjob(jobs **j_head, pid_t pid, int state, char **cmd)
{
	int jid;
	jobs **job;
	char cmdline[LINE_MAX];

	jid = getjid(*j_head);
	job = getjob(j_head, -1, 0);
	stringify(cmdline, cmd);

	*job = (jobs *)malloc(sizeof(jobs));
	(*job)->pid = pid;
	(*job)->jid = jid;
	(*job)->state = state;
	/* this is bloody important, NEVER rely on the fact that next will
	 * point to NULL by default, speaking from experience! >:( */
	(*job)->next = NULL; 
	strcpy((*job)->cmdline, cmdline);

	joblsz++;
	return job;
}

jobs **get_fgjob(jobs **j_head)
{
	jobs **walk;

	for (walk=j_head; *walk!=NULL && (*walk)->state!= FG;
			walk=&((*walk)->next))
		;
	return walk;
}

jobs **get_lastjob(jobs **j_head)
{
	jobs **walk;

	for (walk=j_head; *walk!=NULL && (*walk)->next!= NULL;
			walk=&((*walk)->next))
		;
	return walk;
}

void deljob(jobs **j_head, pid_t pid)
{
	jobs **job, *target;

	job = getjob(j_head, pid, 0);

	/* getjob can return (job **)NULL or &(job *)NULL */
	if (job==NULL || *job==NULL)
		return;

	target = *job;
	/* order is IMPORTANT, we dont want to free neighbour's next 
	 * so update neighbour first then kill the target */
	*job = (*job)->next;
	free(target);
	joblsz--;
}

void do_bgfg(jobs **j_head, char **cmd, int state)
{
	jobs **job;
	pid_t pid;
	sigset_t msk;

	if (!cmd[1]) {
		job = get_lastjob(j_head);
	}
	else if (*cmd[1]=='%' && atoi(cmd[1]+1)) {
		job = getjob(j_head, 0, atoi(cmd[1]+1));
	}
	else {
		ERRMSG("Invalid argument\n");
		return;
	}

	if (job && *job) {

		if (kill(-(*job)->pid, SIGCONT) < 0)
			ERR_EXIT("kill");

		while (job && *job && (*job)->state != BG)
			sleep(1);

		if (job && *job)
			(*job)->state = state;
		else 
			return;

		if (state == BG) {
			if (job && *job)
				printf("[%d] (%d) %s\n", (*job)->jid, (*job)->pid, (*job)->cmdline);
		}
		else {
			if (job && *job) {
				printf("%s\n", (*job)->cmdline);
				pid = (*job)->pid;
			}
			while (job && *job && (*job)->pid==pid && (*job)->state==FG)
				sleep(1);
		}
	}

	else {
		ERRMSG("Invalid job\n");
	}
}

