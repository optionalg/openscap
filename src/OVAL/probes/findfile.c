#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <dirent.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <regex.h>

#include "findfile.h"

typedef struct {
	regex_t re;
	char *file;
	char *direction;
	char *follow;
        fsdev_t *dev_list;
	int (*cb) (const char * pathname, const char *filename, void *arg);
} setting_t;

typedef struct {
	int pathc;
	char **pathv;
	int offs;
} rglob_t;

static int find_files_recursion(const char* path, setting_t * setting, int depth, void *arg );
static int recurse_direction(const char *file, char *direction);
static int recurse_follow(struct stat *st, char *follow);
static int noRegex(char * token);
static int rglob(const char *pattern, rglob_t *result);
static void find_paths_recursion(const char *path, regex_t *re, rglob_t *result );
static void rglobfree(rglob_t * result);

/* 
 *
 * ************* PUBLIC *************  
 *
 */
int find_files(SEXP_t * spath, SEXP_t *sfilename, SEXP_t *behaviors,
               int (*cb) (const char *pathname, const char *filename, void *arg), void *arg) {
	char *name = NULL, *path = NULL;
	int i, rc;
	int max_depth;
	rglob_t rglobbuf;
	setting_t *setting;	
	int finds = 0;

	name = SEXP_string_cstr(SEXP_OVALelm_getval(sfilename, 1));
	path = SEXP_string_cstr(SEXP_OVALelm_getval(spath, 1));
	SEXP_number_get(SEXP_OVALelm_getattrval(behaviors,"max_depth"),&max_depth, NUM_INT32);

	setting = calloc(1, sizeof(setting_t));
	setting->direction = SEXP_string_cstr(SEXP_OVALelm_getattrval(behaviors,"recurse_direction"));
	setting->follow = SEXP_string_cstr(SEXP_OVALelm_getattrval(behaviors,"recurse"));
	setting->cb = cb;


	/* Init list of local devices */
	if( !SEXP_strncmp(SEXP_OVALelm_getattrval(behaviors,"recurse_file_system"), "local",6) ) {
                if ((setting->dev_list = fsdev_init (NULL, 0)) == NULL) {
			goto error;
		}
	}
        
	/* Filename */
	if( !SEXP_strncmp(SEXP_OVALelm_getattrval(sfilename,"operation"), "pattern match", 14) ) {
		if( regcomp(&(setting->re), name, REG_EXTENDED) != 0 ) {
	       	        goto error;
		}
		setting->file = NULL;
	}
	else {
		setting->file = strdup(name);
	}

	/* Is there a '/' at the end of the path? */
	i = strlen(path)-1;
	if( path[i] == '/' )
		path[i] = '\0';

	/* Evaluate path(s) */
	if( !SEXP_strncmp(SEXP_OVALelm_getattrval(spath,"operation"), "pattern match", 14) ) {
		rglobbuf.offs=10;
		rc = rglob(path, &rglobbuf);
		if(!rc && rglobbuf.pathc > 0) {
			finds = 0;
			for(i=0; i < rglobbuf.pathc; i++) {
				rc = find_files_recursion(rglobbuf.pathv[i], setting, max_depth, arg);
				if( rc == 0 ) {/* add path, no files found*/
					(*cb)(rglobbuf.pathv[i], NULL, arg);
					rc++;
				}
				if( rc >= 0 )
					finds += rc;
			}
			rglobfree(&rglobbuf);
		}
	}
	else {
		rc = find_files_recursion(path, setting, max_depth, arg);
		if( rc == 0 ) { /* add path, no files found */
			(*cb)(path, NULL, arg);
			rc++;
		}
		if( rc >= 0 )
			finds += rc;
	}


error:
	free(name);
	free(path);

	free(setting->follow);
	free(setting->direction);
	if (setting->dev_list != NULL)
		fsdev_free (setting->dev_list);
	if (setting->file != NULL)
		free(setting->file);
	else
		regfree(&(setting->re));
	free(setting);
	
	return finds;
}


/* 
 *
 * ************* LOCAL *************  
 *
 */


/*
 *  depth defines how many directories to recurse when a recurse direction is specified
 *  '-1' means no limitation
 *  '0' is equivalent to no recursion
 *  '1' means to step only one directory level up/down
 */
static int find_files_recursion(const char* path, setting_t * setting, int depth, void *arg ) {
	struct stat st;
	struct dirent * pDirent;
	char path_new[PATH_MAX];
	DIR * pDir;
	int rc, tmp;

	pDir = opendir(path);
	if( pDir == NULL) 
		return -1;

	rc = 0;
	while ( (pDirent = readdir(pDir)) ) {
		if (PATH_MAX < strlen(path) + 1 + pDirent->d_reclen + 1)
			continue;
		sprintf(path_new,"%s/%s",path,pDirent->d_name);

		if( lstat(path_new, &st) == -1)
			continue;

		if( depth &&                                                   /* how deep rabbit hole is? */
		    recurse_direction(pDirent->d_name, setting->direction) &&  /* up or down direction? */
		    recurse_follow(&st,setting->follow) &&                     /* follow symlinks? */
		    fsdev_search (setting->dev_list, &st.st_dev)) {            /* local filesystem? */
			tmp = find_files_recursion(path_new, setting, depth == - 1 ? -1 : depth - 1, arg);
			if( tmp >=0 )
				rc += tmp;
		}

		if( !S_ISDIR(st.st_mode) ) {
			/* match filename*/
			if( setting->file ) {
				if(!strncmp(setting->file, pDirent->d_name, pDirent->d_reclen)) {
					rc++;
					(setting->cb)(path, pDirent->d_name, arg);
				}
			}
			else {
				if( regexec(&(setting->re), pDirent->d_name, 0, NULL, 0) == 0 ) {
					rc++;
					(setting->cb)(path, pDirent->d_name, arg);
				}
			}
		}
	}

	closedir(pDir);
	return rc;
}

