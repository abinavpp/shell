#ifndef ALIASH
#define ALIASH

typedef struct alias {
	char *name;
	char *trans;
	struct alias *next;
} alias;

#define ALIAS_DIFF(al)	(strlen((al)->trans) - strlen((al)->name))

extern void al_lin_ins(char *al_name, alias **al_at);
extern void al_lin_free(alias **al_head);
extern alias *al_lin_src(char *al_name, alias *al_head);

extern alias *is_alias(char *al_name);
extern alias **al_src(char *al_name);
extern void al_ins(alias **al_at, char *al_name, char *al_trans);
extern void alias_me(char **cmd);

#endif
