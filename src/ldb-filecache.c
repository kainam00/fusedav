/***
  This file is part of fusedav.

  fusedav is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  fusedav is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU General Public License
  along with fusedav; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "ldb-filecache.h"
#include "statcache.h"
#include "fusedav.h"
#include "log.h"

#include <ne_uri.h>

#define REFRESH_INTERVAL 3

typedef int fd_t;

// Session data
struct ldb_filecache_sdata {
    fd_t fd;
    char filename[PATH_MAX]; // Only used for new replacement files.
    bool readable;
    bool writable;
    bool modified;
};

// FIX ME Where to find ETAG_MAX?
#define ETAG_MAX 256

// Persistent data stored in leveldb
struct ldb_filecache_pdata {
    char filename[PATH_MAX];
    char etag[ETAG_MAX + 1];
    time_t last_server_update;
};

static char *path2key(const char *path);
static int ldb_filecache_close(struct ldb_filecache_sdata *sdata);
static struct ldb_filecache_pdata *ldb_filecache_pdata_get(ldb_filecache_t *cache, const char *path);
static int ldb_filecache_pdata_set(ldb_filecache_t *cache, const char *path, const struct ldb_filecache_pdata *pdata);

static int new_cache_file(const char *cache_path, char *cache_file_path, fd_t *fd) {
    snprintf(cache_file_path, PATH_MAX, "%s/files/fusedav-cache-XXXXXX", cache_path);
    log_print(LOG_DEBUG, "Using pattern %s", cache_file_path);
    if ((*fd = mkstemp(cache_file_path)) < 0) {
        log_print(LOG_ERR, "new_cache_file: Failed mkstemp");
        return -1;
    }

    log_print(LOG_DEBUG, "new_cache_file: mkstemp fd=%d :: %s", *fd, cache_file_path);
    return 0;
}

// Get a file descriptor pointing to the latest full copy of the file.
static fd_t ldb_get_fresh_fd(ne_session *session, ldb_filecache_t *cache, const char *cache_path, const char *path) {
    struct ldb_filecache_pdata *pdata;
    bool cached_file_is_fresh = false;
    fd_t ret_fd = -1;
    int code;
    ne_request *req = NULL;
    int ne_ret;
    
    pdata = ldb_filecache_pdata_get(cache, path);

    if (pdata != NULL)
        log_print(LOG_DEBUG, "ldb_get_fresh_fd: file found in cache: %s::%s", path, pdata->filename);

    // Is it usable as-is?
    if (pdata != NULL && (time(NULL) - pdata->last_server_update) <= REFRESH_INTERVAL) {
        cached_file_is_fresh = true;
    }

    if (cached_file_is_fresh) {
        ret_fd = open(pdata->filename, O_APPEND);
        goto finish;
    }

    req = ne_request_create(session, "GET", path);
    if (!req) {
        log_print(LOG_ERR, "ldb_get_fresh_fd: Failed ne_request_create on GET");
        goto finish;
    }

    // If we have stale cache data, set a header to aim for a 304.
    if (pdata)
        ne_add_request_header(req, "If-None-Match", pdata->etag);

    do {
        ne_ret = ne_begin_request(req);
        if (ne_ret != NE_OK) {
            goto finish;
        }

        code = ne_get_status(req)->code;
        if (code == 304) {
            log_print(LOG_DEBUG, "Got 304 on %s", path);
            
            // Gobble up any remaining data in the response.
            ne_discard_response(req);

            // Mark the cache item as revalidated at the current time.
            pdata->last_server_update = time(NULL);
            ldb_filecache_pdata_set(cache, path, pdata);

            // @TODO: Set proper flags? Or enforce in fusedav.c?
            ret_fd = open(pdata->filename, O_APPEND);
            goto finish;
        }
        else if (code == 200) {
            // Archive the old temp file path for unlinking after replacement.
            char old_filename[PATH_MAX];
            bool unlink_old = false;
            const char *etag = NULL;

            if (pdata == NULL) {
                pdata = malloc(sizeof(struct ldb_filecache_pdata));
                memset(pdata, 0, sizeof(struct ldb_filecache_pdata));

                // Fill in ETag.
                etag = ne_get_response_header(req, "ETag");
                log_print(LOG_DEBUG, "Got ETag: %s", etag);
                strncpy(pdata->etag, etag, ETAG_MAX);
            }
            else {
                strncpy(old_filename, pdata->filename, PATH_MAX);
                unlink_old = true;
            }

            // Create a new temp file and read the file content into it.
            // @TODO: Set proper flags? Or enforce in fusedav.c?
            new_cache_file(cache_path, pdata->filename, &ret_fd);
            ne_read_response_to_fd(req, ret_fd);

            // Point the persistent cache to the new file content.
            pdata->last_server_update = time(NULL);
            ldb_filecache_pdata_set(cache, path, pdata);

            // Unlink the old cache file, which the persistent cache
            // no longer references. This will cause the file to be
            // deleted once no more file descriptors reference it.
            if (unlink_old)
                unlink(old_filename);
            goto finish;
        }

        ne_ret = ne_end_request(req);
    } while (ne_ret == NE_RETRY);

    finish:
        if (req != NULL)
            ne_request_destroy(req);
        if (pdata != NULL)
            free(pdata);
        return ret_fd;
}

int ldb_filecache_open(char *cache_path, ldb_filecache_t *cache, const char *path, struct fuse_file_info *info, bool replace) {
    ne_session *session;
    struct ldb_filecache_sdata *sdata;
    int ret = -1;
    int flags = info->flags;
    struct stat_cache_value value;

    if (!(session = session_get(1))) {
        ret = -EIO;
        log_print(LOG_ERR, "ldb_filecache_open: Failed to get session");
        goto fail;
    }

    // Allocate and zero-out a session data structure.
    sdata = malloc(sizeof(struct ldb_filecache_sdata));
    if (sdata == NULL) {
        log_print(LOG_ERR, "ldb_filecache_open: Failed to malloc sdata");
        goto fail;
    }
    memset(sdata, 0, sizeof(struct ldb_filecache_sdata));

    if (replace) {
        // Create a new file to write into.
        sdata->modified = true;
        if (new_cache_file(cache_path, sdata->filename, &sdata->fd) < 0) {
            goto fail;
        }

        value.st.st_mode = 0660 | S_IFREG;
        value.st.st_nlink = 1;
        value.st.st_size = 0;
        value.st.st_atime = time(NULL);
        value.st.st_mtime = value.st.st_atime;
        value.st.st_ctime = value.st.st_mtime;
        value.st.st_blksize = 0;
        value.st.st_blocks = 8;
        value.st.st_uid = getuid();
        value.st.st_gid = getgid();
        value.prepopulated = false;
        stat_cache_value_set(cache, path, &value);
        log_print(LOG_DEBUG, "Updated stat cache.");
    }
    else {
        // Get a file descriptor pointing to a guaranteed-fresh file.
        sdata->fd = ldb_get_fresh_fd(session, cache, cache_path, path);
    }

    if (flags & O_RDONLY || flags & O_RDWR) sdata->readable = 1;
    if (flags & O_WRONLY || flags & O_RDWR) sdata->writable = 1;

    if (sdata->fd > 0) {
        log_print(LOG_DEBUG, "Setting fh to session data structure with fd %d.", sdata->fd);
        info->fh = (uint64_t) sdata;
        ret = 0;
        goto finish;
    }

fail:
    log_print(LOG_ERR, "No valid fd set for path %s. Setting fh structure to NULL.", path);
    info->fh = (uint64_t) NULL;

    if (sdata != NULL)
        free(sdata);

finish:
    return ret;
}

ssize_t ldb_filecache_read(struct fuse_file_info *info, char *buf, size_t size, ne_off_t offset) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    ssize_t ret = -1;

    log_print(LOG_DEBUG, "ldb_filecache_read");

    // ensure data is present and fresh
    // ETAG exchange
    //

    if ((ret = pread(sdata->fd, buf, size, offset)) < 0) {
        ret = -errno;
        log_print(LOG_ERR, "ldb_filecache_read: error %d; %d %s %d %ld", ret, sdata->fd, buf, size, offset);
        goto finish;
    }

finish:

    // ret is bytes read, or error
    log_print(LOG_DEBUG, "Done reading.");

    return ret;
}

static struct ldb_filecache_pdata *ldb_filecache_pdata_get(ldb_filecache_t *cache, const char *path) {
    struct ldb_filecache_pdata *pdata = NULL;
    char *key;
    leveldb_readoptions_t *options;
    size_t vallen;
    char *errptr = NULL;

    log_print(LOG_DEBUG, "Entered ldb_filecache_pdata_get: path=%s", path);

    key = path2key(path);

    options = leveldb_readoptions_create();
    pdata = (struct ldb_filecache_pdata *) leveldb_get(cache, options, key, strlen(key) + 1, &vallen, &errptr);
    leveldb_readoptions_destroy(options);
    free(key);

    if (errptr != NULL) {
        log_print(LOG_ERR, "leveldb_get error: %s", errptr);
        free(errptr);
        return NULL;
    }

    if (!pdata) {
        log_print(LOG_DEBUG, "ldb_filecache_pdata_get miss on path: %s", path);
        return NULL;
    }

    if (vallen != sizeof(struct ldb_filecache_pdata)) {
        log_print(LOG_ERR, "Length %lu is not expected length %lu.", vallen, sizeof(struct ldb_filecache_pdata));
    }

    return pdata;
}


ssize_t ldb_filecache_write(struct fuse_file_info *info, const char *buf, size_t size, ne_off_t offset) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    ssize_t ret = -1;

    if (!sdata->writable) {
        errno = EBADF;
        ret = 0;
        if (debug) {
            log_print(LOG_DEBUG, "ldb_filecache_write: not writable");
        }
        goto finish;
    }

    if ((ret = pwrite(sdata->fd, buf, size, offset)) < 0) {
        ret = -errno;
        log_print(LOG_ERR, "ldb_filecache_write: error %d %d %s::%d %d %ld", ret, errno, strerror(errno), sdata->fd, size, offset);
        goto finish;
    }

    sdata->modified = true;

finish:

    // ret is bytes written

    return ret;
}

static int ldb_filecache_pdata_set(ldb_filecache_t *cache, const char *path, const struct ldb_filecache_pdata *pdata) {
    leveldb_writeoptions_t *options;
    char *errptr = NULL;
    char *key;
    int ret = -1;

    if (!pdata) {
        if (debug) {
            log_print(LOG_ERR, "ldb_filecache_pdata_set NULL pdata");
        }
        goto finish;
    }

    key = path2key(path);

    options = leveldb_writeoptions_create();
    leveldb_put(cache, options, key, strlen(key) + 1, (const char *) pdata, sizeof(struct ldb_filecache_pdata), &errptr);
    leveldb_writeoptions_destroy(options);

    free(key);

    if (errptr != NULL) {
        log_print(LOG_ERR, "leveldb_set error: %s", errptr);
        free(errptr);
        goto finish;
    }

    ret = 0;

finish:

    return ret;
}

int ldb_filecache_truncate(struct fuse_file_info *info, ne_off_t s) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    int ret = -1;

    if ((ret = ftruncate(sdata->fd, s)) < 0) {
        log_print(LOG_ERR, "ldb_filecache_truncate: error on ftruncate %d", ret);
    }

    return ret;
}

int ldb_filecache_release(ldb_filecache_t *cache, const char *path, struct fuse_file_info *info) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    int ret = -1;

    assert(sdata);

    log_print(LOG_DEBUG, "release(%s)", path);

    if ((ret = ldb_filecache_sync(cache, path, info)) < 0) {
        log_print(LOG_ERR, "ldb_filecache_unref: ldb_filecache_sync returns error %d", ret);
        goto finish;
    }

    log_print(LOG_DEBUG, "Done syncing file for release.");

    ldb_filecache_close(sdata);

    ret = 0;

finish:

    log_print(LOG_DEBUG, "Done releasing file.");

    return ret;
}

int ldb_filecache_sync(ldb_filecache_t *cache, const char *path, struct fuse_file_info *info) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    int ret = -1;
    //struct ldb_filecache_pdata *pdata = NULL;
    ne_session *session;
    struct stat_cache_value value;

    assert(sdata);

    log_print(LOG_DEBUG, "ldb_filecache_sync(%s)", path);

    log_print(LOG_DEBUG, "Checking if file was writable.");
    if (!sdata->writable) {
        // errno = EBADF; why?
        ret = 0;
        if (debug) {
            log_print(LOG_DEBUG, "ldb_filecache_sync: not writable");
        }
        goto finish;
    }

    log_print(LOG_DEBUG, "Checking if file was modified.");
    if (!sdata->modified) {
        ret = 0;
        if (debug) {
            log_print(LOG_DEBUG, "ldb_filecache_sync: not modified");
        }
        goto finish;
    }

    log_print(LOG_DEBUG, "Seeking.");
    if (lseek(sdata->fd, 0, SEEK_SET) == (ne_off_t)-1) {
        log_print(LOG_ERR, "ldb_filecache_sync: failed lseek");
        ret = -1;
        goto finish;
    }

    log_print(LOG_DEBUG, "Getting libneon session.");
    if (!(session = session_get(1))) {
        errno = EIO;
        ret = -1;
        log_print(LOG_ERR, "ldb_filecache_sync: failed session");
        goto finish;
    }

    //pdata = ldb_filecache_pdata_get(cache, path);

    // JB FIXME replace ne_put with our own version which also returns the
    // ETAG information.
    //pdata->last_server_update = time(NULL);
    // FIXME! Generate ETAG. Or rewrite ne_put to put file, and get etag back
    //generate_etag(pdata->etag, sdata->fd);

    // @TODO: Replace PUT with something that gets the ETag returned by Valhalla.
    // Write this data to the persistent cache.

    log_print(LOG_DEBUG, "About to PUT file.");

    if (ne_put(session, path, sdata->fd)) {
        log_print(LOG_ERR, "PUT failed: %s", ne_get_error(session));
        errno = ENOENT;
        ret = -1;
        goto finish;
    }

    // Update stat cache.
    // @TODO: Use actual mode.
    value.st.st_mode = 0660 | S_IFREG;
    value.st.st_nlink = 1;
    value.st.st_size = lseek(sdata->fd, 0, SEEK_END);
    value.st.st_atime = time(NULL);
    value.st.st_mtime = value.st.st_atime;
    value.st.st_ctime = value.st.st_mtime;
    value.st.st_blksize = 0;
    value.st.st_blocks = 8;
    value.st.st_uid = getuid();
    value.st.st_gid = getgid();
    value.prepopulated = false;
    stat_cache_value_set(cache, path, &value);
    log_print(LOG_DEBUG, "Updated stat cache.");

    ret = 0;

finish:

    log_print(LOG_DEBUG, "Done syncing file.");

    return ret;
}

int ldb_filecache_delete(ldb_filecache_t *cache, const char *path) {
    leveldb_writeoptions_t *options;
    char *key;
    int ret = 0;
    char *errptr = NULL;

    key = path2key(path);
    options = leveldb_writeoptions_create();
    leveldb_delete(cache, options, key, strlen(key) + 1, &errptr);
    leveldb_writeoptions_destroy(options);
    free(key);

    if (errptr != NULL) {
        log_print(LOG_ERR, "leveldb_delete error: %s", errptr);
        free(errptr);
        ret = -1;
    }

    return ret;
}

// Allocates a new string.
static char *path2key(const char *path) {
    char *key = NULL;
    asprintf(&key, "fc:%s", path);
    return key;
}

static int ldb_filecache_close(struct ldb_filecache_sdata *sdata) {

    if (sdata->fd >= 0)
        close(sdata->fd);

    return 0;
}

int ldb_filecache_init(char *cache_path) {
    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "%s/files", cache_path);
    if (mkdir(path, 0770) == -1) {
        if (errno != EEXIST) {
            log_print(LOG_ERR, "Path %s could not be created.", path);
            return -1;
        }
    }
    return 0;
}