/*
 * What to follow during recursion?
 * 'symlinks and directories' --default
 * 'directories'
 * 'symlinks'
 */
static int recurse_follow(struct stat *st, char *follow) {
	
	if ( !(S_ISDIR(st->st_mode) || S_ISLNK(st->st_mode)) )
		return 0;

	/* default */
	if( (!follow || !strncmp(follow, "symlinks and directories",24)) )
		return 1;

	/* deprecated */
	if( !strncmp(follow,"files and directories",12)  && S_ISDIR(st->st_mode)  )
		return 1;

	/* one or another, not very usual case */
	if( !strncmp(follow,"symlinks",9)  && S_ISLNK(st->st_mode) )
		return 1;	
	if( !strncmp(follow,"directories",12)  && S_ISDIR(st->st_mode) )
		return 1;

	return 0;
}

/*
 * Up or down? 
 * 'none' - default
 * 'up' to parent directories 
 * 'down' into child directories
 */
 
static int recurse_direction(const char *file, char *direction) {

	if (!strncmp(direction,"down",5)) {
		if( strncmp(file,"..",3) && strncmp(file,".",2) )
			return 1;
	}
	else if (!strncmp(direction,"up",3)) {
		if( !strncmp(file,"..",3) )
			return 1;
	}
	/* default none*/
	return 0;
}

/*
 * Similar to glob function, but instead off shell wildcard 
 * use regexps 
 */
static int rglob(const char *pattern, rglob_t *result) {
	char * tmp, * token, * saveptr;
	char path[PATH_MAX] = "/";
	int len = 1;
	regex_t re;

	/* check pattern */
	if( !pattern || pattern[0]!='/')
		return 1;

	/* get no regexp portion from pattern to path*/
	tmp = strdup (pattern);
	for ( ; ; tmp=NULL) {
		token = strtok_r(tmp, "/", &saveptr);
		if( token==NULL)
			break;
		if( noRegex(token) ) {
			strcat(path,token);
			strcat(path,"/");
		} else
			break;
	}
	free(tmp);
	/* erase last slash, but not the first one! */
	len = strlen(path);
	if ( len > 1 )
		path[len-1] = '\0';


	/* init regex machine */
	 if( regcomp(&re, pattern, REG_EXTENDED) != 0 ) 
                return 1; /* Can't init pattern buffer storage area */

	/* allocate memory for result */
	if ( result->offs < 1 || result->offs > 1000 )
		result->offs=10;
	result->pathv = malloc( sizeof (char**) * result->offs);
	result->pathc=0;

	/* find paths */
	find_paths_recursion(path, &re, result );

	regfree(&re);
	return 0;
	
}

static void find_paths_recursion(const char *path, regex_t *re, rglob_t *result ) {
        struct dirent * pDirent;
	struct stat st;
        DIR * pDir;
	char path_new[PATH_MAX];

        pDir = opendir(path);
        if( pDir == NULL)
                return;

	if( regexec(re, path, 0, NULL, 0) == 0 ) { /* catch? */
		result->pathv[result->pathc] = strdup(path);
		result->pathc++;
		if( result->pathc == result->offs) {
			result->offs = result->offs * 2; /* magic constant */
			result->pathv = realloc(result->pathv, sizeof (char**) * result->offs);
		}
	}

	while ( (pDirent = readdir (pDir)) ) {
		if (PATH_MAX < strlen(path) + 1 + pDirent->d_reclen + 1)
			continue;
		sprintf(path_new,"%s/%s",path,pDirent->d_name);

                if( lstat(path_new, &st) == -1)
                        continue;

		if( S_ISDIR(st.st_mode) && strncmp(pDirent->d_name,"..",3) && strncmp(pDirent->d_name,".",2) ) {
			find_paths_recursion(path_new, re, result ); /* recursion */
		}
	}
	closedir(pDir);
}

static int noRegex(char * token) {
	const char regexChars[] = "^$\\.[](){}*+?";
	size_t i,j,token_len;
	
	token_len = strlen(token);
	for(i=0; i<token_len; i++) {
		if( strchr(regexChars, token[i]) )
			return 0;
	}
	return 1;
}

static void rglobfree(rglob_t * result) {
	int i;

	for(i=0; i<result->pathc; i++) {
		/*printf("result->pathv[%d]: %s\n", i, result->pathv[i]);*/
		free(result->pathv[i]);
	}
	free(result->pathv);

	result->pathc=0;
}

