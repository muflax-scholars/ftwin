#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include <pcre.h>

#include <apr_file_info.h>
#include <apr_file_io.h>
#include <apr_getopt.h>
#include <napr_hash.h>
#include <apr_mmap.h>
#include <apr_strings.h>

#include "config.h"
#include "checksum.h"
#include "debug.h"
#include "napr_heap.h"

#define is_option_set(mask, option)  ((mask & option) == option)

#define set_option(mask, option, on)     \
    do {                                 \
        if (on)                          \
            *mask |= option;             \
        else                             \
            *mask &= ~option;            \
    } while (0)


#define OPTION_ICASE 0x01
#define OPTION_FSYML 0x02
#define OPTION_RECSD 0x04
#define OPTION_VERBO 0x08
#define OPTION_OPMEM 0x10
#define OPTION_REGEX 0x20
#define OPTION_SIZED 0x40

typedef struct ft_file_t
{
    apr_off_t size;
    char *path;
} ft_file_t;

typedef struct ft_chksum_t
{
    apr_uint32_t val_array[HASHSTATE];	/* 256 bits (using Bob Jenkins http://www.burtleburtle.net/bob/c/checksum.c) */
    ft_file_t *file;
} ft_chksum_t;

typedef struct ft_fsize_t
{
    apr_off_t val;
    ft_chksum_t *chksum_array;
    apr_uint32_t nb_files;
    apr_uint32_t nb_checksumed;
} ft_fsize_t;

typedef struct ft_conf_t
{
    apr_off_t minsize;
    apr_pool_t *pool;		/* Always needed somewhere ;) */
    napr_heap_t *heap;		/* Will holds the files */
    napr_hash_t *sizes;		/* will holds the sizes hashed with http://www.burtleburtle.net/bob/hash/integer.html */
    napr_hash_t *ig_files;
    pcre *ig_regex;
    char *p_path;		/* priority path */
    unsigned char mask;
    char sep;
} ft_conf_t;

static int ft_file_cmp(const void *param1, const void *param2)
{
    const ft_file_t *file1 = param1;
    const ft_file_t *file2 = param2;

    if (file1->size < file2->size)
	return -1;
    else if (file2->size < file1->size)
	return 1;

    return 0;
}

static const void *ft_fsize_get_key(const void *opaque)
{
    const ft_fsize_t *fsize = opaque;

    return &(fsize->val);
}

static apr_size_t ft_fsize_get_key_len(const void *opaque)
{
    return 1;
}

static int ft_fsize_key_cmp(const void *key1, const void *key2, apr_size_t len)
{
    apr_uint32_t i1 = *(apr_uint32_t *) key1;
    apr_uint32_t i2 = *(apr_uint32_t *) key2;

    return (i1 == i2) ? 0 : 1;
}

/* http://www.burtleburtle.net/bob/hash/integer.html */
static apr_uint32_t ft_fsize_key_hash(const void *key, apr_size_t klen)
{
    apr_uint32_t i = *(apr_uint32_t *) key;

    i = (i + 0x7ed55d16) + (i << 12);
    i = (i ^ 0xc761c23c) ^ (i >> 19);
    i = (i + 0x165667b1) + (i << 5);
    i = (i + 0xd3a2646c) ^ (i << 9);
    i = (i + 0xfd7046c5) + (i << 3);
    i = (i ^ 0xb55a4f09) ^ (i >> 16);

    return i;
}

static void ft_hash_add_ignore_list(napr_hash_t *hash, const char *file_list)
{
    const char *filename, *end;
    apr_uint32_t hash_value;
    apr_pool_t *pool;
    char *tmp;

    pool = napr_hash_pool_get(hash);
    filename = file_list;
    do {
	end = strchr(filename, 'c');
	if (NULL != end) {
	    tmp = apr_pstrndup(pool, filename, end - filename);
	}
	else {
	    tmp = apr_pstrdup(pool, filename);
	}
	napr_hash_search(hash, tmp, strlen(tmp), &hash_value);
	napr_hash_set(hash, tmp, hash_value);

	filename = end + 1;
    } while ((NULL != end) && ('\0' != *filename));
}

