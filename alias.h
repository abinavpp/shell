#define HASH_MAX 100
typedef struct alias {
	char *name;
	char *trans;
	struct alias *next;
} alias;

alias *al_ent[HASH_MAX];

extern int hash_fun(char *inp);
extern void shift_str(char *str, int n);
extern void repl_str(char *pat, char *rep, char **start_pat);
extern alias **al_src(char *al_name);
extern int al_ins(alias **al_at, char *al_name, char *al_trans);
extern void alias_me(char **cmd);
