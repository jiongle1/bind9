/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#include	<ctype.h>
#include	<errno.h>
#include	<limits.h>
#include	<stdarg.h>
#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<sys/wait.h>
#include	<signal.h>
#include	<time.h>
#include	<unistd.h>

#include	<isc/commandline.h>

#include	"include/tests/t_api.h"

static char *Usage =	"\t-a               : run all tests\n"
			"\t-b <dir>         : chdir to dir before running tests"
			"\t-c <config_file> : use specified config file\n"
			"\t-d <debug_level> : set debug level to debug_level\n"
			"\t-h               : print test info\n"
			"\t-u               : print usage info\n"
			"\t-n <test_name>   : run specified test name\n"
			"\t-t <test_number> : run specified test number\n"
			"\t-x               : don't execute tests in a subproc\n"
			"\t-q <timeout>     : use 'timeout' as the timeout value\n";
/*
 *		-a		-->	run all tests
 *		-b dir		-->	chdir to dir before running tests
 *		-c config	-->	use config file 'config'
 *		-d		-->	turn on api debugging
 *		-h		-->	print out available test names
 *		-u		-->	print usage info
 *		-n name		-->	run test named name
 *		-tn		-->	run test n
 *		-x		-->	don't execute testcases in a subproc
 *		-q timeout	-->	use 'timeout' as the timeout value
 */

#define	T_MAXTESTS		256	/* must be 0 mod 8 */
#define	T_MAXENV		256
#define	T_DEFAULT_CONFIG	"t_config"
#define	T_BUFSIZ		256
#define	T_BIGBUF		4096
#define	T_TIMEOUT		60

int		T_debug;
int		T_timeout;
pid_t		T_pid;
static char	*T_config;
static char	T_tvec[T_MAXTESTS / 8];
static char	*T_env[T_MAXENV + 1];
static char	T_buf[T_BIGBUF];
static char	*T_dir;

static int	t_initconf(char *path);
static int	t_dumpconf(char *path);
static int	t_putinfo(const char *key, const char *info);
static char	*t_getdate(char *buf, size_t buflen);
static void	printhelp(void);
static void	printusage(void);
static void	t_sighandler();

static int	T_int;

static void
t_sighandler(int sig) {
	T_int = sig;
}

