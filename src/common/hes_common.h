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
/// @brief Shared wire format for HES-CLME messages carried on the gateway's
/// internal event bus (POC transport: NNG pub/sub).
///
/// @details
/// The foundation every other file in the tree builds on: the message struct, its
/// size limits, the primitive-action verbs, the module types and the Lexicon
/// address constants.
///
/// NOTE ON CONFORMANCE:
///   ISO/IEC 18012-4 (Event Encoding) defines the *normative* on-wire
///   syntax for HES-CLME (e.g. "get /lx/ob/uo/hv/ts/as/cv/va?ei=1").
///   This POC uses a simple fixed-size struct instead of that string
///   syntax, to keep the example runnable without a full 18012-4
///   encoder/decoder. Swap hes_clme_msg_t's wire representation for a
///   real 18012-4 codec when you're ready to be spec-conformant on
///   the wire; the module/service architecture below does not change.

#ifndef OPENHES_SRC_COMMON_HES_COMMON_H
#define OPENHES_SRC_COMMON_HES_COMMON_H

#include <stdint.h>
#include <string.h>

#define HES_PATH_MAX 128
/// Room for a value. Most payloads are one short token ("0", "1", "27"), but
/// the largest thing that travels here is the identity proof, which the crypto
/// service answers as
///   "pi=" + 64 + ";pk=" + 130 + ";sig=" + 128   = 334 characters.
/// Note that this is a floor, not a round number: a full P-256 public point (130
/// hex characters) plus a signature (128) is 258 characters before any framing
/// at all, so the answer cannot be made to fit in the 256 that used to be here.
/// See services/crypto/crypto.h.
#define HES_PAYLOAD_MAX 512

/// Room for a query string. Usually just one short selector ("ei=1", "di=2"),
/// but the 'da' (multiple data points) query can name a dozen transCodes at
/// once, e.g. "da=pi,vr,hs,ch,pd,nh,nw,ns,vl,il,ll,rl" (18012-4 5.2.5.5).
#define HES_QUERY_MAX 64

/// 'ci' (credInfo), the credential information that accompanies a message
/// (18012-4 5.2.5.5). A plain credential is a short token ("parent-key"), but
/// the field has to be able to carry a PROOF as well -- the same
/// "pi=..;pk=..;sig=.." answer the crypto service gives, 334 characters -- so
/// that an operation can require one. Sized to match HES_PAYLOAD_MAX.
#define HES_CRED_MAX 512
#define HES_USER_MAX 32

////////////////////////////////////////////////////////////////////////////////
/// Primitive action verbs, per ISO/IEC 18012-3 clause on primitive actions
/// (get, set/put, event-report, subscribe).
typedef enum hes_verb {
    HES_VERB_NONE = 0,
    HES_VERB_GET = 1,
    HES_VERB_PUT = 2,    ///< "set"
    HES_VERB_EVENT = 3,  ///< event-report: unsolicited value push
    HES_VERB_SUBSCRIBE = 4,
    HES_VERB_UNSUBSCRIBE = 5,
} hes_verb_t;

////////////////////////////////////////////////////////////////////////////////
/// moduleType short-form codes, per ISO/IEC 15045-4-1 / 18012-3 addressing
/// tables: 'hi' HAN interface, 'wi' WAN interface, 'sm' service module, 'it'
/// internal process (used only within a binding map's addressing table for
/// computed/internal values).
typedef enum hes_module_type {
    HES_MODTYPE_NONE = 0,
    HES_MODTYPE_HI,  ///< HAN interface module
    HES_MODTYPE_WI,  ///< WAN interface module
    HES_MODTYPE_SM,  ///< service module
    HES_MODTYPE_IT,  ///< internal process (binding map's own scratch value)
} hes_module_type_t;

////////////////////////////////////////////////////////////////////////////////
/// One HES-CLME message as carried on the internal bus; fixed size, so it can
/// be sent with nng_send()/nng_recv() without an allocator.
///
/// IMPORTANT: 'path' is deliberately the FIRST field. NNG's pub/sub
/// subscription filter (NNG_OPT_SUB_SUBSCRIBE) is a raw byte-prefix match
/// against the start of the message -- putting the Lexicon path first lets a
/// subscriber filter by "starts with /lx/ob/uo/li/ll/da/cv" directly, with no
/// per-message parsing needed on the transport side. Do not reorder this struct
/// without checking hes_bus.c's subscription setup.
typedef struct hes_clme_msg {
    /// Lexicon address, e.g. "/lx/ob/so/ti/rt/st/cv" -- also the pub/sub topic.
    char path[HES_PATH_MAX];
    uint8_t verb;  ///< hes_verb_t

    /// CLIP-style 'di': on a downstream GET/PUT/SUBSCRIBE it is the destination
    /// deviceIndex; on an upstream EVENT, the source deviceIndex. The binding
    /// map keys value_cache on it; interface modules use it to pick the device
    /// (0 = not applicable / legacy).
    uint32_t device_index;

    char query[HES_QUERY_MAX];      ///< e.g. "ei=1", "di=2", may be empty
    uint32_t payload_len;           ///< bytes used in payload[]
    char payload[HES_PAYLOAD_MAX];  ///< value, stringified ("1785612345", "on", "23.5")

    /// Accompanying information (18012-3 Table 134): the presented credential
    /// and the claimed user name. A binding-map input whose 'ap'
    /// (accompanyOperation) is 'au' requires this information to be authorized
    /// before that row may output -- the per-message ("Mode B") path, used when
    /// the decision cannot be pre-computed into the row's 'at' field because
    /// several senders share one row. See src/services/auth/README.md.
    ///
    /// SECRET: cred_info must never be logged, echoed in a reply, or returned
    /// from a GET. These fields are last so that 'path' stays first, which NNG's
    /// prefix-based subscription filter depends on.
    char cred_info[HES_CRED_MAX];  ///< 'ci' presented credential
    char user_name[HES_USER_MAX];  ///< 'un' claimed user name

    /// High-resolution UTC time stamp of the moment this message entered the
    /// bus. Filled in by hes_bus_send() from hes_clock_now_ns(), never by the
    /// sender, so no module can forget or forge it. 18012-3 11.2.3.1: "a
    /// high-resolution time stamping block is required for every HES-CLME
    /// message to ensure proper sequence of events within the HES gateway
    /// system." Placed last for the same reason as the two fields above:
    /// 'path' must stay first for NNG's prefix-based subscription filter.
    uint64_t timestamp_ns;
} hes_clme_msg_t;