/** 
 * The function used to add recursively or not file and dirs.
 * @param conf Configuration structure.
 * @param filename name of a file or directory to add to the list of twinchecker.
 * @param gc_pool garbage collecting pool, will be cleaned by the caller.
 * @return APR_SUCCESS if no error occured.
 */
#define MATCH_VECTOR_SIZE 210
static apr_status_t ft_conf_add_file(ft_conf_t *conf, const char *filename, apr_pool_t *gc_pool)
{
    int ovector[MATCH_VECTOR_SIZE];
    char errbuf[128];
    apr_finfo_t finfo;
    apr_dir_t *dir;
    apr_int32_t statmask = APR_FINFO_SIZE | APR_FINFO_TYPE;
    apr_size_t fname_len;
    apr_status_t status;
    int rc;

    /* Step 1 : Check if it's a directory and get the size if not */
    if (!is_option_set(conf->mask, OPTION_FSYML))
	statmask |= APR_FINFO_LINK;

    if (APR_SUCCESS != (status = apr_stat(&finfo, filename, statmask, gc_pool))) {
	DEBUG_ERR("error calling apr_stat: %s", apr_strerror(status, errbuf, 128));
	return status;
    }

    /* Step 2: If it is, browse it */
    if (APR_DIR == finfo.filetype) {
	if (APR_SUCCESS != (status = apr_dir_open(&dir, filename, gc_pool))) {
	    DEBUG_ERR("error calling apr_dir_open: %s", apr_strerror(status, errbuf, 128));
	    return status;
	}
	fname_len = strlen(filename);
	while ((APR_SUCCESS == (status = apr_dir_read(&finfo, APR_FINFO_NAME | APR_FINFO_TYPE, dir)))
	       && (NULL != finfo.name)) {
	    /* Check if it has to be ignored */
	    if (NULL != napr_hash_search(conf->ig_files, finfo.name, strlen(finfo.name), NULL))
		continue;

	    if ((NULL != conf->ig_regex)
		&& (0 <=
		    (rc =
		     pcre_exec(conf->ig_regex, NULL, finfo.name, strlen(finfo.name), 0, 0, ovector, MATCH_VECTOR_SIZE))))
		continue;

	    if (APR_DIR == finfo.filetype && !is_option_set(conf->mask, OPTION_RECSD))
		continue;

	    status =
		ft_conf_add_file(conf,
				 apr_pstrcat(gc_pool, filename, ('/' == filename[fname_len - 1]) ? "" : "/", finfo.name,
					     NULL), gc_pool);

	    if (APR_SUCCESS != status) {
		DEBUG_ERR("error recursively calling ft_conf_add_file: %s", apr_strerror(status, errbuf, 128));
		return status;
	    }
	}
	if ((APR_SUCCESS != status) && (APR_ENOENT != status)) {
	    DEBUG_ERR("error calling apr_dir_read: %s", apr_strerror(status, errbuf, 128));
	    return status;
	}

	if (APR_SUCCESS != (status = apr_dir_close(dir))) {
	    DEBUG_ERR("error calling apr_dir_close: %s", apr_strerror(status, errbuf, 128));
	    return status;
	}
    }
    else if (APR_REG == finfo.filetype || ((APR_LNK == finfo.filetype) && (is_option_set(conf->mask, OPTION_FSYML)))) {
	if (finfo.size >= conf->minsize) {
	    ft_file_t *file;
	    ft_fsize_t *fsize;
	    apr_uint32_t hash_value;

	    file = apr_palloc(conf->pool, sizeof(struct ft_file_t));
	    file->path = apr_pstrdup(conf->pool, filename);
	    file->size = finfo.size;

	    napr_heap_insert(conf->heap, file);

	    if (NULL == (fsize = napr_hash_search(conf->sizes, &finfo.size, 1, &hash_value))) {
		fsize = apr_palloc(conf->pool, sizeof(struct ft_fsize_t));
		fsize->val = finfo.size;
		fsize->chksum_array = NULL;
		fsize->nb_checksumed = 0;
		fsize->nb_files = 0;
		napr_hash_set(conf->sizes, fsize, hash_value);
	    }
	    fsize->nb_files++;
	}
    }

    return APR_SUCCESS;
}

