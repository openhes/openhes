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
/// @brief Small process-level helpers: app paths, directory checks, random
/// bytes and whole-file reads.
///
/// @details
/// All of it sits on libuv (uv_exepath/uv_os_*), so the same code serves Linux,
/// macOS and Windows. Nothing here knows about HES or the gateway: the CLI
/// layer calls it before any module starts.

#include "utils.h"

#include <cwalk.h>
#include <log.h>
#include <uv.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

////////////////////////////////////////////////////////////////////////////////
/// Resolves the cache directory: $XDG_CACHE_HOME/ohmg, else ~/.cache/ohmg, else
/// ./ohmg (LOCALAPPDATA instead of XDG on Windows).
///
/// @param buf Destination buffer.
/// @param buf_size Capacity of buf.
/// @return 0 on success, -1 when the path does not fit.
static int get_cache_dir(char* buf, size_t buf_size)
{
#ifdef _WIN32
    char localappdata[MAX_PATH_SIZE] = {0};
    size_t len = sizeof(localappdata);
    if (uv_os_getenv("LOCALAPPDATA", localappdata, &len) == 0) {
        int n = snprintf(buf, buf_size, "%s\\ohmg", localappdata);
        return (n < 0 || (size_t)n >= buf_size) ? -1 : 0;
    }
    // fallback: use home directory
#else
    char xdg_cache[MAX_PATH_SIZE] = {0};
    size_t len = sizeof(xdg_cache);
    if (uv_os_getenv("XDG_CACHE_HOME", xdg_cache, &len) == 0) {
        int n = snprintf(buf, buf_size, "%s/ohmg", xdg_cache);
        return (n < 0 || (size_t)n >= buf_size) ? -1 : 0;
    }

    char homedir[MAX_PATH_SIZE] = {0};
    len = sizeof(homedir);
    if (uv_os_homedir(homedir, &len) == 0) {
        int n = snprintf(buf, buf_size, "%s/.cache/ohmg", homedir);
        return (n < 0 || (size_t)n >= buf_size) ? -1 : 0;
    }
#endif

    // fallback: just use current directory
    int n = snprintf(buf, buf_size, "./ohmg");
    return (n < 0 || (size_t)n >= buf_size) ? -1 : 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Resolves the configuration directory: $XDG_CONFIG_HOME/ohmg, else
/// ~/.config/ohmg, else ./ohmg (LOCALAPPDATA instead of XDG on Windows).
///
/// @param buf Destination buffer.
/// @param buf_size Capacity of buf.
/// @return 0 on success, -1 when the path does not fit.
static int get_config_dir(char* buf, size_t buf_size)
{
#ifdef _WIN32
    char appdata[MAX_PATH_SIZE] = {0};
    size_t len = sizeof(appdata);
    if (uv_os_getenv("LOCALAPPDATA", appdata, &len) == 0) {
        int n = snprintf(buf, buf_size, "%s\\ohmg", appdata);
        return (n < 0 || (size_t)n >= buf_size) ? -1 : 0;
    }
    // fallback: use home directory
#else
    char xdg_config[MAX_PATH_SIZE] = {0};
    size_t len = sizeof(xdg_config);
    if (uv_os_getenv("XDG_CONFIG_HOME", xdg_config, &len) == 0) {
        int n = snprintf(buf, buf_size, "%s/ohmg", xdg_config);
        return (n < 0 || (size_t)n >= buf_size) ? -1 : 0;
    }

    char homedir[MAX_PATH_SIZE] = {0};
    len = sizeof(homedir);
    if (uv_os_homedir(homedir, &len) == 0) {
        int n = snprintf(buf, buf_size, "%s/.config/ohmg", homedir);
        return (n < 0 || (size_t)n >= buf_size) ? -1 : 0;
    }
#endif

    // fallback: just use current directory
    int n = snprintf(buf, buf_size, "./ohmg");
    return (n < 0 || (size_t)n >= buf_size) ? -1 : 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Resolves the configuration file path: "config.toml" inside the config
/// directory above.
///
/// @param buf Destination buffer.
/// @param buf_size Capacity of buf.
/// @return 0 on success, -1 when the path does not fit.
static int get_config_path(char* buf, size_t buf_size)
{
    char dir[MAX_PATH_SIZE] = {0};
    if (get_config_dir(dir, sizeof(dir)) < 0) {
        return -1;
    }

    size_t n = cwk_path_join(dir, "config.toml", buf, buf_size);
    return (n >= buf_size) ? -1 : 0;
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int gen_random(char* buf, size_t buf_size)
{
    return uv_random(NULL, NULL, buf, buf_size, 0, NULL);
}

char* read_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) {
        fclose(f);
        return NULL;
    }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

int is_dir_exist(const char* dir)
{
    uv_loop_t* loop = uv_default_loop();
    uv_fs_t open_req = {0};
    int rv = uv_fs_opendir(loop, &open_req, dir, NULL);
    if (rv == 0) {
        uv_fs_closedir(loop, &open_req, open_req.ptr, NULL);
        uv_fs_req_cleanup(&open_req);
        return 1;
    }

    uv_fs_req_cleanup(&open_req);
    return 0;
}

int get_app_info(app_info_t* info)
{
    size_t exe_path_size = sizeof(info->exe_path);
    int rv = uv_exepath(info->exe_path, &exe_path_size);
    if (rv != 0) {
        log_error("fail to get exe path: %s", uv_strerror(rv));
        return -1;
    }

    size_t home_dir_size = sizeof(info->home);
    rv = uv_os_homedir(info->home, &home_dir_size);
    if (rv != 0) {
        log_error("fail to get home dir: %s", uv_strerror(rv));
        return -1;
    }

    size_t cwd_dir_size = sizeof(info->cwd);
    rv = uv_cwd(info->cwd, &cwd_dir_size);
    if (rv != 0) {
        log_error("fail to get current working dir: %s", uv_strerror(rv));
        return -1;
    }

    size_t tmp_dir_size = sizeof(info->tmp_dir);
    rv = uv_os_tmpdir(info->tmp_dir, &tmp_dir_size);
    if (rv != 0) {
        log_error("fail to get temp dir: %s", uv_strerror(rv));
        return -1;
    }

    cwk_path_join(info->home, ".ohmg", info->data_home, sizeof(info->data_home));
    cwk_path_join(info->data_home, "state", info->state_home, sizeof(info->state_home));

    rv = get_config_dir(info->config_dir, sizeof(info->config_dir));
    rv |= get_config_path(info->config_path, sizeof(info->config_path));
    rv |= get_cache_dir(info->cache_home, sizeof(info->cache_home));
    if (rv != 0) {
        log_error("fail to get cache or config dir");
        return -1;
    }

    cwk_path_join(info->config_dir, "profile.json", info->profile_path, sizeof(info->profile_path));

    snprintf(info->default_rep_url, sizeof(info->default_rep_url), "%s", HES_MANIFEST_REP_URL);
    snprintf(info->default_pub_url, sizeof(info->default_pub_url), "%s", HES_MANIFEST_PUB_URL);
    return 0;
}
