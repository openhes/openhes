/*
 * Copyright (c) 2020 rxi
 * Copyright (c) 2021 yksz
 * Copyright (c) 2022 Tom G. Huang
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "log.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#define _AMD64_
#include <winsock2.h>
#else
#include <pthread.h>
#endif /* defined(_WIN32) || defined(_WIN64) */

#define MAX_CALLBACKS 32

typedef struct callback {
    log_log_cb fn;
    void* udata;
    int level;
} callback_t;

static struct {
    void* udata;
    log_lock_cb lock;
    int level;
    int verbosity;
    bool quiet;
    callback_t callbacks[MAX_CALLBACKS];
    int callback_count;

#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif /* defined(_WIN32) || defined(_WIN64) */

    char filename[LOG_MAX_FILE_NAME_SIZE];
    size_t current_file_size;
    size_t max_file_size;
    size_t max_file_count;
} L;

static const char* level_strings[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};

#ifdef LOG_USE_COLOR
static const char* level_colors[] = {"\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"};
#endif

static void stream_callback(log_event_t* ev) {
    char buf[16];
    buf[strftime(buf, sizeof(buf), "%H:%M:%S", ev->time)] = '\0';
#ifdef LOG_USE_COLOR
    fprintf(ev->udata, "%s %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ", buf, level_colors[ev->level], level_strings[ev->level], ev->file, ev->line);
#else
    fprintf(ev->udata, "%s %-5s %s:%d: ", buf, level_strings[ev->level], ev->file, ev->line);
#endif
    vfprintf(ev->udata, ev->fmt, ev->ap);
    fprintf(ev->udata, "\n");
    fflush(ev->udata);
}

static void file_callback(log_event_t* ev) {
    char buf[64];
    buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';
    fprintf(ev->udata, "%s %-5s %s:%d: ", buf, level_strings[ev->level], ev->file, ev->line);
    vfprintf(ev->udata, ev->fmt, ev->ap);
    fprintf(ev->udata, "\n");
    fflush(ev->udata);
}

static long file_size(FILE* fp) {
    long current = ftell(fp);

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);

    fseek(fp, current, SEEK_SET);
    return size;
}

static void backup_file_name(const char* basename, unsigned char index, char* buf, size_t buf_size) {
    assert(buf_size >= strlen(basename) + 5);  // one dot, three digits, and one NULL

    if (index > 0) {
        snprintf(buf, buf_size, "%s.%d", basename, index);
    } else {
        snprintf(buf, buf_size, "%s", basename);
    }
}

static int is_file_exist(const char* filename) {
    FILE* fp = fopen(filename, "rb");
    if (fp == NULL) {
        return 0;
    } else {
        fclose(fp);
        return 1;
    }
}

static int rotate_files(log_event_t* ev) {
    printf("rotate...\n");

    char src[LOG_MAX_FILE_NAME_SIZE + 4] = {0};
    char dst[LOG_MAX_FILE_NAME_SIZE + 4] = {0};
    fclose(ev->udata);

    for (int i = (int)L.max_file_count; i > 0; i--) {
        backup_file_name(L.filename, i - 1, src, sizeof(src));
        backup_file_name(L.filename, i, dst, sizeof(dst));
        printf("src: %s\n", src);
        printf("dst: %s\n", dst);
        if (is_file_exist(src)) {
            /* Remove destination first (rename() on Windows fails if
             * the destination file exists). */
            if (is_file_exist(dst)) {
                if (remove(dst) != 0) {
                    fprintf(stderr, "ERROR: logger: Failed to remove file: `%s`\n", dst);
                }
            }
            if (rename(src, dst) != 0) {
                fprintf(stderr, "ERROR: logger: Failed to rename file: `%s` -> `%s`\n", src, dst);
            }
        }
    }

    ev->udata = fopen(L.filename, "ab");
    if (ev->udata == NULL) {
        fprintf(stderr, "ERROR: logger: Failed to open file: `%s`\n", ev->file);
        return 1;
    }

    long size = file_size(ev->udata);
    if (size == -1) {
        fprintf(stderr, "ERROR: logger: Failed to get file size: `%s`\n", ev->file);
        return 1;
    }

    L.current_file_size = (size_t)size;
    return 0;
}