static apr_status_t checksum_file(const char *filename, apr_off_t size, apr_uint32_t *state, apr_pool_t *gc_pool)
{
    char errbuf[128];
    apr_file_t *fd = NULL;
    apr_mmap_t *mm;
    apr_status_t status;
    apr_uint32_t i;

    status = apr_file_open(&fd, filename, APR_READ | APR_BINARY, APR_OS_DEFAULT, gc_pool);
    if (APR_SUCCESS != status) {
	DEBUG_ERR("unable to open(%s, O_RDONLY), skipping: %s", filename, apr_strerror(status, errbuf, 128));
	return status;
    }

    status = apr_mmap_create(&mm, fd, 0, (apr_size_t) size, APR_MMAP_READ, gc_pool);
    if (APR_SUCCESS != status) {
	DEBUG_ERR("unable to mmap %s, skipping: %s", filename, apr_strerror(status, errbuf, 128));
	return status;
    }

    for (i = 0; i < HASHSTATE; ++i)
	state[i] = 1;

    hash(mm->mm, mm->size, state);

    /*
       printf("filename: %s ", filename);
       for (i = 0; i < HASHSTATE; ++i)
       printf("%.8x", state[i]);
       printf("\n");
     */

    if (APR_SUCCESS != (status = apr_mmap_delete(mm))) {
	DEBUG_ERR("error calling apr_mmap_delete: %s", apr_strerror(status, errbuf, 128));
	return status;
    }
    if (APR_SUCCESS != (status = apr_file_close(fd))) {
	DEBUG_ERR("error calling apr_file_close: %s", apr_strerror(status, errbuf, 128));
	return status;
    }

    return APR_SUCCESS;
}

