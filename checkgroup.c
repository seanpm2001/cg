#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <getopt.h>
#include <regex.h>
#include <map.h>
#include "ranges.h"
#include "util.h"
#include "decode.h"
#include "mime.h"
#include "header.h"

#define NNTPHOSTFILE "/etc/nntpserver"
#define DEFAULTEDITOR "vi"
#define DECODER "uudeview -f -i -n -q"
#define BINHEX "bhdeview"
#define BUFSIZE 8192
#define RCNAME ".decoderc"

struct file {
    char *tag;
    char *comment;
    int npart;
    long *artno;
    int new;
    long size;
};

#define MAX_PATTERNS 5

char *spattern[MAX_PATTERNS] = {  /* ! ( ! */
/*    "(.*) - (.*[^ ]) *[[(]([0-9]*)/([0-9]*)[])]",   /* "c - n ()" */
    "(([^-]|-+[^ -])+)-+ (.*[^ ]) *[[(]([0-9]*)[/\\]([0-9]*)[])]", /* c - n () */
    "- (.*[^ ]) *[[(]([0-9]*)/([0-9]*)[])] *(.*)",  /* "- n () c" mac groups */
    "(.*)()[[(]([0-9]*)/([0-9]*)[])]",              /* "n ()" desperate */
    "(.*)[[( ]([0-9]+) of ([0-9]+)[]) ](.*)",       /* "c[x of y]n" */
    "(.*)  *([^ ]*\\.[a-z0-9][a-z0-9][a-z0-9]) *$"  /* "c n.ext" */
};

regex_t pattern[MAX_PATTERNS]; 

int pat_key[MAX_PATTERNS] =     {/* 2 */ 3, 1, 1, 4 /* 2 */, 2 };
int pat_comment[MAX_PATTERNS] = {/* 1 */ 1, 4, 2, 1 /* 4 */, 1 };
int pat_part[MAX_PATTERNS] =    {/* 3 */ 4, 2, 3, 2 /* 1 */, 0 };
int pat_npart[MAX_PATTERNS] =   {/* 4 */ 5, 3, 4, 3 /* 3 */, 0 };

char *prg;
char *nntp_response, *nntp_group, *nntp_host;
char *decoder = DECODER;
struct range *rcmap;

char *newsrc;
#define DEFAULT_NEWSRC  "~/.newsrc"
int mark_complete;

FILE *conin, *conout, *dfile;



#include "config.h"

char version_string[] = 
PACKAGE " " VERSION "\n\
Copyright (C) 1997 Dieter Baron, Thomas Klausner\n"
PACKAGE " comes with ABSOLUTELY NO WARRANTY, to the extent permitted by law.\n\
You may redistribute copies of\n"
PACKAGE " under the terms of the GNU General Public License.\n\
For more information about these matters, see the files named COPYING.\n";

char usage_string[] = "\
Usage: %s [-cm] [-n rcfile] group ...\n";

char help_string[] = "\
\n\
  -h, --help            display this help message\n\
  -V, --version         display version number\n\
\n\
  -m, --mac             call hexbin decoder\n\
  -n, --newsrc FILE     use FILE as newsrc file\n\
  -c, --mark-complete   mark only parts of complete files as read\n\
\n\
Report bugs to <cg-bugs@giga.or.at>.\n";

#define OPTIONS	"hVmn:c"

struct option options[] = {
    { "help",          0, 0, 'h' },
    { "version",       0, 0, 'V' },
    { "mac",           0, 0, 'm' },
    { "newsrc",        1, 0, 'n' },
    { "mark-complete", 0, 0, 'c' },
    { NULL,            0, 0, 0   }
};


/* extern */
int sopen(char *host, char *service);

/* intern */
long complete (map *parts, long no_file, struct file **todec);
long *choose (struct file **todec, long no_complete, char *group);
int parse(map *parts, FILE *f);
char *extract(char *s, regmatch_t m);
int decode(struct file *value);
int nntp_resp(void);
int nntp_put(char *fmt, ...);
int writerc(char *group);
int readrc(char *group, long lower, long upper, long no_art);
void writegrouptorc (FILE *copy, char *compstr);



