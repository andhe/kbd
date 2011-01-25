#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include "xmalloc.h"
#include "findfile.h"
#include "nls.h"

char pathname[MAXPATHLEN];
static int ispipe;

void fpclose(FILE *fp) {
	if (ispipe)
	     pclose(fp);
	else
	     fclose(fp);
}

#define SIZE(a) (sizeof(a)/sizeof(a[0]))

static struct decompressor {
	char *ext;		/* starts with `.', has no other dots */
	char *cmd;
} decompressors[] = {
	{ ".gz", "gzip -d -c" },
	{ ".bz2", "bzip2 -d -c" },
	{ 0, 0 }
};

static FILE *
pipe_open(struct decompressor *dc) {
	char *pipe_cmd;
	FILE *fp;

	ispipe = 1;
	pipe_cmd = xmalloc(strlen(dc->cmd) + strlen(pathname) + 2);
	sprintf(pipe_cmd, "%s %s", dc->cmd, pathname);
	fp = popen(pipe_cmd, "r");
	if (fp == NULL)
		fprintf(stderr, _("error executing  %s\n"), pipe_cmd);
	xfree(pipe_cmd);
	return fp;
}

/* If a file PATHNAME exists, then open it.
   If is has a `compressed' extension, then open a pipe reading it */
static FILE *
maybe_pipe_open(void) {
	FILE *fp;
	char *t;
	struct decompressor *dc;

	if ((fp = fopen(pathname, "r")) != NULL) {
	    t = strrchr(pathname, '.');
	    if (t) {
		for (dc = &decompressors[0]; dc->cmd; dc++)
		    if (strcmp(t, dc->ext) == 0) {
			fclose(fp);
			return pipe_open(dc);
		    }
	    }
	}
	return fp;
}

static FILE *
findfile_by_fullname(char *fnam, char **suffixes) {
	FILE *fp = NULL;
	const char **sp;
	struct stat st;
	struct decompressor *dc;
	size_t fnam_len, sp_len;

	fnam_len = strlen(fnam);

	for (sp = suffixes; *sp; sp++) {
		if (*sp == 0)
			continue; /* we tried it already */

		sp_len = strlen(*sp);

		if (fnam_len + sp_len + 1 > sizeof(pathname))
			continue;

		sprintf(pathname, "%s%s", fnam, *sp);

		if(stat(pathname, &st) == 0
		   && S_ISREG(st.st_mode)
		   && (fp = fopen(pathname, "r")) != NULL)
			return fp;

		for (dc = &decompressors[0]; dc->cmd; dc++) {
			if (fnam_len + sp_len + strlen(dc->ext) + 1 > sizeof(pathname))
				continue;

			sprintf(pathname, "%s%s%s", fnam, *sp, dc->ext);

			if (stat(pathname, &st) == 0
			    && S_ISREG(st.st_mode)
			    && access(pathname, R_OK) == 0)
				return pipe_open(dc);
		}
	}

	return NULL;
}

static FILE *
findfile_in_dir(char *fnam, char *dir, int recdepth, char **suf) {
	FILE *fp = NULL;
	DIR *d;
	struct dirent *de;
	char *ff, *fdir, *p, *q, **sp;
	struct decompressor *dc;
	int secondpass = 0;

	ispipe = 0;

	ff = strchr(fnam, '/');
	if (ff) {
		fdir = xstrdup(fnam);
		fdir[ff-fnam] = 0; 	/* caller guarantees fdir != "" */
	} else
		fdir = 0;		/* just to please gcc */

	/* Scan the directory twice: first for files, then
	   for subdirectories, so that we do never search
	   a subdirectory when the directory itself already
	   contains the file we are looking for. */
 StartScan:
	d = opendir(dir);
	if (d == NULL) {
	    xfree(fdir);
	    return NULL;
	}
	while ((de = readdir(d)) != NULL) {
	    struct stat statbuf;
	    int okdir;

	    if (strcmp(de->d_name, ".") == 0 ||
		strcmp(de->d_name, "..") == 0)
		continue;

	    if (strlen(dir) + strlen(de->d_name) + 2 > sizeof(pathname))
		continue;

	    okdir = (ff && strcmp(de->d_name, fdir) == 0);

	    if ((secondpass && recdepth) || okdir) {
		char *a;

		a = xmalloc(strlen(dir) + strlen(de->d_name) + 2);
		sprintf(a, "%s/%s", dir, de->d_name);
		if (stat(a, &statbuf) == 0 &&
		    S_ISDIR(statbuf.st_mode)) {
			if (okdir)
				fp = findfile_in_dir(ff+1, a, 0, suf);
			if (!fp && recdepth)
				fp = findfile_in_dir(fnam, a, recdepth-1, suf);
			if (fp) {
				xfree(a);
				xfree(fdir);
				closedir(d);
				return fp;
			}
		}
		xfree(a);
	    }

	    if (secondpass)
		    continue;

	    /* Should we be in a subdirectory? */
	    if (ff)
		    continue;

	    /* Does d_name start right? */
	    p = &de->d_name[0];
	    q = fnam;
	    while (*p && *p == *q) p++,q++;
	    if (*q)
		    continue;

	    sprintf(pathname, "%s/%s", dir, de->d_name);
	    if (stat(pathname, &statbuf) != 0 || !S_ISREG(statbuf.st_mode))
		    continue;

	    /* Does tail consist of a known suffix and possibly
	       a compression suffix? */
	    for(sp = suf; *sp; sp++) {
		    int l;

		    if (!strcmp(p, *sp)) {
		            xfree(fdir);
		            closedir(d);
			    return maybe_pipe_open();
		    }

		    l = strlen(*sp);
		    if (strncmp(p,*sp,l) == 0) {
			for (dc = &decompressors[0]; dc->cmd; dc++)
			    if (strcmp(p+l, dc->ext) == 0) {
			        xfree(fdir);
			        closedir(d);
				return pipe_open(dc);
			    }
		    }
	    }
	}
	closedir(d);
	if (recdepth > 0 && !secondpass) {
		secondpass = 1;
		goto StartScan;
	}
	xfree(fdir);
	return NULL;
}

/* find input file; leave name in pathname[] */
FILE *
findfile(char *fnam, char **dirpath, char **suffixes) {
	char **dp, *dir;
	FILE *fp = NULL;
	int dl, recdepth;

	if (strlen(fnam) >= sizeof(pathname))
		return NULL;

	/* Try explicitly given name first */
	strcpy(pathname, fnam);
	fp = maybe_pipe_open();
	if (fp)
		return fp;

	/* Test for full pathname - opening it failed, so need suffix */
	/* (This is just nonsense, for backwards compatibility.) */
	if (*fnam == '/')
		return findfile_by_fullname(fnam, suffixes);

	/* Search a list of directories and directory hierarchies */
	for (dp = dirpath; (*dp && !fp); dp++) {
		recdepth = 0;
		dl = strlen(*dp);

		/* trailing stars denote recursion */
		while (dl && (*dp)[dl-1] == '*')
			dl--, recdepth++;

		/* delete trailing slashes */
		while (dl && (*dp)[dl-1] == '/')
			dl--;

		if (dl)
			dir = xstrndup(*dp, dl);
		else
			dir = xstrdup(".");

		fp = findfile_in_dir(fnam, dir, recdepth, suffixes);
		xfree(dir);
	}
	return fp;
}
