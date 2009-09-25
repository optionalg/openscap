#ifndef __STUB_PROBE
/*
 * textfilecontent54 probe:
 *
 *  textfilecontent54_object
 *    textfilecontentbehaviors behaviors
 *    string path
 *    string filename
 *    string pattern
 *    int instance
 *
 *  textfilecontent_item
 *    attrs
 *      id
 *      status_enum status
 *    string path
 *    string filename
 *    string pattern
 *    int instance
 *    string line (depr)
 *    string text
 *    [0..*] anytype subexpression
 */

#include <config.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#if defined USE_REGEX_PCRE
#include <pcre.h>
#elif defined USE_REGEX_POSIX
#include <regex.h>
#endif

#include <seap.h>
#include <probe-api.h>
#include <findfile.h>

#define FILE_SEPARATOR '/'

#if defined USE_REGEX_PCRE
static int get_substrings(char *str, pcre *re, int want_substrs, char ***substrings) {
	int i, ret, rc;
	int ovector[60], ovector_len = sizeof (ovector) / sizeof (ovector[0]);

	// todo: max match count check

	for (i = 0; i < ovector_len; ++i)
		ovector[i] = -1;

	rc = pcre_exec(re, NULL, str, strlen(str), 0, 0,
		       ovector, ovector_len);

	if (rc < -1) {
		return -1;
	} else if (rc == -1) {
		/* no match */
		return 0;
	} else if(!want_substrs) {
		/* just report successful match */
		return 1;
	}

	char **substrs;

	ret = 0;
	if (rc == 0) {
		/* vector too small */
		rc = ovector_len / 3;
	}

	substrs = malloc(rc * sizeof (char *));
	for (i = 0; i < rc; ++i) {
		int len;
		char *buf;

		if (ovector[2 * i] == -1)
			continue;
		len = ovector[2 * i + 1] - ovector[2 * i];
		buf = malloc(len + 1);
		memcpy(buf, str + ovector[2 * i], len);
		buf[len] = '\0';
		substrs[ret] = buf;
		++ret;
	}
	/*
	  if (ret < rc)
	  substrs = realloc(substrs, ret * sizeof (char *));
	*/
	*substrings = substrs;

	return ret;
}
#elif defined USE_REGEX_POSIX
static int get_substrings(char *str, regex_t *re, int want_substrs, char ***substrings) {
	int i, ret, rc;
	regmatch_t pmatch[40];
	int pmatch_len = sizeof (pmatch) / sizeof (pmatch[0]);

	rc = regexec(re, str, pmatch_len, pmatch, 0);
	if (rc == REG_NOMATCH) {
		/* no match */
		return 0;
	} else if (!want_substrs) {
		/* just report successful match */
		return 1;
	}

	char **substrs;

	ret = 0;
	substrs = malloc(pmatch_len * sizeof (char *));
	for (i = 0; i < pmatch_len; ++i) {
		int len;
		char *buf;

		if (pmatch[i].rm_so == -1)
			continue;
		len = pmatch[i].rm_eo - pmatch[i].rm_so;
		buf = malloc(len + 1);
		memcpy(buf, str + pmatch[i].rm_so, len);
		buf[len] = '\0';
		substrs[ret] = buf;
		++ret;
	}

	/*
	  if (ret < pmatch_len)
	  substrs = realloc(substrs, ret * sizeof (char *));
	*/
	*substrings = substrs;

	return ret;
}
#endif

static SEXP_t *create_item(const char *path, const char *filename, char *pattern,
			   int instance, char **substrs, int substr_cnt)
{
	int i;
	SEXP_t *attrs, *item;

	attrs = probe_attr_creat ("id", SEXP_OVALitem_newid(&global.id_desc),
                                  "status", SEXP_number_newd(OVAL_STATUS_EXISTS),
                                  NULL);

	item = probe_item_creat ("textfilecontent_item", attrs,
                                 /* entities */
                                 "path", NULL,
                                 SEXP_string_newf(path),
                                 "filename", NULL,
                                 SEXP_string_newf(filename),
                                 "pattern", NULL,
                                 SEXP_string_newf(pattern),
                                 "instance", NULL,
                                 SEXP_number_newd(instance),
                                 "text", NULL,
                                 SEXP_string_newf(substrs[0]),
                                 NULL);

	probe_itement_setstatus(item, "path",     1, OVAL_STATUS_EXISTS);
	probe_itement_setstatus(item, "filename", 1, OVAL_STATUS_EXISTS);

	for (i = 1; i < substr_cnt; ++i) {
                probe_item_ent_add (item, "subexpression", NULL, SEXP_string_newf(substrs[i]));
                // FIXME: SEXP_OVALobj_setelmstatus(item, "subexpression", i, OVAL_STATUS_EXISTS);
	}

	return item;
}