int
main(int argc, char **argv)
{

	int			c;
	int			tnum;
	int			subprocs;
	pid_t			deadpid;
	int			status;
	int			len;
	int			first;
	testspec_t		*pts;
	struct sigaction	sa;

	first = 1;
	subprocs = 1;
	T_timeout = T_TIMEOUT;

	/* -a option is now default */
	memset(T_tvec, 0xffff, sizeof(T_tvec));

	/* parse args */
	while ((c = isc_commandline_parse(argc, argv, ":at:c:d:n:huxq:b:")) != -1) {
		if (c == 'a') {
			/* flag all tests to be run */
			memset(T_tvec, 0xffff, sizeof(T_tvec));
		}
		else if (c == 'b') {
			T_dir = isc_commandline_argument;
		}
		else if (c == 't') {
			tnum = atoi(isc_commandline_argument);
			if ((tnum > 0) && (tnum < T_MAXTESTS)) {
				if (first) {
					/*
					 * turn off effect of -a default
					 * and allow multiple -t and -n
					 * options
					 */
					memset(T_tvec, 0, sizeof(T_tvec));
					first = 0;
				}
				/* flag test tnum to be run */
				tnum -= 1;
				T_tvec[tnum / 8] |= (0x01 << (tnum % 8));
			}
		}
		else if (c == 'c') {
			T_config = isc_commandline_argument;
		}
		else if (c == 'd') {
			T_debug = atoi(isc_commandline_argument);
		}
		else if (c == 'n') {
			pts = &T_testlist[0];
			tnum = 0;
			while (pts->pfv != NULL) {
				if (! strcmp(pts->func_name,
					     isc_commandline_argument)) {
					if (first) {
						memset(T_tvec, 0, sizeof(T_tvec));
						first = 0;
					}
					T_tvec[tnum/8] |= (0x01 << (tnum%8));
					break;
				}
				++pts;
				++tnum;
			}
			if (pts->pfv == NULL) {
				fprintf(stderr, "no such test %s\n",
					isc_commandline_argument);
				exit(1);
			}
		}
		else if (c == 'h') {
			printhelp();
			exit(0);
		}
		else if (c == 'u') {
			printusage();
			exit(0);
		}
		else if (c == 'x') {
			subprocs = 0;
		}
		else if (c == 'q') {
			T_timeout = atoi(isc_commandline_argument);
		}
		else if (c == ':') {
			fprintf(stderr, "Option -%c requires an argument\n",
						isc_commandline_option);
			exit(1);
		}
		else if (c == '?') {
			fprintf(stderr, "Unrecognized option -%c\n",
				isc_commandline_option);
			exit(1);
		}
	}

	/* set cwd */

	if (T_dir != NULL)
		(void) chdir(T_dir);

	/* we don't want buffered output */

	(void) setbuf(stdout, NULL);
	(void) setbuf(stderr, NULL);

	/* setup signals */

	sa.sa_flags = 0;
	sigfillset(&sa.sa_mask);
	sa.sa_handler = t_sighandler;
	(void) sigaction(SIGALRM, &sa, NULL);
	(void) sigaction(SIGINT,  &sa, NULL);

	/* output start stanza to journal */

	sprintf(T_buf, "%s:", argv[0]);
	len = strlen(T_buf);
	(void) t_getdate(T_buf + len, T_BIGBUF - len);
	t_putinfo("S", T_buf);

	/* setup the test environment using the config file */

	if (T_config == NULL)
		T_config = T_DEFAULT_CONFIG;

	t_initconf(T_config);
	if (T_debug)
		t_dumpconf(T_config);

	/* now invoke all the test cases */

	tnum = 0;
	pts = &T_testlist[0];
	while (*pts->pfv != NULL) {
		if (T_tvec[tnum / 8] & (0x01 << (tnum % 8))) {
			if (subprocs) {
				T_pid = fork();
				if (T_pid == 0) {
					(*pts->pfv)();
					exit(0);
				}
				else if (T_pid > 0) {

					T_int = 0;
					sa.sa_handler = t_sighandler;
					(void) sigaction(SIGALRM, &sa, NULL);
					alarm(T_timeout);

					deadpid = (pid_t) -1;
					while (deadpid != T_pid) {
						deadpid = waitpid(T_pid, &status, 0);
						if (deadpid == T_pid) {
							if (WIFSIGNALED(status)) {
								t_info("the test case caused an exception %d\n",
									WTERMSIG(status));
								t_result(T_UNRESOLVED);
							}
						}
						else if ((deadpid == -1) && (errno == EINTR) && T_int) {
							t_info("the test case was interrupted %d\n", T_int);
							kill(T_pid, SIGTERM);
							t_result(T_UNRESOLVED);
							T_int = 0;
						}
						else if ((deadpid == -1) &&
							 ((errno == ECHILD) || (errno == ESRCH)))
							break;
					}

					sa.sa_handler = SIG_IGN;
					(void) sigaction(SIGALRM, &sa, NULL);
					alarm(0);
				}
				else {
					t_info("fork failed, errno == %d\n", errno);
					t_result(T_UNRESOLVED);
				}
			}
			else {
				(*pts->pfv)();
			}
		}
		++pts;
		++tnum;
	}

	(void) t_getdate(T_buf, T_BIGBUF);
	t_putinfo("E", T_buf);

	return(0);
}

void
t_assert(const char *component, int anum, int class,
		const char *what, ...)
{
	int	n;
	va_list	args;

	(void) printf ("T:%s:%d:%s\n", component, anum, class == T_REQUIRED ?
		"A" : "C");

	/* format text to a buffer */
	va_start(args, what);
	n = vsprintf(T_buf, what, args);
	va_end(args);

	(void) t_putinfo("A", T_buf);
	(void) printf("\n");
}

