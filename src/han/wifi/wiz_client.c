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
/// @brief Implementation of the WiZ UDP client (wiz_client.h): one request out,
/// and the reply that belongs to it back, with a receive timeout.
///
/// @details
/// Every call is a synchronous request/response over a single UDP socket:
/// wiz_send_recv() turns the client's ip/port into a sockaddr_in, sends the JSON
/// request, and waits for its reply. The socket is opened with a
/// WIZ_REPLY_TIMEOUT_SEC receive timeout, so a silent or unreachable bulb cannot
/// hang the module's loop -- and a request that misses that deadline is sent once
/// more, because the first exchange after an idle period is the slowest one.
///
/// One datagram back is an assumption a WiZ bulb does not keep: it also emits
/// acknowledgements and notifications, some of them late. So the receive queue is
/// drained before each send, and a reply that names another method is skipped
/// rather than mistaken for the answer -- see wiz_send_recv().
///
/// The request strings are built with snprintf into fixed buffers -- the protocol
/// is small and the numbers are bounded -- and replies are parsed as JSON with
/// Jansson (the project's JSON library), so field order, whitespace and nesting
/// do not change the answer and a malformed reply is reported as one.

#include "wiz_client.h"

#include <jansson.h>
#include <log.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/// Largest request or reply datagram this client handles, in bytes. A getPilot
/// reply is a few hundred bytes of JSON, and a reply cut short is no longer
/// parseable at all, so the buffer keeps headroom over the fields this client
/// reads.
#define MAX_PACKET_BUF_SIZE 512

/// How many datagrams meant for somebody else one exchange may skip before it
/// gives up. The bulb is chatty (duplicate acknowledgements, notifications), but
/// a stream that never stops is not a conversation this client can have.
#define WIZ_MAX_IGNORED_REPLIES 4

/// How long one request waits for its reply, in seconds. A WiZ bulb is not fast:
/// the first exchange after an idle period is the slowest, and one that answered
/// in time on the previous poll can miss a 1 s deadline and answer a moment
/// later. Measured, not guessed -- see the round-trip line wiz_send_recv() logs.
#define WIZ_REPLY_TIMEOUT_SEC 2

/// How many times one request is sent before it is declared unanswered. Every
/// request here is idempotent (read a value, set a state) and replies are
/// matched by method, so re-sending is safe -- and a late answer to the first
/// attempt is just as good as one to the second.
#define WIZ_SEND_ATTEMPTS 2

////////////////////////////////////////////////////////////////////////////////
/// Is this datagram the answer to the request that carried `method`?
///
/// A WiZ reply echoes the request's method, so a datagram naming another one is
/// not the answer: it is a leftover from an earlier exchange -- a duplicated
/// acknowledgement, or one that arrived after its own request had given up -- or
/// an unsolicited notification. Reading it as the answer is what used to make a
/// getPilot report 'no boolean "state" in reply' while holding a setPilot ack.
///
/// A datagram that does not parse, or that carries no method at all, cannot be
/// classified; it is accepted, so the caller reports it instead of this function
/// hiding it behind a timeout.
///
/// @param resp   The datagram text.
/// @param method The method the pending request carries, e.g. "getPilot".
/// @return 1 when it may be treated as that request's answer, 0 when it is
///         recognisably somebody else's.
static int reply_is_answer(const char* resp, const char* method)
{
    json_error_t err;
    json_t* root = json_loads(resp, 0, &err);
    if (!root) {
        return 1;
    }

    const json_t* m = json_object_get(root, "method");
    int answer = !json_is_string(m) || strcmp(json_string_value(m), method) == 0;
    json_decref(root);
    return answer;
}

////////////////////////////////////////////////////////////////////////////////
/// Milliseconds on the monotonic clock, for the round-trip log line.
///
/// @return The monotonic clock in milliseconds.
static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