static apr_status_t ft_conf_process_sizes(ft_conf_t *conf)
{
    char errbuf[128];
    ft_file_t *file;
    ft_fsize_t *fsize;
    napr_heap_t *tmp_heap;
    apr_pool_t *gc_pool;
    apr_uint32_t hash_value;
    apr_status_t status;
    apr_size_t nb_processed, nb_files;

    if (is_option_set(conf->mask, OPTION_VERBO))
	fprintf(stderr, "Referencing files and sizes:\n");

    if (APR_SUCCESS != (status = apr_pool_create(&gc_pool, conf->pool))) {
	DEBUG_ERR("error calling apr_pool_create: %s", apr_strerror(status, errbuf, 128));
	apr_terminate();
	return -1;
    }
    nb_processed = 0;
    nb_files = napr_heap_size(conf->heap);

    tmp_heap = napr_heap_make(conf->pool, ft_file_cmp);
    while (NULL != (file = napr_heap_extract(conf->heap))) {
	if (NULL != (fsize = napr_hash_search(conf->sizes, &file->size, 1, &hash_value))) {
	    /* More than two files, we will need to checksum because :
	     * - 1 file of a size means no twin.
	     * - 2 files of a size means that anyway we must read the both, so
	     *   we will probably cmp them at that time instead of running CPU
	     *   intensive function like checksum.
	     */
	    if (1 == fsize->nb_files) {
		/* No twin possible, remove the entry */
		/*DEBUG_DBG("only one file of size %"APR_OFF_T_FMT, fsize->val); */
		napr_hash_remove(conf->sizes, fsize, hash_value);
	    }
	    else {
		if (NULL == fsize->chksum_array)
		    fsize->chksum_array = apr_palloc(conf->pool, fsize->nb_files * sizeof(struct ft_chksum_t));

		fsize->chksum_array[fsize->nb_checksumed].file = file;
		/* no multiple check, just a memcmp will be needed, don't call checksum on 0-length file too */
		if ((2 == fsize->nb_files) || (0 == fsize->val)) {
		    /*DEBUG_DBG("two files of size %"APR_OFF_T_FMT, fsize->val); */
		    memset(fsize->chksum_array[fsize->nb_checksumed].val_array, 0, HASHSTATE * sizeof(apr_int32_t));
		}
		else if (APR_SUCCESS !=
			 (status =
			  checksum_file(file->path, file->size, fsize->chksum_array[fsize->nb_checksumed].val_array,
					gc_pool))) {
		    DEBUG_ERR("error calling checksum_file: %s", apr_strerror(status, errbuf, 128));
		    apr_pool_destroy(gc_pool);
		    return status;
		}
		fsize->nb_checksumed++;
		napr_heap_insert(tmp_heap, file);
	    }
	    if (is_option_set(conf->mask, OPTION_VERBO)) {
		fprintf(stderr, "\rProgress [%" APR_SIZE_T_FMT "/%" APR_SIZE_T_FMT "] %d%% ", nb_processed, nb_files,
			(int) ((float) nb_processed / (float) nb_files * 100.0));
		nb_processed++;
	    }
	}
	else {
	    DEBUG_ERR("inconsistency error found, no size[%" APR_OFF_T_FMT "] in hash for file %s", file->size, file->path);
	    apr_pool_destroy(gc_pool);
	    return APR_EGENERAL;
	}
    }
    if (is_option_set(conf->mask, OPTION_VERBO)) {
	fprintf(stderr, "\rProgress [%" APR_SIZE_T_FMT "/%" APR_SIZE_T_FMT "] %d%% ", nb_processed, nb_files,
		(int) ((float) nb_processed / (float) nb_files * 100.0));
	fprintf(stderr, "\n");
    }

    apr_pool_destroy(gc_pool);
    conf->heap = tmp_heap;

    return APR_SUCCESS;
}

static int chksum_cmp(const void *chksum1, const void *chksum2)
{
    const ft_chksum_t *chk1 = chksum1;
    const ft_chksum_t *chk2 = chksum2;
    int i;

    if (0 == (i = memcmp(chk1->val_array, chk2->val_array, HASHSTATE))) {
	/* XXX ici mettre le prio path */
	return strcmp(chk1->file->path, chk2->file->path);
    }

    return i;
}

static apr_status_t filecmp(apr_pool_t *pool, const char *fname1, const char *fname2, apr_off_t size, int *i)
{
    char errbuf[128];
    apr_file_t *fd1 = NULL, *fd2 = NULL;
    apr_mmap_t *mm1, *mm2;
    apr_status_t status;

    if (0 == size) {
	*i = 0;
	return APR_SUCCESS;
    }

    status = apr_file_open(&fd1, fname1, APR_READ | APR_BINARY, APR_OS_DEFAULT, pool);
    if (APR_SUCCESS != status) {
	DEBUG_ERR("unable to open(%s, O_RDONLY), skipping: %s", fname1, apr_strerror(status, errbuf, 128));
	return status;
    }

    status = apr_mmap_create(&mm1, fd1, 0, (apr_size_t) size, APR_MMAP_READ, pool);
    if (APR_SUCCESS != status) {
	DEBUG_ERR("unable to mmap %s, skipping: %s", fname1, apr_strerror(status, errbuf, 128));
	return status;
    }

    status = apr_file_open(&fd2, fname2, APR_READ | APR_BINARY, APR_OS_DEFAULT, pool);
    if (APR_SUCCESS != status) {
	DEBUG_ERR("unable to open(%s, O_RDONLY), skipping: %s", fname2, apr_strerror(status, errbuf, 128));
	return status;
    }

    status = apr_mmap_create(&mm2, fd2, 0, size, APR_MMAP_READ, pool);
    if (APR_SUCCESS != status) {
	DEBUG_ERR("unable to mmap %s, skipping: %s", fname2, apr_strerror(status, errbuf, 128));
	return status;
    }

    *i = memcmp(mm1->mm, mm2->mm, size);

    if (APR_SUCCESS != (status = apr_mmap_delete(mm2))) {
	DEBUG_ERR("error calling apr_mmap_delete: %s", apr_strerror(status, errbuf, 128));
	return status;
    }
    if (APR_SUCCESS != (status = apr_file_close(fd2))) {
	DEBUG_ERR("error calling apr_file_close: %s", apr_strerror(status, errbuf, 128));
	return status;
    }

    if (APR_SUCCESS != (status = apr_mmap_delete(mm1))) {
	DEBUG_ERR("error calling apr_mmap_delete: %s", apr_strerror(status, errbuf, 128));
	return status;
    }
    if (APR_SUCCESS != (status = apr_file_close(fd1))) {
	DEBUG_ERR("error calling apr_file_close: %s", apr_strerror(status, errbuf, 128));
	return status;
    }

    return APR_SUCCESS;
}