int
main(int argc, char **argv)
{
    map *parts;
    int err, fd, verbose, i, c, k;
    long no_art, lower, upper, no_complete, *toget, gute, j, no_file;
    char b[BUFSIZE];
    struct file **todec;
    FILE *fp;

    prg = argv[0];

    nntp_group = NULL;
    newsrc = DEFAULT_NEWSRC;
    mark_complete = 0;
    errfilename[0] = '\0';
    errpartno = errlineno = 0;

    mime_init();
    header_init();

    opterr = 0;
    while ((c=getopt_long(argc, argv, OPTIONS, options, 0)) != EOF) {
	switch (c) {
	case 'm':
	    decoder = BINHEX;
	    break;
	case 'n':
	    newsrc = optarg;
	    break;
	case 'c':
	    mark_complete = 0;
	    break;
	case 'h':
	    printf(usage_string, prg);
	    fputs(help_string, stdout);
	    exit(0);
	case 'V':
	    fputs(version_string, stdout);
	    exit(0);
	default:
	    fprintf(stderr, usage_string, prg);
	    exit(1);
	}
    }

    if (optind == argc) {
	fprintf(stderr, usage_string, prg);
	exit(1);
    }

    newsrc = expand(newsrc);

    dfile = fopen(".debug", "w");

    for (i=0;i<MAX_PATTERNS;i++) {
	if ((err=regcomp(&pattern[i], spattern[i], REG_EXTENDED|REG_ICASE))
	    != 0) {
	    regerror(err, &pattern[i], b, BUFSIZE);
	    fprintf(stderr, "%s: can't compile regex pattern %d: %s\n", prg,
		    i, b);
	    exit(1);
	}
    }
    
    err = 0;

    if ((nntp_host=getenv("NNTPSERVER")) == NULL) {
	if ((fp=fopen(NNTPHOSTFILE, "r")) == NULL) {
	    prerror(errnone, "can't open %s: %s", NNTPHOSTFILE,
		    strerror(errno));
	    exit(7);
	}
	if (fgets(b, BUFSIZE, fp) == NULL) {
	    prerror(errnone, "can't read newsserver from %s: %s",
		    NNTPHOSTFILE, strerror(errno));
	    exit(7);
	}
	fclose(fp);
	if (b[strlen(b)-1]=='\n')
	    b[strlen(b)-1]='\0';
	if ((nntp_host=strdup(b)) == NULL) {
	    prerror(errnone, "can't strdup nntp_host from `%s': shoot me", b);
	    exit(77);
	}
    }
    
    
    /* talk to server */
    if ((fd=sopen(nntp_host, "nntp")) == -1)
	return -1;
    
    conin = fdopen(fd, "r");
    conout = fdopen(fd, "w");

    if ((nntp_resp() & ~1) != 200) {
	fprintf(stderr, "%s: server %s says: %s\n", prg, nntp_host,
		nntp_response);
	exit(3);
    }

    nntp_put("mode reader");

    for (i=optind;i<argc;i++) {
	if (nntp_put("group %s", argv[i]) != 211) {
	    fprintf(stderr, "%s: group %s failed: %s\n", prg, argv[i],
		    nntp_response);
	    continue;
	}

	if (sscanf(nntp_response, "%*d %ld %ld %ld", &no_art, &lower, &upper)
	    != 3) {
	    fprintf(stderr, "%s: can't parse group %s reply: %s\n", prg,
		    argv[i], nntp_response);
	    continue;
	}

	if (no_art == 0) {
	    printf("%s: no new files found in %s\n", prg, argv[i]);
	    continue;
	}

	if (lower > upper || no_art < 0 || lower < 0 || upper < 0) {
	    fprintf(stderr, "%s: invalid response from newsserver"
		    " for group %s: %s\n", prg, argv[i], nntp_response);
	    continue;
	}
	
	readrc(argv[i], lower, upper, no_art);
	
	if (nntp_put("xover %ld-%ld", lower, upper) != 224) {
	    fprintf(stderr, "%s: xover for group %s failed: %s\n", prg,
		    argv[i], nntp_response);
	    continue;
	}
    
	if ((parts=map_new(no_art*2)) == NULL) {
	    fprintf(stderr, "%s: can't create part map\n", prg);
	    exit(1);
	}
	
	no_file=parse(parts, conin);

	if ((todec=(struct file **)malloc(sizeof(struct file *)*no_file))
	    == NULL) {
	    fprintf(stderr, "%s: malloc failure\n", prg);
	    exit(1);
	}

	no_complete=complete(parts, no_file, todec);

	map_free(parts, 0);

	if (no_complete == 0) {
	    printf("%s: no new files found in %s\n", prg, argv[i]);
	    continue;
	}

	toget = choose(todec, no_complete, argv[i]);
	/* XXX: The lines below are just a patch -- better handling ??*/
	if (toget == NULL) {       /* error handling */
	    for (j=0; j<no_complete; j++) {
		free(todec[j]->comment);
		free(todec[j]->tag);
		free(todec[j]->artno);
		free(todec[j]);
	    }	    
	    free(todec);
	    continue;
	}
	
	gute=0;
	
	for (j=0; toget[j]!=-1;j++) {
	    if (decode(todec[toget[j]]) == 0) {
		for (k=0; k<todec[toget[j]]->npart; k++)
		    range_clear(rcmap, todec[toget[j]]->artno[k]);
	    }
	    else
		gute++;
	}
		
	no_file = j;

	for (j=0; j<no_complete; j++) {
	    free(todec[j]->comment);
	    free(todec[j]->tag);
	    free(todec[j]->artno);
	    free(todec[j]);
	}	    
	free(toget);
	free(todec);

	writerc(argv[i]);
	
	if (verbose)
	    printf("%s: %ld found, %ld chosen, %ld decoded\n",
		   argv[i], no_complete, no_file, gute);
    }

    exit(0);
}



