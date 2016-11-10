#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>

#include "util.h"

char *chrtochr(char *str, int cf, int ct)
{
	if ((str = astrchr(str, cf, 1)))
		*str = ct;
	return str;
}

void clean_up(const char *str, ...)
{
	va_list ap;

	va_start(ap, str);
	while (*str != '\0') {
		switch (*str++) {
		case 'f' :
			if (close(va_arg(ap, int)) < 0)
				ERR_EXIT("close");
			break;
		case 'a' :
			free(va_arg(ap, void*));
			break;
		default :
			break;
		}
	}
	va_end(ap);
}

int hash_fun(char *inp)
{
	int sum;

	for (sum=0; *inp != '\0'; inp++)
		sum += *inp;
	return sum % HASH_MAX;
}

/* null flag decides whether to append '\0' or not */
void astrcpy(char *dst, const char *src, int len, char null_flag)
{
	strncpy(dst, src, len);
	if (null_flag) {
		*(dst+len) = '\0';
	}
}

char *astrchr(char *str, int c, int esc_flag)
{
	str = strchr(str, c);
	if (esc_flag) {
		while (str && *(str-1) == '\\')
			str = strchr(++str, c);
	}
	return str;
}

void shift_str(char *str, int n)
{
	char temp_str[strlen(str)]; /* stores orig str */

	if (n) { /* else don't shift */
		astrcpy(temp_str, str, strlen(str), 1);
		/* everything that's skipped here is non relevant */
		astrcpy(str+n, temp_str, strlen(temp_str), 1);
	}
}

void repl_str(char *pat, char *rep, char *start_pat)
{
	int len_pat , len_rep, len_diff;

	len_pat = strlen(pat);
	len_rep = strlen(rep);
	len_diff = len_rep-len_pat;
	/* only if we get the pattern */
	if (start_pat) {
		shift_str(start_pat+len_pat, len_diff); /* shift only the stuff after the pattern */
		astrcpy(start_pat, rep, len_rep, 0); /* just copy the rep without \0 at end */
	}
}

char *int_till_txt(char *str, int *res)
{
	char *start_inp, s[LINE_MAX];
	char *inp = s;
	astrcpy(inp, str, strlen(str), 1);

	for (start_inp=inp; isdigit(*inp); str++, inp++)
		;
	*inp = '\0';
	*res = atoi(start_inp);
	return str;
}

void stringify(char *dst, char **src)
{
	for (; NOTNULL(src); src+=1) {
		astrcpy(dst, *src, strlen(*src), 1);
		dst += strlen(*src);

		if (NOTNULL(src+1))
			astrcpy(dst++, " ", 1, 1);
	}
}