static apr_status_t ft_conf_twin_report(ft_conf_t *conf)
{
    char errbuf[128];
    apr_off_t old_size = -1;
    ft_file_t *file;
    ft_fsize_t *fsize;
    apr_uint32_t hash_value;
    apr_size_t i, j;
    int rv;
    apr_status_t status;
    unsigned char already_printed;

    if (is_option_set(conf->mask, OPTION_VERBO))
	fprintf(stderr, "Reporting duplicate files:\n");

    while (NULL != (file = napr_heap_extract(conf->heap))) {
	if (file->size == old_size)
	    continue;

	old_size = file->size;
	if (NULL != (fsize = napr_hash_search(conf->sizes, &file->size, 1, &hash_value))) {
	    qsort(fsize->chksum_array, fsize->nb_files, sizeof(ft_chksum_t), chksum_cmp);
	    for (i = 0; i < fsize->nb_files; i++) {
		if (NULL == fsize->chksum_array[i].file)
		    continue;
		already_printed = 0;
		for (j = i + 1; j < fsize->nb_files; j++) {
		    if (0 == memcmp(fsize->chksum_array[i].val_array, fsize->chksum_array[j].val_array, HASHSTATE)) {
			if (APR_SUCCESS !=
			    (status =
			     filecmp(conf->pool, fsize->chksum_array[i].file->path, fsize->chksum_array[j].file->path,
				     fsize->val, &rv))) {
			    DEBUG_ERR("error calling filecmp: %s", apr_strerror(status, errbuf, 128));
			    return status;
			}
			if (0 == rv) {
			    if (!already_printed) {
				if (is_option_set(conf->mask, OPTION_SIZED))
				    printf("size [%" APR_OFF_T_FMT "]:\n", fsize->val);
				printf("%s%c", fsize->chksum_array[i].file->path, conf->sep);
				already_printed = 1;
			    }
			    else {
				printf("%c", conf->sep);
			    }
			    printf("%s", fsize->chksum_array[j].file->path);
			    /* mark j as a twin ! */
			    fsize->chksum_array[j].file = NULL;
			    fflush(stdout);
			}
		    }
		    else {
			/* hash are ordered, so at first mismatch we check the next */
			break;
		    }
		}
		if (already_printed)
		    printf("\n\n");
	    }
	}
	else {
	    DEBUG_ERR("inconsistency error found, no size[%" APR_OFF_T_FMT "] in hash for file %s", file->size, file->path);
	    return APR_EGENERAL;
	}
    }

    return APR_SUCCESS;
}

