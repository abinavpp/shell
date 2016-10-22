#ifndef MARKH
#define MARKH

#include "util.h"

#define MKNAME_MAX 64

typedef struct marks {
	char name[MKNAME_MAX];
	char path[PATH_MAX];
	struct marks *next;
}marks;

extern void mark_me(char **cmd);
extern void goto_mark(char **cmd);
extern void unmark_me(char **cmd);
extern void mark_free();

#endif
