#ifndef UTILH
#define UTILH

#include <unistd.h>

#define HASH_MAX 		101
#define PRMT_MAX		128
#define ARG_MAX			256
#define PATH_MAX		4096
#define LINE_MAX		4096

#define TERMSTR_RED(s)		"\e[0;31m"s"\e[00m"
#define TERMSTR_GREEN(s)	"\e[0;32m"s"\e[00m"
#define TERMSTR_YELLOW(s)	"\e[0;33m"s"\e[00m"
#define TERMSTR_BLUE(s)		"\e[0;34m"s"\e[00m"

#define NOTNULL(dptr) ((dptr) && (*(dptr)))
#define STRINGIZE(c) #c

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

extern char *chrtochr(char *str, int cf, int ct);
extern char *int_till_txt(char *inp, int *res);
extern int hash_fun(char *inp);
extern void repl_str(const char *pat, const char *rep, char *start_pat);
extern void astrcpy(char *dst, const char *src, int len, char null_flag);
extern char *astrchr(char *str, int c, int esc_flag);
extern void clean_up(const char *str, ...);
extern void stringify(char *dst, char **src);
extern void prints(char *str);

#endif