static void version()
{
    fprintf(stdout, PACKAGE_STRING "\n");
    fprintf(stdout, "Copyright (C) 2007 François Pesce\n");
    fprintf(stdout, "Licensed under the Apache License, Version 2.0 (the \"License\");\n");
    fprintf(stdout, "you may not use this file except in compliance with the License.\n");
    fprintf(stdout, "You may obtain a copy of the License at\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "\thttp://www.apache.org/licenses/LICENSE-2.0\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "Unless required by applicable law or agreed to in writing, software\n");
    fprintf(stdout, "distributed under the License is distributed on an \"AS IS\" BASIS,\n");
    fprintf(stdout, "WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n");
    fprintf(stdout, "See the License for the specific language governing permissions and\n");
    fprintf(stdout, "limitations under the License.\n\n");
    fprintf(stdout, "Report bugs to " PACKAGE_BUGREPORT "\n");
}

static void usage(const char *name, const apr_getopt_option_t *opt_option)
{
    int i;

    fprintf(stdout, PACKAGE_STRING "\n");
    fprintf(stdout, "Usage: %s [OPTION]... [FILES or DIRECTORIES]...\n", name);
    fprintf(stdout, "Find identical files passed as parameter or recursively found in directories.\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stdout, "\n");

    for (i = 0; NULL != opt_option[i].name; i++) {
	fprintf(stdout, "-%c,\t--%s\t%s\n", opt_option[i].optch, opt_option[i].name, opt_option[i].description);
    }
}

int main(int argc, const char **argv)
{
    static const apr_getopt_option_t opt_option[] = {
	{"case-unsensitive", 'c', FALSE, "this option applies to regex match."},
	{"display-size", 'd', FALSE, "display size before duplicates."},
	{"regex-ignore-file", 'e', TRUE, "filenames that match this are ignored."},
	{"follow-symlink", 'f', FALSE, "follow symbolic links."},
	{"help", 'h', FALSE, "\t\tdisplay usage."},
	{"ignore-list", 'i', TRUE, "\tcomma-separated list of file names to ignore."},
	{"minimal-length", 'm', TRUE, "minimum size of file to process."},
	{"optimize-memory", 'o', FALSE, "reduce memory usage, but increase process time."},
	{"priority-path", 'p', TRUE, "\tfile in this path are displayed first when\n\t\t\t\tduplicates are reported."},
	{"recurse-subdir", 'r', FALSE, "recurse subdirectories."},
	{"separator", 's', TRUE, "\tseparator character between twins, default: \\n."},
	{"verbose", 'v', FALSE, "\tdisplay a progress bar."},
	{"version", 'V', FALSE, "\tdisplay version."},
	{NULL, 0, 0, NULL},	/* end (a.k.a. sentinel) */
    };
    char errbuf[128];
    char *regex = NULL;
    ft_conf_t conf;
    apr_getopt_t *os;
    apr_pool_t *pool, *gc_pool;
    apr_uint32_t hash_value;
    const char *optarg;
    int optch, i;
    apr_status_t status;

    if (APR_SUCCESS != (status = apr_initialize())) {
	DEBUG_ERR("error calling apr_initialize: %s", apr_strerror(status, errbuf, 128));
	return -1;
    }

    if (APR_SUCCESS != (status = apr_pool_create(&pool, NULL))) {
	DEBUG_ERR("error calling apr_pool_create: %s", apr_strerror(status, errbuf, 128));
	apr_terminate();
	return -1;
    }

    if (APR_SUCCESS != (status = apr_getopt_init(&os, pool, argc, argv))) {
	DEBUG_ERR("error calling apr_getopt_init: %s", apr_strerror(status, errbuf, 128));
	apr_terminate();
	return -1;
    }

    conf.pool = pool;
    conf.heap = napr_heap_make(pool, ft_file_cmp);
    conf.ig_files = napr_hash_str_make(pool, 32, 8);
    conf.sizes = napr_hash_make(pool, 4096, 8, ft_fsize_get_key, ft_fsize_get_key_len, ft_fsize_key_cmp, ft_fsize_key_hash);
    /* To avoid endless loop, ignore looping directory ;) */
    napr_hash_search(conf.ig_files, ".", 1, &hash_value);
    napr_hash_set(conf.ig_files, ".", hash_value);
    napr_hash_search(conf.ig_files, "..", 2, &hash_value);
    napr_hash_set(conf.ig_files, "..", hash_value);
    conf.ig_regex = NULL;
    conf.p_path = NULL;
    conf.minsize = 0;
    conf.sep = '\n';
    conf.mask = 0x00;

    while (APR_SUCCESS == (status = apr_getopt_long(os, opt_option, &optch, &optarg))) {
	switch (optch) {
	case 'c':
	    set_option(&conf.mask, OPTION_ICASE, 1);
	    break;
	case 'd':
	    set_option(&conf.mask, OPTION_SIZED, 1);
	    break;
	case 'e':
	    regex = apr_pstrdup(pool, optarg);
	    break;
	case 'f':
	    set_option(&conf.mask, OPTION_FSYML, 1);
	    break;
	case 'h':
	    usage(argv[0], opt_option);
	    return 0;
	case 'i':
	    ft_hash_add_ignore_list(conf.ig_files, optarg);
	    break;
	case 'm':
	    conf.minsize = strtoul(optarg, NULL, 10);
	    if (ULONG_MAX == conf.minsize) {
		DEBUG_ERR("can't parse %s for -m / --minimal-length", optarg);
		apr_terminate();
		return -1;
	    }
	    break;
	case 'o':
	    set_option(&conf.mask, OPTION_OPMEM, 1);
	    break;
	case 'p':
	    conf.p_path = apr_pstrdup(pool, optarg);
	    break;
	case 'r':
	    set_option(&conf.mask, OPTION_RECSD, 1);
	    break;
	case 's':
	    conf.sep = *optarg;
	    break;
	case 'v':
	    set_option(&conf.mask, OPTION_VERBO, 1);
	    break;
	case 'V':
	    version();
	    return 0;
	}
    }

    if (NULL != regex) {
	const char *errptr;
	int erroffset, options = PCRE_DOLLAR_ENDONLY | PCRE_DOTALL;

	if (is_option_set(conf.mask, OPTION_ICASE))
	    options |= PCRE_CASELESS;

	conf.ig_regex = pcre_compile(regex, options, &errptr, &erroffset, NULL);
	if (NULL == conf.ig_regex) {
	    DEBUG_ERR("can't parse %s at [%.*s] for -e / --regex-ignore-file: %s", regex, erroffset, regex, errptr);
	    apr_terminate();
	    return -1;
	}
    }

    /* Step 1 : Browse the file */
    if (APR_SUCCESS != (status = apr_pool_create(&gc_pool, pool))) {
	DEBUG_ERR("error calling apr_pool_create: %s", apr_strerror(status, errbuf, 128));
	apr_terminate();
	return -1;
    }
    for (i = os->ind; i < argc; i++) {
	if (APR_SUCCESS != (status = ft_conf_add_file(&conf, argv[i], gc_pool))) {
	    DEBUG_ERR("error calling ft_conf_add_file: %s", apr_strerror(status, errbuf, 128));
	    apr_terminate();
	    return -1;
	}
    }
    apr_pool_destroy(gc_pool);

    if (0 < napr_heap_size(conf.heap)) {
	/* Step 2: Process the sizes set */
	if (APR_SUCCESS != (status = ft_conf_process_sizes(&conf))) {
	    DEBUG_ERR("error calling ft_conf_process_sizes: %s", apr_strerror(status, errbuf, 128));
	    apr_terminate();
	    return -1;
	}

	/* Step 3: Report the twins */
	if (APR_SUCCESS != (status = ft_conf_twin_report(&conf))) {
	    DEBUG_ERR("error calling ft_conf_twin_report: %s", apr_strerror(status, errbuf, 128));
	    apr_terminate();
	    return status;
	}
    }
    else {
	DEBUG_ERR("Please submit at least two files...");
	usage(argv[0], opt_option);
	return -1;
    }

    apr_terminate();

    return 0;
}
