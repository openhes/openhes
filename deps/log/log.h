/**
 * Copyright (c) 2020 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `log.c` for details.
 */

#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#define LOG_VERSION "0.1.0"
#define LOG_MAX_FILE_NAME_SIZE 256

typedef struct log_event {
    va_list ap;
    const char *fmt;
    const char *file;
    struct tm *time;
    void *udata;
    int line;
    int level;
} log_event_t;

typedef void (*log_log_cb)(log_event_t *ev);
typedef void (*log_lock_cb)(bool lock, void *udata);

typedef enum log_level {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO = 2,
    LOG_WARN = 3,
    LOG_ERROR = 4,
    LOG_FATAL = 5
} log_level_t;

typedef enum log_verbosity {
    OUT_QUIET = 0,        // -q, --quiet
    OUT_NORMAL = 1,       //
    OUT_VERBOSE = 2,      // -v
    OUT_ERROR = 3         // cannot silent errors
} log_verbosity_t;

#define log_trace(...) log_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...) log_log(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...) log_log(LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

#define out_data(...) log_data(__VA_ARGS__)
#define out_info(...) log_output(OUT_NORMAL, __VA_ARGS__)
#define out_verbose(...) log_output(OUT_VERBOSE, __VA_ARGS__)
#define out_error(...) log_output(OUT_ERROR, __VA_ARGS__)

const char *log_level_string(int level);
void log_set_lock(log_lock_cb fn, void *udata);
void log_set_default_lock(void);
void log_set_level(int level);
void log_set_verbosity(int verbosity);
int log_get_verbosity(void);
void log_set_quiet(bool enable);
int log_add_callback(log_log_cb fn, void *udata, int level);
int log_add_fp(FILE *fp, int level);
int log_add_rotate_file(const char *path, int level, size_t size, size_t n);
void log_clear_callbacks(void);

void log_data(const char *fmt, ...);
void log_output(int min_verbosity, const char *fmt, ...);
void log_log(int level, const char *file, int line, const char *fmt, ...);

#endif