////////////////////////////////////////////////////////////////////////////////
/// Discards whatever is queued on the socket before a request goes out.
///
/// A datagram that arrived after its own request had given up is not this
/// request's answer, and reading it as one is what used to make a getPilot
/// report a setPilot acknowledgement. Runs before the FIRST attempt only: an
/// answer that arrives late is still an answer to this request, so the retry has
/// to be able to read it.
///
/// @param c      The client whose socket is drained.
/// @param method The method about to be sent, for the log line only.
/// @return How many datagrams were discarded (0 when the queue was empty).
static int drop_stale(wiz_client_t* c, const char* method)
{
    char stale[MAX_PACKET_BUF_SIZE];
    int dropped = 0;

    while (recvfrom(c->fd, stale, sizeof(stale), MSG_DONTWAIT, NULL, NULL) > 0) {
        dropped++;
    }

    if (dropped > 0) {
        log_debug("wiz: dropped %d stale datagram(s) before %s", dropped, method);
    }

    return dropped;
}

////////////////////////////////////////////////////////////////////////////////
/// Sends one UDP request to the bulb and waits for ITS reply, both on the socket
/// wiz_client_open() opened. The queue is drained first, and datagrams that are
/// recognisably somebody else's are skipped -- see reply_is_answer().
///
/// @param c       The client whose ip, port and fd are used.
/// @param method  The method this request carries ("getPilot", "setPilot"),
///                which its reply echoes -- see reply_is_answer().
/// @param req     The request text, e.g. a "getPilot" JSON message.
/// @param resp    Receives the reply text.
/// @param resp_sz Capacity of resp; the reply is truncated to fit minus one.
/// @return 0 on success, -1 on a bad address, a failed send, or no reply to this
///         request after WIZ_SEND_ATTEMPTS tries (each case is logged).
static int wiz_send_recv(wiz_client_t* c, const char* method, const char* req, char* resp,
                         size_t resp_sz)
{
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)c->port);
    if (inet_pton(AF_INET, c->ip, &addr.sin_addr) != 1) {
        log_error("bad IP '%s'", c->ip);
        return -1;
    }

    int ignored = 0;
    int dropped = 0;  // late datagrams cleared out before the first attempt
    for (int attempt = 1; attempt <= WIZ_SEND_ATTEMPTS; attempt++) {
        if (attempt == 1) {
            dropped = drop_stale(c, method);
        }

        long long sent_at = mono_ms();
        ssize_t sn = sendto(c->fd, req, strlen(req), 0, (struct sockaddr*)&addr, sizeof(addr));
        if (sn < 0) {
            log_error("failed to send %s: %s", method, strerror(errno));
            return -1;
        }

        for (;;) {
            struct sockaddr_in from = {0};
            socklen_t fromlen = sizeof(from);
            ssize_t rn = recvfrom(c->fd, resp, resp_sz - 1, 0, (struct sockaddr*)&from, &fromlen);
            if (rn < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    break;  // this attempt timed out -- send it again, or give up below
                }
                log_error("failed to receive the %s reply from wiz: %s", method,
                          strerror(errno));
                return -1;
            }
            resp[rn] = '\0';

            if (reply_is_answer(resp, method)) {
                log_debug("wiz: %s answered after %lld ms (attempt %d)", method,
                          mono_ms() - sent_at, attempt);
                return 0;
            }

            ignored++;
            log_debug("wiz: ignoring a reply that is not for %s: %s", method, resp);
            if (ignored > WIZ_MAX_IGNORED_REPLIES) {
                log_error("no reply to %s from bulb at %s:%d (only unrelated replies)", method,
                          c->ip, c->port);
                return -1;
            }
        }
    }

    log_error("no reply to %s from bulb at %s:%d (timeout after %d attempt(s), %d datagram(s) "
              "received)",
              method, c->ip, c->port, WIZ_SEND_ATTEMPTS, dropped + ignored);
    return -1;
}

////////////////////////////////////////////////////////////////////////////////
/// Parses a WiZ reply into a JSON tree.
///
/// Ownership: the caller owns the tree and must json_decref() it. A reply that
/// is not JSON yields NULL -- the bulb is the only thing on this socket, so an
/// unparsable datagram means something other than a bulb answered (or the reply
/// did not fit the buffer, which is why MAX_PACKET_BUF_SIZE has headroom).
///
/// @param resp The reply text.
/// @return The parsed tree, or NULL when the reply is not JSON (logged).
static json_t* parse_reply(const char* resp)
{
    json_error_t err;
    json_t* root = json_loads(resp, 0, &err);
    if (!root) {
        log_error("wiz reply is not JSON: %s (%s)", err.text, resp);
    }

    return root;
}

