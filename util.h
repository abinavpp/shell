#ifndef UTILH
#define UTILH

#include <unistd.h>

#define HASH_MAX 		100
#define PRMT_MAX		128
#define ARG_MAX			256
#define PATH_MAX		4096
#define LINE_MAX		4096

#define NOTNULL(dptr) ((dptr) && (*(dptr)))

#define ERR(e) do {						\
	fprintf(stderr, "%s @ ", __FILE__); \
	fprintf(stderr, "%u\n", __LINE__);	\
	perror((e));						\
	} while (0);						

#define ERRMSG(e) do {					\
	fprintf(stderr, "%s @ ", __FILE__); \
	fprintf(stderr, "%u\n", __LINE__);	\
	fprintf(stderr, (e));				\
	} while (0);				

#define ERR_EXIT(e) do {	\
	ERR((e));				\
	_exit(EXIT_FAILURE);	\
	} while (0);

extern char *int_till_txt(char *inp, int *res);
extern int hash_fun(char *inp);
extern void shift_str(char *str, int n);
extern void repl_str(char *pat, char *rep, char *start_pat);
extern void astrcpy(char *dst, const char *src, int len, char null_flag);
extern void clean_up(const char *str, ...);
extern void stringify(char *dst, char **src);

#endif