int compfile (const void *a, const void *b);

long
complete (map *parts, long no_file, struct file **todec)
{
    long i,j;
    map_iter *iterate;
    char *key;
    struct file *value;
    
    if ((iterate=map_start(parts)) == NULL) {
	fprintf(stderr, "%s: can't iterate part map\n", prg);
	exit(1);
    }

    for (i=0; map_next(iterate, (void *)&key, (void *)&value) == 0; ) {
	if (value->new) {
	    for (j=0; j<value->npart; j++) {
		if (value->artno[j] == -1)
		    break;
	    }
	}
	if ((j == value->npart) && value->new) {
	    todec[i++]=value;
	    if (mark_complete) {
		for (j=0; j<value->npart; j++)
		    range_set(rcmap, value->artno[j]);
	    }
	}
	else {
	    free(value->comment);
	    free(value->tag);
	    free(value->artno);
	    free(value);
	}
    }
    map_stop(iterate);

    qsort(todec, i, sizeof(struct file *), compfile);
    
    return i;
}



long *
choose (struct file **todec, long no_complete, char *group)
{
    char b[BUFSIZE], fname[BUFSIZE], *editor;
    long i, j;
    long *chosen;
    FILE *temp;
    
    sprintf(fname, "00-%s-%d", group, (int)getpid());

    if ((temp=fopen(fname, "w")) == NULL) {
	fprintf(stderr,"%s: tempfile failure: Und Tschuess!\n", prg);
	exit(2);
    }

    for (i=0; i<no_complete; i++) {
	fprintf(temp,"%5ld [%5ldk] %.80s --- %s\n", i+1,
		(todec[i]->size+1023)/1024, todec[i]->tag,
		todec[i]->comment);
    }

    fclose(temp);

    editor=getenv("VISUAL");
    if (editor == NULL)
	editor=getenv("EDITOR");
    if (editor == NULL)
	editor=DEFAULTEDITOR;

    sprintf(b,"%s %s", editor, fname);
    system(b);

    if ((temp=fopen(fname,"r")) == NULL) {
	fprintf(stderr, "%s: couldn't open %s for reading: %s\n",
		prg, fname, strerror(errno));
	return NULL;
    }
    
    i=0;

    chosen=(long *)malloc(sizeof(long)*(no_complete+1));
    
    while (fgets(b, BUFSIZE, temp) != NULL) {
	j = 0;
	sscanf(b,"%ld",&j);
	if ((j > 0) && (j < no_complete+1))
	    chosen[i++]=j-1;
    }

    fclose(temp);
    
    chosen[i]=-1;
    
    return chosen;
}




int
compfile (const void *a, const void *b)
{
    return strcasecmp((*(struct file **)a)->tag, (*(struct file **)b)->tag);
}



