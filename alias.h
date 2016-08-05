#ifndef ALIASH
#define ALIASH


typedef struct alias {
	char *name;
	char *trans;
	struct alias *next;
} alias;

extern alias **al_src(char *al_name);
extern int al_ins(alias **al_at, char *al_name, char *al_trans);
extern void alias_me(char **cmd);

#endif