void
t_info(const char *format, ...)
{
	va_list	args;

	va_start(args, format);
	(void) vsprintf(T_buf, format, args);
	va_end(args);
	(void) t_putinfo("I", T_buf);
}

void
t_result(int result)
{

	char	*p;

	switch (result) {
		case T_PASS:
			p = "PASS";
			break;
		case T_FAIL:
			p = "FAIL";
			break;
		case T_UNRESOLVED:
			p = "UNRESOLVED";
			break;
		case T_UNSUPPORTED:
			p = "UNSUPPORTED";
			break;
		case T_UNTESTED:
			p = "UNTESTED";
			break;
		default:
			p = "UNKNOWN";
			break;
	}
	printf("R:%s\n", p);
}

char *
t_getenv(const char *name)
{
	char	*n;
	char	**p;
	size_t	len;

	n = NULL;
	if (name && *name) {

		p = &T_env[0];
		len = strlen(name);

		while (*p != NULL) {
			if (strncmp(*p, name, len) == 0) {
				if ( *(*p + len) == '=') {
					n = *p + len + 1;
					break;
				}
			}
			++p;
		}
	}
	return(n);
}

/*
 *
 * read in the config file at path, initializing T_env
 *
 * note: no format checking for now ...
 *
 */

static int
t_initconf(char *path)
{

	int	n;
	int	rval;
	char	**p;
	FILE	*fp;

	rval = -1;

	fp = fopen(path, "r");
	if (fp != NULL) {
		n = 0;
		p = &T_env[0];
		while (n < T_MAXENV) {
			*p = t_fgetbs(fp);
			if (*p == NULL)
				break;
			if ((**p == '#') || (strchr(*p, '=') == NULL)) {
				/* skip comments and other junk */
				(void) free(*p);
				continue;
			}
			++p; ++n;
		}
		(void) fclose(fp);
		rval = 0;
	}

	return(rval);
}

/*
 *
 * dump T_env to stdout
 *
 */

static int
t_dumpconf(char *path)
{
	int	rval;
	char	**p;
	FILE	*fp;

	rval = -1;
	fp = fopen(path, "r");
	if (fp != NULL) {
		p = &T_env[0];
		while (*p != NULL) {
			printf("C:%s\n", *p);
			++p;
		}
		(void) fclose(fp);
		rval = 0;
	}
	return(rval);
}

/*
 *
 * read a newline or EOF terminated string from fp
 * on success:
 *	return a malloc'd buf containing the string with
 *	the newline converted to a '\0'.
 * on error:
 *	return NULL
 *
 * caller is responsible for freeing buf
 *
 */

char *
t_fgetbs(FILE *fp)
{

	int	c;
	size_t	n;
	size_t	size;
	char	*buf;
	char	*p;

	n	= 0;
	size	= T_BUFSIZ;
	buf	= (char *) malloc(T_BUFSIZ * sizeof(char));

	if (buf != NULL) {
		p = buf;
		while ((c = fgetc(fp)) != EOF) {

			if (c == '\n')
				break;

			*p++ = c;
			++n;
			if ( n >= size ) {
				size += T_BUFSIZ;
				buf = (char *) realloc(buf, size * sizeof(char));
				if (buf == NULL)
					break;
				p = buf + n;
			}
		}
		*p = '\0';
		return(((c == EOF) && (n == 0)) ? NULL : buf);
	}
	else {
		fprintf(stderr, "malloc failed %d", errno);
		return(NULL);
	}
}

/*
 *
 * put info to log, using key
 * for now, just dump it out.
 * later format into pretty lines
 *
 */

static int
t_putinfo(const char *key, const char *info)
{
	int	rval;

	/* for now */
	rval = printf("%s:%s", key, info);
	return(rval);
}

static char *
t_getdate(char *buf, size_t buflen)
{
	size_t		n;
	time_t		t;
	struct tm	*p;

	t = time(NULL);
	p = localtime(&t);
	n = strftime(buf, buflen - 1, "%A %d %B %H:%M:%S %Y\n", p);
	return(n != 0 ? buf : NULL);
}