static void rotate_file_callback(log_event_t* ev) {
    char buf[64] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time);

    int len = snprintf(NULL, 0, "%s %-5s %s:%d: ", buf, level_strings[ev->level], ev->file, ev->line);
    len += vsnprintf(NULL, 0, ev->fmt, ev->ap);
    len += snprintf(NULL, 0, "\n");
    if (L.current_file_size + len > L.max_file_size) {
        if (rotate_files(ev) != 0) {
            return;
        }
    }

    size_t total_size = 0;
    int size = fprintf(ev->udata, "%s %-5s %s:%d: ", buf, level_strings[ev->level], ev->file, ev->line);
    if (size == -1) {
        printf("fail: %s\n", strerror(errno));
        exit(1);
    }

    if (size > 0) {
        total_size += size;
    }

    size = vfprintf(ev->udata, ev->fmt, ev->ap);
    if (size > 0) {
        total_size += size;
    }

    size = fprintf(ev->udata, "\n");
    if (size > 0) {
        total_size += size;
    }

    fflush(ev->udata);
    L.current_file_size += total_size;
}

static void lock(void) {
    if (L.lock) {
        L.lock(true, L.udata);
    }
}

static void unlock(void) {
    if (L.lock) {
        L.lock(false, L.udata);
    }
}

const char* log_level_string(int level) {
    return level_strings[level];
}

void log_set_lock(log_lock_cb fn, void* udata) {
    L.lock = fn;
    L.udata = udata;
}

static void lock_cb(bool lock, void* udata) {
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION* mutex = udata;
    if (lock) {
        EnterCriticalSection(mutex);
    } else {
        LeaveCriticalSection(mutex);
    }
#else
    pthread_mutex_t* mutex = udata;
    if (lock) {
        pthread_mutex_lock(mutex);
    } else {
        pthread_mutex_unlock(mutex);
    }
#endif /* defined(_WIN32) || defined(_WIN64) */
}

void log_set_default_lock(void) {
#if defined(_WIN32) || defined(_WIN64)
    InitializeCriticalSection(&L.mutex);
#else
    pthread_mutex_init(&L.mutex, NULL);
#endif /* defined(_WIN32) || defined(_WIN64) */

    log_set_lock(lock_cb, &L.mutex);
}

void log_set_level(int level) {
    L.level = level;
}

void log_set_verbosity(int verbosity) {
    L.verbosity = verbosity < OUT_QUIET ? OUT_QUIET : verbosity;
}

int log_get_verbosity(void) {
    return L.verbosity;
}

void log_set_quiet(bool enable) {
    L.quiet = enable;
}

void log_data(const char* fmt, ...) {
    lock();

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);

    fflush(stdout);
    unlock();
}

void log_output(int verbosity, const char* fmt, ...) {
    FILE* stream = stdout;

    if (verbosity <= OUT_QUIET) {
        verbosity = OUT_QUIET;
    }

    if (verbosity == OUT_ERROR) {
        stream = stderr;
    } else if (L.verbosity < verbosity) {
        return;
    }

    lock();

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stream, fmt, ap);
    va_end(ap);

    fprintf(stream, "\n");
    fflush(stream);

    unlock();
}

int log_add_callback(log_log_cb fn, void* udata, int level) {
    if (L.callback_count >= MAX_CALLBACKS) {
        return -1;
    }
    L.callbacks[L.callback_count] = (callback_t){fn, udata, level};
    L.callback_count++;
    return 0;
}

void log_clear_callbacks(void) {
    lock();
    for (int i = 0; i < L.callback_count; i++) {
        L.callbacks[i].fn = NULL;
        L.callbacks[i].udata = NULL;
        L.callbacks[i].level = 0;
    }
    L.callback_count = 0;
    unlock();
}

int log_add_fp(FILE* fp, int level) {
    return log_add_callback(file_callback, fp, level);
}

int log_add_rotate_file(const char* path, int level,size_t size, size_t n) {
    snprintf(L.filename, LOG_MAX_FILE_NAME_SIZE, "%s", path);
    L.max_file_size = size;
    L.max_file_count = n;

    FILE* fp = fopen(path, "ab");
    if (fp == NULL) {
        return -1;
    }
    L.current_file_size = file_size(fp);

    return log_add_callback(rotate_file_callback, fp, level);
}

static void init_event(log_event_t* ev, void* udata) {
    if (!ev->time) {
        time_t t = time(NULL);
        ev->time = localtime(&t);
    }
    ev->udata = udata;
}

void log_log(int level, const char* file, int line, const char* fmt, ...) {
    log_event_t ev = {
            .fmt = fmt,
            .file = file,
            .line = line,
            .level = level,
    };

    lock();

    if (!L.quiet && level >= L.level) {
        init_event(&ev, stderr);
        va_start(ev.ap, fmt);
        stream_callback(&ev);
        va_end(ev.ap);
    }

    for (int i = 0; i < L.callback_count; i++) {
        callback_t* cb = &L.callbacks[i];
        if (level >= cb->level) {
            init_event(&ev, cb->udata);
            va_start(ev.ap, fmt);
            cb->fn(&ev);
            va_end(ev.ap);
        }
    }

    unlock();
}

// Add default lock/unlock, so it can be thread-safe in most platforms.
// Add rotate file callback.