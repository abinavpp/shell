#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define HASH_MAX 100

typedef struct alias {
	char *name;
	char *trans;
	struct alias *next;
} alias;

alias *al_ent[HASH_MAX];

int hash_fun(char *inp)
{
	int sum;

	for (sum=0; *inp != '\0'; inp++)
		sum += *inp;
	return sum % HASH_MAX;
}

/* null flag decides whether to append '\0' or not */
static inline void my_strcpy(char *dst, const char *src, char null_flag)
{
	while (*src != '\0')
		*dst++ = *src++;
	if (null_flag)
		*dst = '\0';
}

void shift_str(char *str, int n)
{
	char temp_str[strlen(str)]; /* stores orig str */
	int strlen;
	int i;

	if (n) { /* else don't shift */
		my_strcpy(temp_str, str, 1);
		str += n; /* everything that's skipped here is non relevant */
		my_strcpy(str, temp_str, 1);
	}
}

void repl_str(char *pat, char *rep, char **start_pat)
{
	int len_pat;
	int len_rep;
	int len_diff;

	len_pat = strlen(pat);
	len_rep = strlen(rep);
	len_diff = len_rep-len_pat;
	/* only if we get the pattern */
	if (start_pat != NULL) {
		*start_pat += len_pat; /* shift only the stuff after the pattern */
		shift_str(*start_pat, len_diff);
		*start_pat -= len_pat; /* go back to the pattern */
		my_strcpy(*start_pat, rep, 0); /* just copy the rep without \0 at end */
	}
	
}

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
	al_trans = cmd[1]; /* trailing spaces will be taken care by the pareser */

	al_ins(al_src(al_name), al_name, al_trans); /* alias is hashed in */
	printf("\naliasing %s with %s\n", al_name, al_trans);
 }
