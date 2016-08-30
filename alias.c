#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "alias.h"
#include "util.h"

static alias *al_ent[HASH_MAX];

alias *is_alias(char *al_name)
{
	return *(al_src(al_name));
}

alias *al_lin_src(char *al_name, alias *al_head)
{
	alias *walk;

	for (walk=al_head; walk!=NULL && strcmp(walk->name, al_name);)
		walk=walk->next;
	return walk;
}

void al_lin_free(alias **al_head)
{
	alias **walk, **next;

	for (walk=al_head; *walk!=NULL; walk=next) {
		next = &((*walk)->next);
		free((*walk)->name);
		free(*walk);
		*walk = NULL;
	}
}

void al_lin_ins(char *al_name, alias **al_head)
{
	alias **walk;

	for (walk=al_head; *walk!=NULL; )
		walk=&((*walk)->next);

	*walk = (alias *)malloc(sizeof(alias));
	(*walk)->name = (char *)malloc(strlen(al_name));
	strcpy((*walk)->name, al_name);
}

alias **al_src(char *al_name)
{
	alias **walk;

	for (walk=&al_ent[hash_fun(al_name)]; *walk!=NULL && strcmp((*walk)->name, al_name);)
		walk = &((*walk)->next);
	return walk;
}

void al_ins(alias **al_at, char *al_name, char *al_trans)
{
	if (*al_at == NULL) {
		*al_at = (alias *)malloc(sizeof(alias));
		(*al_at)->name = (char *)realloc((*al_at)->name, strlen(al_name));
		if (al_trans) {
			(*al_at)->trans = (char *)realloc((*al_at)->trans, strlen(al_trans));
			strcpy((*al_at)->trans, al_trans);
		}
		strcpy((*al_at)->name, al_name);
	}
	else {
		(*al_at)->trans = (char *)realloc((*al_at)->trans, strlen(al_trans));
		strcpy((*al_at)->trans, al_trans);
	}
}

void alias_me(char **cmd)
{
	char *al_name, *al_trans;
	alias *al_cur;

	if (!strchr(cmd[1], '=')) {
		if ((al_cur=is_alias(cmd[1])))
			printf("%s is aliased with %s\n", cmd[1], al_cur->trans);
		else
			printf("%s is not aliased\n", cmd[1]);
		return;
	}

	for (al_name = cmd[1]; *cmd[1]!='\0' && *cmd[1]!='=';) {
		if (*cmd[1] == ' ')
			*cmd[1] = '\0'; /* eat em bloody spaces my baby */
		cmd[1]++;
	}
	*cmd[1] = '\0'; /* al_name points till here */
	while (*(++cmd[1]) == ' ')
		; /* skip em bloody spaces made by puny humans */

	al_trans = cmd[1]; /* trailing spaces will be taken care by the parser */

	al_ins(al_src(al_name), al_name, al_trans); /* alias is hashed in */
	printf("aliasing %s with %s\n", al_name, al_trans);
 }
