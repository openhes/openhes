////////////////////////////////////////////////////////////////////////////////
// Copyright 2026 Tom G. Huang <tomghuang@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Process-level helpers the CLI uses before any gateway module starts.
///
/// @details
/// The process's paths and flags live in app_info_t, filled in once by
/// get_app_info() and handed to every subcommand as its argtable context. The
/// rest are small utilities (directory check, random bytes, whole-file read)
/// that depend on libuv and libc only -- nothing here knows about HES.

#ifndef OPENHES_SRC_UTILS_H
#define OPENHES_SRC_UTILS_H

#include "app_config.h"

#include <stdbool.h>
#include <stddef.h>

////////////////////////////////////////////////////////////////////////////////
/// Every path and runtime flag a subcommand starts with, resolved once at
/// startup by get_app_info(). The struct is the argtable context each
/// cmd_*_proc() receives.
typedef struct app_info {
    char exe_path[MAX_PATH_SIZE];        ///< Path to the executable
    char home[MAX_PATH_SIZE];            ///< Path to the user's home directory
    char cwd[MAX_PATH_SIZE];             ///< Path to the current working directory
    char tmp_dir[MAX_PATH_SIZE];         ///< Path to the temporary directory
    char data_home[MAX_PATH_SIZE];       ///< Path to the user's data directory
    char state_home[MAX_PATH_SIZE];      ///< Path to the user's state directory
    char cache_home[MAX_PATH_SIZE];      ///< Path to the user's cache directory
    char config_dir[MAX_PATH_SIZE];      ///< Path to the configuration directory
    char config_path[MAX_PATH_SIZE];     ///< Path to the configuration file
    char profile_path[MAX_PATH_SIZE];    ///< Path to the product profile file
    char default_rep_url[MAX_URL_SIZE];  ///< Default request/reply URL
    char default_pub_url[MAX_URL_SIZE];  ///< Default publish/subscribe URL
    int log_level;                       ///< Log level for the application
    int verbosity;                       ///< Verbosity level for the application
    bool quiet;                          ///< Flag indicating if run in quiet mode
} app_info_t;

////////////////////////////////////////////////////////////////////////////////
/// Gets application information such as paths and directories.
///
/// @param info Pointer to an app_info_t structure to be filled with application information.
/// @return 0 on success, -1 on failure.
int get_app_info(app_info_t* info);

////////////////////////////////////////////////////////////////////////////////
/// Checks if a directory exists.
///
/// @param dir Pointer to the directory path to check.
/// @return 1 if the directory exists, 0 otherwise.
int is_dir_exist(const char* dir);

////////////////////////////////////////////////////////////////////////////////
/// Generates a random string.
///
/// @param buf Pointer to the buffer to store the random string.
/// @param buf_size Size of the buffer.
/// @return 0 on success, -1 on failure.
int gen_random(char* buf, size_t buf_size);

////////////////////////////////////////////////////////////////////////////////
/// Reads a whole file into a NUL-terminated heap buffer.
///
/// @param path Path to the file to read.
/// @return Allocated file contents on success, otherwise NULL. The caller
///         owns the returned buffer and must free it.
char* read_file(const char* path);

#endif  // #ifndef OPENHES_SRC_UTILS_H