////////////////////////////////////////////////////////////////////////////////
/// Zeroes a message, i.e. "nothing to send yet".
///
/// @param m The message to reset.
static inline void hes_msg_init(hes_clme_msg_t* m)
{
    memset(m, 0, sizeof(*m));
}

////////////////////////////////////////////////////////////////////////////////
/// Copies a NUL-terminated string into a fixed-size buffer, always
/// NUL-terminating and never reading past the source's terminator.
///
/// @param dst Destination buffer.
/// @param dstsz Capacity of dst.
/// @param src Source string.
/// @return The number of bytes copied (== strlen(src) if it fit, dstsz-1 if it
///         was truncated).
static inline size_t hes_strlcpy(char* dst, size_t dstsz, const char* src)
{
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

////////////////////////////////////////////////////////////////////////////////
/// Sets a message's payload from a C string, capping it at HES_PAYLOAD_MAX.
///
/// @param m The message to fill in.
/// @param s The payload text.
static inline void hes_msg_set_payload_str(hes_clme_msg_t* m, const char* s)
{
    m->payload_len = (uint32_t)hes_strlcpy(m->payload, HES_PAYLOAD_MAX, s);
}

////////////////////////////////////////////////////////////////////////////////
/// Sets a message's Lexicon path, capping it at HES_PATH_MAX.
///
/// @param m The message to fill in.
/// @param p The absolute Lexicon address.
static inline void hes_msg_set_path(hes_clme_msg_t* m, const char* p)
{
    hes_strlcpy(m->path, HES_PATH_MAX, p);
}

////////////////////////////////////////////////////////////////////////////////
/// Well-known Lexicon addresses used by this POC.
///
/// Sourced from ISO/IEC 18012-3 (CDV draft), tables for the Identification
/// service (clause 11.2.2 area) and Time service (clause 11.2.3 area). Short
/// forms: 'id' identification, 'ti' time, 'co' centralOperations, 'rt' realTime,
/// 'st' status, 'cv' currentValue.
///
/// The time service has THREE functional objects (11.2.3.2/3/4), so it answers
/// on three addresses -- that is what makes 11.2.3 look like so many tables:
/// the address says where, the memoryType says who may touch it. See
/// services/time/time.h.
////////////////////////////////////////////////////////////////////////////////
/// @name Identification service
/// @{
#define HES_LX_ID "/lx/ob/so/id"  ///< identification service: configurationData
#define HES_LX_ID_CENTRALOPS_CV \
    "/lx/ob/so/id/co/st/cv"  ///< identification service: gateway-wide info
#define HES_LX_ID_DISCOVER_BM "/lx/ac/so/id/dc/bm/cv"  ///< functional action: discover binding maps
#define HES_LX_TIME_REALTIME_CV \
    "/lx/ob/so/ti/rt/st/cv"  ///< time service 11.2.3.2: realTime (Tables 45-48)
#define HES_LX_TIME_LOCALTZ_CV \
    "/lx/ob/so/ti/tz/st/cv"  ///< time service 11.2.3.3: localTimeZone (49-52)
#define HES_LX_TIME_SOURCE_CV \
    "/lx/ob/so/ti/st/mp/cv"                 ///< time service 11.2.3.4: sourceOfTime (53-56)
#define HES_LX_CRYPTO "/lx/ob/so/cr"        ///< cryptographic service: capability (Tables 70-71)
#define HES_LX_CRYPTO_CI "/lx/ob/so/cr/ci"  ///< cryptographic service: ciphers (Tables 72-73)

////////////////////////////////////////////////////////////////////////////////
// Well-known Lexicon addresses used by this POC
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// @}
/// @name Lighting (and the SensorTag inputs)
/// @{
/// The address is /lx/ob/uo/li/ll/<applicationObject>/cv: the applicationObject
/// is part of the address, never optional. 'da' is the digitalActuator (on/off)
/// and 'aa' the analogActuator (dimming) -- 18012-3 F.3.3 Table 144, and the
/// form 18012-4 5.2.8.2.2 uses throughout.
#define HES_LX_LIGHT_CV \
    "/lx/ob/uo/li/ll/da/cv"  ///< lighting digitalActuator (on/off) current value
#define HES_LX_LIGHT_BRIGHTNESS_CV \
    "/lx/ob/uo/li/ll/aa/cv"  ///< lighting analogActuator: va 0-100 => 0%-100% brightness
#define HES_LX_BTN_CV \
    "/lx/ob/uo/ui/ud/da/cv"  ///< user-interface user object: the SensorTag button/switch
#define HES_LX_TEMP_CV "/lx/ob/uo/hv/ts/as/cv"  ///< SensorTag ambient temperature
/// @}

#endif  // #ifndef OPENHES_SRC_COMMON_HES_COMMON_H