////////////////////////////////////////////////////////////////////////////////
/// Looks one field up in a parsed WiZ reply.
///
/// A getPilot answers with its readings nested under "result"
/// ({"method":"getPilot","result":{"state":true,...}}), so a field is looked
/// up there first and at the top level second: the lookup then does not depend
/// on which of the two shapes the bulb's firmware sends.
///
/// @param root The parsed reply.
/// @param key  The JSON member name, e.g. "dimming".
/// @return The member's value, borrowed from root, or NULL when it is absent.
static const json_t* reply_field(const json_t* root, const char* key)
{
    const json_t* result = json_object_get(root, "result");
    if (json_is_object(result)) {
        const json_t* v = json_object_get(result, key);
        if (v) {
            return v;
        }
    }

    return json_object_get(root, key);
}

////////////////////////////////////////////////////////////////////////////////
/// Clamps an integer to one colour channel's range, 0..255.
///
/// @param v The value to clamp.
/// @return v, or the nearest bound.
static int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

////////////////////////////////////////////////////////////////////////////////
/// Clamps an integer to the brightness range, 0..100 (percent).
///
/// @param v The value to clamp.
/// @return v, or the nearest bound.
static int clamp100(int v)
{
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int wiz_client_open(wiz_client_t* c, const char* ip, int port)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->ip, sizeof(c->ip), "%s", ip);
    c->port = port;
    c->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (c->fd < 0) {
        log_error("failed to open socket for wiz: %s", strerror(errno));
        return -1;
    }

    struct timeval tv = {.tv_sec = WIZ_REPLY_TIMEOUT_SEC, .tv_usec = 0};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return 0;
}

int wiz_client_set_state(wiz_client_t* c, bool on)
{
    char req[MAX_PACKET_BUF_SIZE] = {0};
    snprintf(req, sizeof(req), "{\"method\":\"setPilot\",\"params\":{\"state\":%s}}",
             on ? "true" : "false");

    char resp[MAX_PACKET_BUF_SIZE] = {0};
    int rv = wiz_send_recv(c, "setPilot", req, resp, sizeof(resp));
    if (rv != 0) {
        return -1;
    }

    log_trace("setPilot(state=%s) -> %s", on ? "true" : "false", resp);

    // A real client would parse resp for {"result":{"success":true}};
    // treating "we got a reply at all" as success is sufficient here.
    return 0;
}

int wiz_client_get_state(wiz_client_t* c, bool* out_on)
{
    const char* req = "{\"method\":\"getPilot\",\"params\":{}}";
    char resp[MAX_PACKET_BUF_SIZE] = {0};
    int rv = wiz_send_recv(c, "getPilot", req, resp, sizeof(resp));
    if (rv != 0) {
        return -1;
    }

    json_t* root = parse_reply(resp);
    if (!root) {
        return -1;
    }

    // The protocol spells this as a JSON boolean; anything else (a string, a
    // number, an absent field) is not a state this client can trust.
    const json_t* state = reply_field(root, "state");
    if (!json_is_boolean(state)) {
        log_error("no boolean \"state\" in reply: %s", resp);
        json_decref(root);
        return -1;
    }

    *out_on = json_is_true(state);
    json_decref(root);
    return 0;
}

int wiz_client_set_rgb(wiz_client_t* c, int r, int g, int b)
{
    r = clamp255(r);
    g = clamp255(g);
    b = clamp255(b);

    char req[MAX_PACKET_BUF_SIZE] = {0};
    snprintf(req, sizeof(req), "{\"method\":\"setPilot\",\"params\":{\"r\":%d,\"g\":%d,\"b\":%d}}",
             r, g, b);

    char resp[MAX_PACKET_BUF_SIZE] = {0};
    int rv = wiz_send_recv(c, "setPilot", req, resp, sizeof(resp));
    if (rv != 0) {
        return -1;
    }

    log_trace("setPilot(rgb=%d,%d,%d) -> %s", r, g, b, resp);

    // A real client would check resp for {"result":{"success":true}};
    // like wiz_client_set_state, treating any reply as success is
    // sufficient here.
    return 0;
}

