#ifndef UTILH
#define UTILH

#define HASH_MAX 		100
#define PRMT_MAX		128
#define ARG_MAX			256
#define PATH_MAX		4096
#define LINE_MAX		4096 

#define ERR(e)do {						\
	fprintf(stderr, "%s @ ", __FILE__); \
	fprintf(stderr, "%u\n", __LINE__);	\
	perror(e);							\
	} while (0);				\

#define ERRMSG(e)do {						\
	fprintf(stderr, "%s @ ", __FILE__); \
	fprintf(stderr, "%u\n", __LINE__);	\
	fprintf(stderr, (e));				\
	} while (0);				\

extern char *int_till_txt(char *inp, int *res);
extern int hash_fun(char *inp);
extern void shift_str(char *str, int n);
extern void repl_str(char *pat, char *rep, char *start_pat);
extern void my_strcpy(char *dst, const char *src, char null_flag);

#endif