static int report_missing(SEXP_t *elm)
{
	oval_operation_t op;

	op = SEXP_number_geti_32 (probe_ent_getattrval(elm, "operation"));

        if (op == OVAL_OPERATION_EQUALS)
		return 1;
	else
		return 0;
}

struct pfdata {
	char *pattern;
	SEXP_t *filename_elm;
	SEXP_t *instance_elm;
	SEXP_t *item_list;
};

static int process_file(const char *path, const char *filename, void *arg)
{
	struct pfdata *pfd = (struct pfdata *) arg;
	int ret = 0, path_len, filename_len;
	char *whole_path = NULL;
	FILE *fp = NULL;

// todo: move to probe_main()?
#if defined USE_REGEX_PCRE
	int erroffset = -1;
	pcre *re = NULL;
	const char *error;

	re = pcre_compile(pfd->pattern, PCRE_UTF8, &error, &erroffset, NULL);
	if (re == NULL) {
		return -1;
	}
#elif defined USE_REGEX_POSIX
	regex_t _re, *re = &_re;

	if (regcomp(re, pfd->pattern, REG_EXTENDED | REG_NEWLINE) != 0) {
		return -1;
	}
#endif

	if (filename == NULL) {
		SEXP_t *attrs, *item;

		if (report_missing(pfd->filename_elm)) {
			attrs = probe_attr_creat ("id", SEXP_number_newd(-1), // todo: id
						     "status", SEXP_number_newd(OVAL_STATUS_DOESNOTEXIST),
						     NULL);
			item = probe_item_creat ("textfilecontent_item",
						   attrs,
						   "path", NULL,
						   SEXP_string_newf(path),
						   "filename", NULL,
						   SEXP_string_newf(filename),
						   NULL);

			probe_itement_setstatus (item, "path",     1, OVAL_STATUS_EXISTS);
		        probe_itement_setstatus (item, "filename", 1, OVAL_STATUS_DOESNOTEXIST);
		} else {
			attrs = probe_attr_creat ("id", SEXP_number_newd(-1), // todo: id
                                                  "status", SEXP_number_newd(OVAL_STATUS_EXISTS),
                                                  NULL);

			item = probe_item_creat ("textfilecontent_item", attrs,
                                                 /* entities */
                                                 "path", NULL,
                                                 SEXP_string_newf(path),
                                                 NULL);

			probe_itement_setstatus(item, "path", 1, OVAL_STATUS_EXISTS);
		}

		SEXP_list_add(pfd->item_list, item);

		goto cleanup;
	}

	path_len = strlen(path);
	filename_len = strlen(filename);
	whole_path = malloc(path_len + filename_len + 2);
	memcpy(whole_path, path, path_len);
	if (whole_path[path_len - 1] != FILE_SEPARATOR) {
		whole_path[path_len] = FILE_SEPARATOR;
		++path_len;
	}
	memcpy(whole_path + path_len, filename, filename_len + 1);

	fp = fopen(whole_path, "rb");
	if (fp == NULL) {
		ret = -2;
		goto cleanup;
	}

	int cur_inst = 0;
	char line[4096];
	SEXP_t *next_inst = NULL;

	while (fgets(line, sizeof(line), fp) != NULL) {
		char **substrs;
		int substr_cnt, want_instance;

		if (next_inst != NULL)
			SEXP_free(next_inst);
		next_inst = SEXP_number_newd(cur_inst + 1);
		if (SEXP_OVALentobj_cmp(pfd->instance_elm, next_inst) == OVAL_RESULT_TRUE)
			want_instance = 1;
		else
			want_instance = 0;

		substr_cnt = get_substrings(line, re, want_instance, &substrs);
		if (substr_cnt > 0) {
			++cur_inst;

			if (want_instance) {
				int k;

				SEXP_list_add(pfd->item_list,
					      create_item(path, filename, pfd->pattern,
							  cur_inst, substrs, substr_cnt));
				for (k = 0; k < substr_cnt; ++k)
					free(substrs[k]);
				free(substrs);
			}
		}
	}

 cleanup:
	if (fp != NULL)
		fclose(fp);
	if (whole_path != NULL)
		free(whole_path);
#if defined USE_REGEX_PCRE
	if (re != NULL)
		pcre_free(re);
#elif defined USE_REGEX_POSIX
	regfree(re);
#endif

	return ret;
}

