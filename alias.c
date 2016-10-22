#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "alias.h"
#include "util.h"

/* main alias hash table entries */
static alias *al_ent[HASH_MAX];

/* 
 * the following al_lin... functions are operations on
 * a minimal linear data structure used by the parser only
 * for blacklisting. al_trans is not used since we are blacklisting
 * alias names and not its trans al_head is presumebly defined outside
 * alias.c and must be tracked there unlike the main alias hash table
 * that is all maintained in here.
 */

/* searches al_lin from al_head in search of al_name entry */
alias *al_lin_src(alias *al_head, char *al_name)
{
	alias *walk;

	for (walk=al_head; walk && strcmp(walk->name, al_name);)
		walk=walk->next;
	return walk;
}

/* deallocates the whole al_lin data structure and makes
 * (*al_head) point to NULL */
void al_lin_free(alias **al_head)
{
	alias *walk, *target;

	if (!NOTNULL(al_head))
		return;

	for (walk=*al_head;;) {
		target = walk;
		if (walk->next)
			walk = walk->next;
		else {
			free(target->name);
			free(target);
			break;
		}
		free(target->name);
		free(target);
	}
	*al_head = NULL;
}

/* inserts into al_lin data structure pointed by *al_head */
void al_lin_ins(alias **al_head, char *al_name)
{
	alias *old_head;

	old_head = *al_head;

	if ((*al_head = (alias *)malloc(sizeof(alias))) == NULL)
		ERR_EXIT("malloc");
	if (( (*al_head)->name = (char *)malloc(strlen(al_name)+1) )
			== NULL)
		ERR_EXIT("malloc");
	astrcpy((*al_head)->name, al_name, strlen(al_name), 1);
	(*al_head)->next = old_head;
}

/*
 * searches for al_name in the alias hash table and
 * returns a reference to the pointer itself so that insertion
 * and deletion can be done better
 */
static alias **al_src(char *al_name)
{
	alias **walk;

	for (walk=&al_ent[hash_fun(al_name)]; 
			*walk && strcmp((*walk)->name, al_name);)
		walk = &((*walk)->next);
	return walk;
}


/* best for use outside alias.c */
alias *is_alias(char *al_name)
{
	return *(al_src(al_name));
}

/* 
 * inserts alias with al_name and al_trans into the alias
 * hash table, if overwriting, al_trans is realloced else
 * the entries are freshly malloced, this call should succeed
 * all the time
 */
static void al_ins(char *al_name, char *al_trans)
{
	alias **al_at;

	if (!al_name || !al_trans)
		return;

	al_at = al_src(al_name);

	if (*al_at == NULL) {
		if ((*al_at = (alias *)malloc(sizeof(alias))) == NULL)
			ERR_EXIT("malloc");

		if (( (*al_at)->name = (char *)malloc(strlen(al_name)+1) )
				== NULL)
			ERR("malloc");
		astrcpy((*al_at)->name, al_name, strlen(al_name), 1);

		if (( (*al_at)->trans = (char *)malloc(strlen(al_trans)+1) )
				== NULL)
			ERR_EXIT("malloc");

		astrcpy((*al_at)->trans, al_trans, strlen(al_trans), 1);

		(*al_at)->next = NULL;
	}
	else {
		if ( ((*al_at)->trans = (char *)realloc
					((*al_at)->trans, strlen(al_trans)+1)) == NULL)
			ERR("realloc");
		astrcpy((*al_at)->trans, al_trans, strlen(al_trans), 1);
	}
}

/* removes alias from alias hash table, returns 1 if found & 
 * removed OR 0 if not found */
static int al_del(char *al_name)
{
	alias **al_tgt;
	alias *free_tgt;

	al_tgt = al_src(al_name);

	if (*al_tgt) {
		free((*al_tgt)->name);
		free((*al_tgt)->trans);
		free_tgt = *al_tgt;
		*al_tgt = (*al_tgt)->next;
		free(free_tgt);
		return 1;
	}
	return 0;
}

void al_free()
{
	int i;
	alias *free_tgt;

	for(i=0; i<HASH_MAX; i++) {
		while (al_ent[i]) {
			free(al_ent[i]->name);
			free(al_ent[i]->trans);
			free_tgt = al_ent[i];
			al_ent[i] = al_ent[i]->next;
			free(free_tgt);
		}
	}
}

/* Following functions are the ones called by shell builtins */

/* called by shell builtin alias */
void alias_me(char **cmd)
{
	char *al_name, *al_trans;
	alias *al_cur;

	if (!NOTNULL(cmd[1]))
		return;

	if (!astrchr(cmd[1], '=', 1)) {
		if ((al_cur=is_alias(cmd[1])))
			printf("%s is aliased with %s\n", cmd[1], al_cur->trans);
		else
			printf("%s is not aliased\n", cmd[1]);
		return;
	}

	for (al_name = cmd[1]; *cmd[1]!='\0' && *cmd[1]!='=';) {
		if (*cmd[1] == ' ')
			*cmd[1] = '\0'; /* eat em bloody spaces */
		cmd[1]++;
	}
	*cmd[1] = '\0'; /* al_name points till here */
	while (*(++cmd[1]) == ' ')
		; /* skip em bloody spaces made by puny humans */

	/* trailing spaces will be taken care by the parser */
	al_trans = cmd[1]; 

	if (al_name && *al_name) {
		al_ins(al_name, al_trans); /* alias is hashed in */
		printf("aliasing %s with %s\n", al_name, al_trans);
	}
}

/* called by shell builtin unalias */
void unalias_me(char **cmd)
{
	if (!NOTNULL(cmd[1]))
		return;

	if (al_del(cmd[1]))
		printf("%s unaliased\n", cmd[1]);
	else
		printf("%s not aliased\n", cmd[1]);
}