/* some generally used utilities */

struct dns_errormap {
	dns_result_t	result;
	char		*text;
} dns_errormap[] = {
	{	DNS_R_SUCCESS,		"DNS_R_SUCCESS"		},
	{	DNS_R_NOMEMORY,		"DNS_R_NOMEMORY"	},
	{	DNS_R_NOSPACE,		"DNS_R_NOSPACE"		},
	{	DNS_R_LABELTOOLONG,	"DNS_R_LABELTOOLONG"	},
	{	DNS_R_BADESCAPE,	"DNS_R_BADESCAPE"	},
	{	DNS_R_BADBITSTRING,	"DNS_R_BADBITSTRING"	},
	{	DNS_R_BITSTRINGTOOLONG,	"DNS_R_BITSTRINGTOOLONG"},
	{	DNS_R_EMPTYLABEL,	"DNS_R_EMPTYLABEL"	},
	{	DNS_R_BADDOTTEDQUAD,	"DNS_R_BADDOTTEDQUAD"	},
	{	DNS_R_UNEXPECTEDEND,	"DNS_R_UNEXPECTEDEND"	},
	{	DNS_R_NOTIMPLEMENTED,	"DNS_R_NOTIMPLEMENTED"	},
	{	DNS_R_UNKNOWN,		"DNS_R_UNKNOWN"		},
	{	DNS_R_BADLABELTYPE,	"DNS_R_BADLABELTYPE"	},
	{	DNS_R_BADPOINTER,	"DNS_R_BADPOINTER"	},
	{	DNS_R_TOOMANYHOPS,	"DNS_R_TOOMANYHOPS"	},
	{	DNS_R_DISALLOWED,	"DNS_R_DISALLOWED"	},
	{	DNS_R_NOMORE,		"DNS_R_NOMORE"		},
	{	DNS_R_EXTRATOKEN,	"DNS_R_EXTRATOKEN"	},
	{	DNS_R_EXTRADATA,	"DNS_R_EXTRADATA"	},
	{	DNS_R_TEXTTOOLONG,	"DNS_R_TEXTTOOLONG"	},
	{	DNS_R_RANGE,		"DNS_R_RANGE"		},
	{	DNS_R_EXISTS,		"DNS_R_EXISTS"		},
	{	DNS_R_NOTFOUND,		"DNS_R_NOTFOUND"	},
	{	DNS_R_SYNTAX,		"DNS_R_SYNTAX"		},
	{	DNS_R_BADCKSUM,		"DNS_R_BADCKSUM"	},
	{	DNS_R_BADAAAA,		"DNS_R_BADAAAA"		},
	{	DNS_R_NOOWNER,		"DNS_R_NOOWNER"		},
	{	DNS_R_NOTTL,		"DNS_R_NOTTL"		},
	{	DNS_R_BADCLASS,		"DNS_R_BADCLASS"	},
	{	DNS_R_UNEXPECTEDTOKEN,	"DNS_R_UNEXPECTEDTOKEN"	},
	{	DNS_R_BADBASE64,	"DNS_R_BADBASE64"	},
	{	DNS_R_PARTIALMATCH,	"DNS_R_PARTIALMATCH"	},
	{	DNS_R_NEWORIGIN,	"DNS_R_NEWORIGIN"	},
	{	DNS_R_UNCHANGED,	"DNS_R_UNCHANGED"	},
	{	DNS_R_BADTTL,		"DNS_R_BADTTL"		},
	{	DNS_R_NOREDATA,		"DNS_R_NOREDATA"	},
	{	DNS_R_CONTINUE,		"DNS_R_CONTINUE"	},
	{	DNS_R_DELEGATION,	"DNS_R_DELEGATION"	},
	{	DNS_R_GLUE,		"DNS_R_GLUE"		},
	{	DNS_R_DNAME,		"DNS_R_DNAME"		},
	{	DNS_R_CNAME,		"DNS_R_CNAME"		},
	{	DNS_R_NXDOMAIN,		"DNS_R_NXDOMAIN"	},
	{	DNS_R_NXRDATASET,	"DNS_R_NXRDATASET"	},
	{	DNS_R_BADDB,		"DNS_R_BADDB"		},
	{	DNS_R_ZONECUT,		"DNS_R_ZONECUT"		},
	{	(dns_result_t) 0,	NULL			}
};