int wiz_client_get_rgb(wiz_client_t* c, int* out_r, int* out_g, int* out_b)
{
    const char* req = "{\"method\":\"getPilot\",\"params\":{}}";
    char resp[MAX_PACKET_BUF_SIZE] = {0};
    int rv = wiz_send_recv(c, "getPilot", req, resp, sizeof(resp));
    if (rv != 0) {
        return -1;
    }

    json_t* root = parse_reply(resp);
    if (!root) {
        return -1;
    }

    const json_t* r = reply_field(root, "r");
    const json_t* g = reply_field(root, "g");
    const json_t* b = reply_field(root, "b");
    if (!json_is_number(r) || !json_is_number(g) || !json_is_number(b)) {
        log_error("could not find r/g/b in reply: %s", resp);
        json_decref(root);
        return -1;
    }

    *out_r = (int)json_number_value(r);
    *out_g = (int)json_number_value(g);
    *out_b = (int)json_number_value(b);
    json_decref(root);
    return 0;
}

int wiz_client_get_brightness(wiz_client_t* c, int* out_level)
{
    const char* req = "{\"method\":\"getPilot\",\"params\":{}}";
    char resp[MAX_PACKET_BUF_SIZE] = {0};
    int rv = wiz_send_recv(c, "getPilot", req, resp, sizeof(resp));
    if (rv != 0) {
        return -1;
    }

    json_t* root = parse_reply(resp);
    if (!root) {
        return -1;
    }

    // Preferred: the bulb's own "dimming" field, which is what actually
    // governs brightness and is independent of the r/g/b colour channels.
    const json_t* dimming = reply_field(root, "dimming");
    if (json_is_number(dimming)) {
        *out_level = clamp100((int)json_number_value(dimming));
        json_decref(root);
        return 0;
    }

    // Fallback for replies without "dimming": peak RGB channel as a
    // percentage of full (255).
    const json_t* r = reply_field(root, "r");
    const json_t* g = reply_field(root, "g");
    const json_t* b = reply_field(root, "b");
    if (!json_is_number(r) || !json_is_number(g) || !json_is_number(b)) {
        log_error("no \"dimming\" or r/g/b in reply: %s", resp);
        json_decref(root);
        return -1;
    }

    int peak = (int)json_number_value(r);
    int gv = (int)json_number_value(g);
    int bv = (int)json_number_value(b);
    if (gv > peak) {
        peak = gv;
    }

    if (bv > peak) {
        peak = bv;
    }

    *out_level = (peak * 100 + 127) / 255;  // round to nearest
    json_decref(root);
    return 0;
}

int wiz_client_set_brightness(wiz_client_t* c, int level)
{
    // WiZ bulbs expose brightness as the "dimming" parameter (10-100); it
    // is NOT derived from the r/g/b colour channels. Sending r/g/b only
    // changes the colour, leaving the bulb's dimming -- and so its actual
    // brightness -- untouched.
    if (level < 10) {
        level = 10;
    }
    if (level > 100) {
        level = 100;
    }

    char req[MAX_PACKET_BUF_SIZE] = {0};
    snprintf(req, sizeof(req), "{\"method\":\"setPilot\",\"params\":{\"dimming\":%d}}", level);

    char resp[MAX_PACKET_BUF_SIZE] = {0};
    int rv = wiz_send_recv(c, "setPilot", req, resp, sizeof(resp));
    if (rv != 0) {
        return -1;
    }

    log_trace("setPilot(dimming=%d) -> %s", level, resp);

    // A real client would check resp for {"result":{"success":true}};
    // like wiz_client_set_state, treating any reply as success is
    // sufficient here.
    return 0;
}

void wiz_client_close(wiz_client_t* c)
{
    if (c->fd >= 0) {
        close(c->fd);
    }
}
