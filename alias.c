#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "alias.h"
#include "util.h"

static alias *al_ent[HASH_MAX];

alias *al_lin_src(alias *al_head, char *al_name)
{
	alias *walk;

	for (walk=al_head; walk && strcmp(walk->name, al_name);)
		walk=walk->next;
	return walk;
}

void al_lin_free(alias **al_head)
{
	alias **walk, **next;

	for (walk=al_head; NOTNULL(walk); walk=next) {
		next = &((*walk)->next);
		free((*walk)->name);
		free(*walk);
		*walk = NULL;
	}
}

void al_lin_ins(alias **al_head, char *al_name)
{
	alias **walk;

	for (walk=al_head; NOTNULL(walk); )
		walk=&((*walk)->next);

	if ((*walk = (alias *)malloc(sizeof(alias))) == NULL)
		ERR_EXIT("malloc");
	if (( (*walk)->name = (char *)malloc(strlen(al_name)+1) )
			== NULL)
		ERR_EXIT("malloc");
	astrcpy((*walk)->name, al_name, strlen(al_name), 1);
	(*walk)->next = NULL;
}

alias *is_alias(char *al_name)
{
	return *(al_src(al_name));
}

alias **al_src(char *al_name)
{
	alias **walk;

	for (walk=&al_ent[hash_fun(al_name)]; 
			*walk && strcmp((*walk)->name, al_name);)
		walk = &((*walk)->next);
	return walk;
}

void al_ins(char *al_name, char *al_trans)
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

void alias_me(char **cmd)
{
	char *al_name, *al_trans;
	alias *al_cur;

	if (!NOTNULL(cmd[1]))
		return;

	if (!strchr(cmd[1], '=')) {
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

	al_ins(al_name, al_trans); /* alias is hashed in */
	printf("aliasing %s with %s\n", al_name, al_trans);
 }