int
parse(map *parts, FILE *f)
{
    regmatch_t match[6];
    struct file *val, **valp;
    char *key, *s, *subj, *comment;
    char b[8192];
    int npart, part, i, end, l;
    long artno, no_file, size;

    end = 0;
    no_file = 0;

    while (fgets(b, 8192, f)) {
	l=strlen(b);
	
	/* normalization */
	if (b[l-1] == '\n')
	    b[--l] = '\0';
	if (b[l-1] == '\r')
	    b[--l] = '\0';

	if (b[0] == '.' && b[1] == '\0') {
	    end = 1;
	    break;
	}

	s = strtok(b, "\t");
	artno = strtol(s, NULL, 10);

	subj = strtok(NULL, "\t");

	/* auth = */  strtok(NULL, "\t");
	/* date = */  strtok(NULL, "\t");
	/* msgid = */ strtok(NULL, "\t");

	s = strtok(NULL, "\t");
	while (s != NULL && s[0] == '<')
	    s = strtok(NULL, "\t");
	if (s == NULL) {
	    /* DEBUG */ fprintf(dfile,
				"%s: xover for article %ld is weird\n",
				prg, artno);
	    size = 0;
	}
	else {
	    size = strtol(s, NULL, 10);
	    /* lines = */ strtok(NULL, "\t");
	    /* xref = */ strtok(NULL, "\t");
	}
	
	for (i=0; i<MAX_PATTERNS; i++) {
	    if (regexec(&pattern[i], subj, 6, match, 0) == 0)
		break;
	}
	
	if (i == MAX_PATTERNS) {
	    /* DEBUG */	fprintf(dfile, "%s\n", subj);
	    continue;
	}

	key = extract(subj, match[pat_key[i]]);
	comment = extract(subj, match[pat_comment[i]]);
	if (pat_part[i]) {
	    s = extract(subj, match[pat_part[i]]);
	    part = atoi(s);
	    free(s);
	}
	else
	    part = 1;
	if (pat_npart[i]) {
	    s = extract(subj, match[pat_npart[i]]);
	    npart = atoi(s);
	    free(s);
	}
	else
	    npart = 1;
	
	if (part == 0) {
	    /* XXX save info */
	    free(key);
	    free(comment);
	    if (!mark_complete)
		range_set(rcmap, artno);
	    continue;
	}

	if (npart == 0) {
	    /* DEBUG */ fprintf(dfile,"%s: ignored: number of parts zero\n", subj);
	    free(key);
	    free(comment);
	    if (!mark_complete)
		range_set(rcmap, artno);
	    continue;
	}

	sprintf(b, "%s_%d", key, npart);
	valp = (struct file **)map_insert(parts, b);
	if (*valp == NULL) {
	    no_file++;
	    if ((val=(struct file *)malloc(sizeof(struct file))) == NULL) {
		fprintf(stderr, "%s: malloc failure\n", prg);
		exit(1);
	    }
	    val->tag = key;
	    val->comment = comment;
	    val->npart = npart;
	    if (((val->artno=(long *)malloc(sizeof(long)*npart))
		 == NULL)) {
		fprintf(stderr, "%s: malloc failure\n", prg);
		exit(1);
	    }
	    for (i=0; i<npart; i++)
		val->artno[i] = -1;
	    val->new=0;
	    val->size = 0;

	    *valp = val;
	}
	else {
	    val = *valp;
	    free(key);
	    free(comment);
	}

	if (val->artno[part-1] != -1 ) {
	    /* DEBUG  fprintf(dfile, "%s: ignored: duplicate part %d\n",
	       val->tag, part); */
	    if (!mark_complete)
		range_set(rcmap, artno);
	    continue;
	}

	val->artno[part-1] = artno;
	if (!range_isin(rcmap, artno))
	    val->new=1;

	val->size += 3*size/4;
	
	if (!mark_complete)
	    range_set(rcmap, artno);

    }
    
    return no_file;
}



char *
extract(char *s, regmatch_t m)
{
    char *t;

    if ((t=(char *)malloc(m.rm_eo-m.rm_so+1)) == NULL) {
	fprintf(stderr, "%s: malloc failure\n", prg);
	exit(1);
    }

    strncpy(t, s+m.rm_so, m.rm_eo-m.rm_so);
    t[m.rm_eo-m.rm_so] = '\0';

    return t;
}