dns_result_t
t_dns_result_fromtext(char *name) {

	dns_result_t		result;
	struct dns_errormap	*pmap;

	result = DNS_R_UNEXPECTED;

	pmap = dns_errormap;
	while (pmap->text != NULL) {
		if (strcmp(name, pmap->text) == 0)
			break;
		++pmap;
	}

	if (pmap->text != NULL)
		result = pmap->result;

	return(result);
}

struct dc_method_map {
	int	dc_method;
	char	*text;
} dc_method_map[] = {
	
	{	DNS_COMPRESS_NONE,	"DNS_COMPRESS_NONE"	},
	{	DNS_COMPRESS_GLOBAL14,	"DNS_COMPRESS_GLOBAL14"	},
	{	DNS_COMPRESS_GLOBAL16,	"DNS_COMPRESS_GLOBAL16"	},
	{	DNS_COMPRESS_GLOBAL,	"DNS_COMPRESS_GLOBAL"	},
	{	DNS_COMPRESS_LOCAL,	"DNS_COMPRESS_LOCAL"	},
	{	DNS_COMPRESS_ALL,	"DNS_COMPRESS_ALL"	},
	{	0,			NULL			}
};

int
t_dc_method_fromtext(char *name) {

	int			dc_method;
	struct dc_method_map	*pmap;

	dc_method = DNS_COMPRESS_NONE;

	pmap = dc_method_map;
	while (pmap->text != NULL) {
		if (strcmp(name, pmap->text) == 0)
			break;
		++pmap;
	}

	if (pmap->text != NULL)
		dc_method = pmap->dc_method;

	return(dc_method);
}

int
t_bustline(char *line, char **toks) {

	int	cnt;
	char	*p;

	cnt = 0;
	if (line && *line) {
		while ((p = strtok(line, "\t")) && (cnt < T_MAXTOKS)) {
			*toks++ = p;
			line = NULL;
			++cnt;
		}
	}
	return(cnt);
}

static void
printhelp() {
	int		cnt;
	testspec_t	*pts;

	cnt = 1;
	pts = &T_testlist[0];

	printf("Available tests:\n");
	while (pts->func_name) {
		printf("\t%d\t%s\n", cnt, pts->func_name);
		++pts;
		++cnt;
	}
}

static void
printusage() {
	printf("Usage:\n%s\n", Usage);
}

int
t_eval(char *filename, int (*func)(char **), int nargs) {


	FILE		*fp;
	char		*p;
	int		line;
	int		cnt;
	int		result;
	int		nfails;
	int		nprobs;
	char		*tokens[T_MAXTOKS + 1];

	nfails = 0;
	nprobs = 0;

	fp = fopen(filename, "r");
	if (fp != NULL) {
		line = 0;
		while ((p = t_fgetbs(fp)) != NULL) {

			++line;

			/* skip comment lines */
			if ((isspace((int)*p)) || (*p == '#'))
				continue;

			cnt = t_bustline(p, tokens);
			if (cnt == nargs) {
				result = func(tokens);
				if (result != T_PASS) {
					if (result == T_FAIL)
						++nfails;
					else
						++nprobs;
				}
			}
			else {
				t_info("bad format in %s at line %d\n",
						filename, line);
				++nprobs;
			}

			(void) free(p);
		}
		(void) fclose(fp);
	}
	else {
		t_info("Missing datafile %s\n", filename);
		++nprobs;
	}

	result = T_UNRESOLVED;

	if ((nfails == 0) && (nprobs == 0))
		result = T_PASS;
	else if (nfails)
		result = T_FAIL;

	return(result);
}