SEXP_t *probe_main(SEXP_t *probe_in, int *err)
{
	SEXP_t *path_elm, *filename_elm, *instance_elm, *behaviors_elm;
	char *pattern;

	if (probe_in == NULL) {
		*err = PROBE_EINVAL;
		return NULL;
	}

	/* parse request */
	if ( (path_elm = SEXP_OVALobj_getelm(probe_in, "path", 1)) == NULL ||
	     (filename_elm = SEXP_OVALobj_getelm(probe_in, "filename", 1)) == NULL ||
	     (pattern = SEXP_string_cstr(SEXP_OVALobj_getelmval(probe_in, "pattern", 1, 1))) == NULL ||
	     (instance_elm = SEXP_OVALobj_getelm(probe_in, "instance", 1)) == NULL) {
		*err = PROBE_ENOELM;
		return NULL;
	}

	/* canonicalize behaviors */
	behaviors_elm = SEXP_OVALobj_getelm(probe_in, "behaviors", 1);
	if (behaviors_elm == NULL) {
		SEXP_t * behaviors_new;
		behaviors_new = SEXP_OVALelm_create("behaviors",
						    SEXP_OVALattr_create ("max_depth", SEXP_string_newf ("1"),
									  "recurse_direction", SEXP_string_newf ("none"),
									  NULL),
						    NULL /* val */,
						    NULL /* end */);
		behaviors_elm = SEXP_list_first(behaviors_new);
	}
	else {
		if (!SEXP_OVALelm_hasattr (behaviors_elm, "max_depth"))
			SEXP_OVALelm_attr_add (behaviors_elm,"max_depth", SEXP_string_newf ("1"));
		if (!SEXP_OVALelm_hasattr (behaviors_elm, "recurse_direction"))
			SEXP_OVALelm_attr_add (behaviors_elm,"recurse_direction", SEXP_string_newf ("none"));
	}

	int fcnt;
	struct pfdata pfd;

	pfd.pattern = pattern;
	pfd.filename_elm = filename_elm;
	pfd.instance_elm = instance_elm;
	pfd.item_list = SEXP_list_new();

	fcnt = find_files(path_elm, filename_elm, behaviors_elm,
			  process_file, (void *) &pfd);
	if (fcnt == 0) {
		if (report_missing(pfd.filename_elm)) {
			SEXP_t *item, *attrs;
			attrs = SEXP_OVALattr_create("id", SEXP_number_newd(-1), // todo: id
						     "status", SEXP_number_newd(OVAL_STATUS_DOESNOTEXIST),
						     NULL);
			item = SEXP_OVALobj_create("textfilecontent_item",
						   attrs,
						   "path", NULL,
						   SEXP_OVALelm_getval(path_elm, 1),
						   NULL);
			SEXP_OVALobj_setelmstatus(item, "path", 1, OVAL_STATUS_DOESNOTEXIST);
			SEXP_list_add(pfd.item_list, item);
		}
	} else if (fcnt < 0) {
		SEXP_t *item, *attrs;
		attrs = SEXP_OVALattr_create("id", SEXP_number_newd(-1), // todo: id
					     "status", SEXP_number_newd(OVAL_STATUS_ERROR),
					     NULL);
		item = SEXP_OVALobj_create("textfilecontent_item",
					   attrs,
					   "path", NULL,
					   SEXP_OVALelm_getval(path_elm, 1),
					   NULL);
		SEXP_list_add(pfd.item_list, item);
	}

	*err = 0;
	return pfd.item_list;
}
#endif
