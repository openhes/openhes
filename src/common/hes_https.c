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
/// @brief Implementation of the blocking HTTPS GET (hes_https.h): one libcurl
/// easy handle per call, with a body sink that refuses to truncate.
///
/// @details
/// Three pieces:
///
///   - body_sink_t / sink_write(): libcurl delivers the entity in as many chunks
///     as the network produced. A chunk that does not fit the caller's buffer
///     aborts the transfer (returning short makes libcurl fail with
///     CURLE_WRITE_ERROR) instead of silently truncating -- an entity the caller
///     cannot hold is an error, not a partial success.
///
///   - curl_init_once(): curl_global_init() must run once before any thread
///     touches libcurl. That is enforced here with pthread_once rather than
///     assumed of the caller, because the caller is a background sync thread.
///
///   - hes_https_get(): sets the options the exchange needs (URL, https-only,
///     certificate verification against the distribution's CA bundle
///     (HES_HTTPS_CA_BUNDLE), timeout, the sink) and requires a 2xx status
///     before reporting success.

#include "hes_https.h"

#include "app_config.h"

#include <log.h>

#include <curl/curl.h>

#include <pthread.h>
#include <string.h>

#define HTTPS_USER_AGENT "openhes-ohmg/1.0"

#define HTTPS_DEFAULT_TIMEOUT_MS 8000

////////////////////////////////////////////////////////////////////////////////
/// Response sink: libcurl hands the entity over in as many chunks as the
/// network produced, so the body is accumulated here. A chunk that does not fit
/// fails the transfer instead of being dropped -- an entity the caller cannot
/// hold is an error, never a silent truncation.
typedef struct body_sink {
    char* body;
    size_t cap;  ///< Buffer capacity, NUL included.
    size_t len;
    int overflow;
} body_sink_t;

////////////////////////////////////////////////////////////////////////////////
/// libcurl write callback: appends one received chunk to the body sink. A chunk
/// that does not fit sets the overflow flag and reports a short write, which
/// libcurl turns into a failed transfer -- an entity the caller's buffer cannot
/// hold is an error, never a silent truncation.
///
/// @param data     The chunk libcurl received.
/// @param size     Size of one element (always 1 for curl).
/// @param nmemb    How many elements.
/// @param userdata The body_sink_t for this transfer.
/// @return How many bytes were taken; fewer than size * nmemb aborts the
///         transfer.
static size_t sink_write(char* data, size_t size, size_t nmemb, void* userdata)
{
    body_sink_t* sink = (body_sink_t*)userdata;
    size_t n = size * nmemb;

    if (n > sink->cap - 1 - sink->len) {
        sink->overflow = 1;
        return 0;  // returning short aborts the transfer (CURLE_WRITE_ERROR)
    }

    memcpy(sink->body + sink->len, data, n);
    sink->len += n;
    sink->body[sink->len] = '\0';
    return n;
}

/// curl_global_init() must run once before any thread touches libcurl. This
/// client is called from the time service's sync thread, so the "once" part is
/// enforced here rather than assumed of the caller.
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;

////////////////////////////////////////////////////////////////////////////////
/// Runs curl_global_init() exactly once, for whichever thread gets there first
/// (the function pthread_once() calls).
static void curl_init_once(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int hes_https_get(const char* url, char* body, size_t body_cap, size_t* body_len, int timeout_ms)
{
    if (body_len != NULL) {
        *body_len = 0;
    }
    if (url == NULL || body == NULL || body_cap < 2) {
        return -1;
    }
    body[0] = '\0';

    if (timeout_ms <= 0) {
        timeout_ms = HTTPS_DEFAULT_TIMEOUT_MS;
    }

    pthread_once(&curl_once, curl_init_once);

    CURL* handle = curl_easy_init();
    if (handle == NULL) {
        log_error("https: cannot create a curl handle for %s", url);
        return -1;
    }

    char detail[CURL_ERROR_SIZE];
    detail[0] = '\0';

    body_sink_t sink;
    sink.body = body;
    sink.cap = body_cap;
    sink.len = 0;
    sink.overflow = 0;

    struct curl_slist* headers = curl_slist_append(NULL, "Accept: application/json");

    // https only, before and after any redirect: this client exists to make the
    // reply trustworthy, so a downgrade has to fail rather than succeed.
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");

    // Certificate verification is libcurl's default; it is restated because it
    // is the whole point of the exercise. There is no switch to turn it off.
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);

    // ...and the trust store is named rather than inherited from the build.
    // libcurl uses whatever CA bundle it was configured with at configure time,
    // which is host-relative autodetection (deps/curl/CMakeLists.txt): a tree
    // configured where the probe found nothing -- the arm64 cross build -- carries
    // no CA store at all, and then every chain is checked against an EMPTY trust
    // store and rejected as "not correctly signed by the trusted CA". Naming the
    // file also keeps the failure honest: a missing bundle is reported as an
    // unreadable CA file, not as a server whose certificate is bad.
    curl_easy_setopt(handle, CURLOPT_CAINFO, HES_HTTPS_CA_BUNDLE);

    // One budget for the whole exchange: DNS, connect, TLS, request, reply.
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    // A timed transfer in a threaded program must not rely on signals.
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(handle, CURLOPT_USERAGENT, HTTPS_USER_AGENT);
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, sink_write);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, detail);

    CURLcode rv = curl_easy_perform(handle);

    long status = 0;
    if (rv == CURLE_OK) {
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);

    if (sink.overflow) {
        log_error("https: response from %s exceeds %d bytes", url, (int)body_cap);
        body[0] = '\0';
        return -1;
    }
    if (rv != CURLE_OK) {
        log_error("https: %s failed: %s%s%s", url, curl_easy_strerror(rv),
                  detail[0] != '\0' ? " - " : "", detail);
        body[0] = '\0';
        return -1;
    }
    if (status < 200 || status > 299) {
        log_error("https: %s answered with HTTP status %ld", url, status);
        body[0] = '\0';
        return -1;
    }

    if (body_len != NULL) {
        *body_len = sink.len;
    }
    return 0;
}
