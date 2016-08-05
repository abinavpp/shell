#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "alias.h"
#include "util.h"

alias *al_ent[HASH_MAX];

alias **al_src(char *al_name);
int al_ins(alias **al_at, char *al_name, char *al_trans);
void alias_me(char **cmd);

alias **al_src(char *al_name)
{
	alias **walk;

	for (walk=&al_ent[hash_fun(al_name)]; *walk!=NULL && strcmp((*walk)->name, al_name);)
		walk = &((*walk)->next);
	return walk;
}

int al_ins(alias **al_at, char *al_name, char *al_trans)
{
	if (*al_at == NULL) {
		*al_at = (alias *)malloc(sizeof(alias));
		(*al_at)->name = (char *)realloc((*al_at)->name, strlen(al_name));
		(*al_at)->trans = (char *)realloc((*al_at)->trans, strlen(al_trans));

		strcpy((*al_at)->name, al_name);
		strcpy((*al_at)->trans, al_trans);
	}
	else {
		(*al_at)->trans = (char *)realloc((*al_at)->trans, strlen(al_trans));
		strcpy((*al_at)->trans, al_trans);
	}
}

void alias_me(char **cmd)
{
	char *al_name, *al_trans;

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
	printf("\naliasing %s with %s\n", al_name, al_trans);
 }