int
decode(struct file *val)
{
    enum enctype type, oldtype;
    FILE *fout;
    char b[60];
    int i, ret;

    printf("decoding `%s'\n", val->tag);
    sprintf(errfilename, "[%s]", val->tag);
    
    decode_line(b, NULL, NULL);

    type = enc_unknown;
    fout = NULL;

    for (i=0; i<val->npart; i++) {
	errpartno = i+1;
	ret = nntp_put("article %ld", val->artno[i]);
 	if (ret != 220 && ret != 224) {
	    prerror(errpart, "article %ld failed: %s\n",
		    val->artno[i], nntp_response);
	    errfilename[0] = '\0';
	    errpartno = 0;
	    return 0;
	}
	errlineno = 0;

	oldtype = type;
	type = decode_file(conin, &fout, type);

	switch(type) {
	case enc_nodata:
	    prerror(errpart, "no encoded data found");
	    
	case enc_error:
	    if (fout)
		fclose(fout);
	    /* XXX: next line should be removed when error handling below ok */
	    prerror(errpart, "decoding failed");
	    errfilename[0] = '\0';
	    errpartno = 0;
	    return 0;
	    
	case enc_eof:
	    if (i != val->npart-1) {
		prerror(errpart, "premature end of encoded data (%d"
			"parts expected)", val->npart);
		if (fout)
		    fclose(fout);
		errfilename[0] = '\0';
		errpartno = 0;
		return 0;
	    }
	    break;
	default:
	    break;
	}
    }

    if (fout)
	fclose(fout);
    
    if (oldtype != enc_base64 && type != enc_eof) {
	prerror(errfile, "end of encoded data not found");
	errfilename[0] = '\0';
	errpartno = 0;
	return 0;
    }

    errfilename[0] = '\0';
    errpartno = 0;
    return 1;
}



int
nntp_put(char *fmt, ...)
{
    int fd, ret, tries, writeerr;
    char buf[BUFSIZE];
    va_list argp;

    ret = tries = writeerr = 0;
    
    if (conout == NULL)
	return -1;
    
    va_start(argp, fmt);
    vsprintf(buf, fmt, argp);
    va_end(argp);

    for (;;) {
	fprintf(conout, "%s\r\n", buf);
    
	if (fflush(conout) || ferror(conout))
	    writeerr = 1;
	else 
	    ret = nntp_resp();
	
	/* connection to server closed -- reconnect */
	if (writeerr || (ret == 400) || (ret == 503)) {
	    writeerr = 0;
	    if (tries == 0) {
		tries++;
		fclose(conin);
		fclose(conout);

		if ((fd=sopen(nntp_host, "nntp")) == -1)
		    return -1;
		
		conin = fdopen(fd, "r");
		conout = fdopen(fd, "w");

		if ((nntp_resp() & ~1) != 200) {
		    fprintf(stderr, "%s: server %s says: %s\n", prg, nntp_host,
			    nntp_response);
		    return -1;
		}

		fprintf(conout, "mode reader\r\n");
		if (fflush(conout) || ferror(conout)) /* XXX: retry? */
		    return -1;
		
		nntp_resp();

		if (nntp_group != NULL) {
		    fprintf(conout, "%s\r\n", nntp_group);
		    if (fflush(conout) || ferror(conout)) /* XXX: retry? */
			return -1;
		    
		    if (nntp_resp() != 211) {
			fprintf(stderr, "%s: timed out -- can't reconnect",
				prg);
			return -1;
		    }
		}
			
		fprintf(stderr, "%s: timed out -- but reconnected\n", prg);
	    }
	    else {
		fprintf(stderr, "%s: timed out -- can't reconnect\n", prg);
		return -1;
	    }
	}
	else
	    break;
    }

    if (strncasecmp(buf, "group ", 6) == 0) {
	free(nntp_group);
	nntp_group = strdup(buf);
    }
    
    return ret;
}



int
nntp_resp(void)
{
    char line[BUFSIZE];
    int resp;
    
    if (conin == NULL)
	return -1;

    clearerr(conin);
    if (fgets(line, BUFSIZE, conin) == NULL)
	return -1;

    resp = atoi(line);
    free (nntp_response);
    nntp_response = strdup(line);

    return resp;
}
