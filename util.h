#ifndef UTILH
#define UTILH

#define HASH_MAX 		100
#define MAX_LINE		4096 

#define ERR(e)do {						\
	fprintf(stderr, "%s @ ", __FILE__); \
	fprintf(stderr, "%u\n", __LINE__);	\
	perror(e);							\
	exit(1);} while (0);				\

extern int hash_fun(char *inp);
extern void shift_str(char *str, int n);
extern void repl_str(char *pat, char *rep, char **start_pat);
extern void my_strcpy(char *dst, const char *src, char null_flag);

#endif
