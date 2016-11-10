#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "mark.h"

/* points to the head of the main mark list */
static marks *mk_head;

/* returns a ref to mark with name name */
static marks **src_mark(char *name)
{
	marks **walk;

	for (walk=&mk_head; *walk && strcmp((*walk)->name, name);
			walk=&((*walk)->next))
		;
	return walk;
}

/* adds mark if new, overwrites if exists */
static void add_mark(char *name, char *path)
{
	marks **mark;

	mark = src_mark(name);
	if (!(*mark)) {
		(*mark) = (marks *)malloc(sizeof(marks));
		(*mark)->next = NULL;
	}
	astrcpy((*mark)->name, name, strlen(name), 1);
	astrcpy((*mark)->path, path, strlen(path), 1);
}

/* del mark with name */
static void del_mark(char *name)
{
	marks *target, **mark;

	if (*(mark=src_mark(name))) {
		printf("mark %s removed\n", (*mark)->name);
		target = *mark;
		*mark = (*mark)->next;
		free(target);
	}
	else
		fprintf(stderr, "%s not marked\n", name);
}

/* frees the whole mark list, called at shell cleanup */
void mark_free()
{
	marks *walk, *target;

	for (walk=mk_head; walk; walk=walk->next ,free(target)) {
		target = walk;
	}
	mk_head = NULL;
}

void print_all_marks()
{
	marks *walk;

	for (walk=mk_head; walk; walk=walk->next)
		printf("%s -> %s\n", walk->name, walk->path);
}

/* Following fucntions are called by the shell builtins */

/* called by mk */
void mark_me(char **cmd)
{
	char res_path[PATH_MAX];
	marks **mark;

	if (!cmd[1]) {
		print_all_marks();
	}
	else if (!cmd[2]) {
		if (*(mark=src_mark(cmd[1])))
			printf("%s -> %s\n", (*mark)->name, (*mark)->path);
		else
			fprintf(stderr, "%s not marked\n", cmd[1]);
	}
	else {
		add_mark(cmd[2], realpath(cmd[1], res_path) ? res_path : cmd[1]);
	}
}

/* called by unmk */
void unmark_me(char **cmd)
{
	int i;

	for (i=1; cmd[i]; i++)
		del_mark(cmd[i]);
}

/* called by gt */
void goto_mark(char **cmd)
{
	marks **mark;

	if (!cmd[1]) {
		fprintf(stderr, "gt : Usage gt <mark name>\n");
	}
	else if (*(mark=src_mark(cmd[1]))) {
		if (chdir((*mark)->path) < 0)
			ERR("chdir");
	}
	else {
		fprintf(stderr, "mark not defined\n");
	}
}
