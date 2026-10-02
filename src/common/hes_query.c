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
/// @brief Implementation of the document layer (hes_query.h): path translation,
/// XPath evaluation, and the element/list handles.
///
/// @details
/// The unit has four parts:
///
///   - the context and element handles: a hes_query_ctx_t owns the parsed
///     document and its root; a hes_query_elem_t wraps an xmlNodePtr plus its
///     context, so relative queries can be evaluated later.
///
///   - _translate_path(): turns the compact dot-notation path into a real XPath
///     expression, using a growable buffer so no path can be silently truncated
///     into a query for a different element.
///
///   - the scalar readers (string/long/double/bool) and the element/list readers.
///     A scalar query reads a node-set and returns a borrowed string; an element
///     query returns a handle.
///
///   - hes_query_open()/hes_query_close(): parse the file, and release the
///     document and every handle derived from it. libxml2's parser state is
///     initialized on open and deliberately never cleaned up (see the header).

#include "hes_query.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
/// Opaque context: owns the parsed document and remembers the root element.
struct hes_query_ctx {
    xmlDocPtr doc;
    xmlNodePtr root;
    char last_error[256];
};

////////////////////////////////////////////////////////////////////////////////
/// Element handle: wraps an xmlNodePtr and remembers the owning context so
/// relative queries can be evaluated.
struct hes_query_elem {
    hes_query_ctx_t* ctx;
    xmlNodePtr node;
};

////////////////////////////////////////////////////////////////////////////////
// Internal helpers
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// Append n bytes of s to a growable buffer.
static int _append_n(char** out, size_t* len, size_t* cap, const char* s, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t ncap = (*cap + n + 1) * 2;
        char* tmp = (char*)realloc(*out, ncap);
        if (tmp == NULL) {
            return -1;
        }

        *out = tmp;
        *cap = ncap;
    }

    memcpy(*out + *len, s, n);
    *len += n;
    (*out)[*len] = '\0';
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Append a NUL-terminated string to a growable buffer.
static int _append(char** out, size_t* len, size_t* cap, const char* s)
{
    return _append_n(out, len, cap, s, strlen(s));
}

////////////////////////////////////////////////////////////////////////////////
/// Translate a simplified path into a real XPath expression.
///
///   "file.overallProcess.lexiconType.objectType.operationTable[0].@transCode"
///        -> "/file/overallProcess/lexiconType/objectType/operationTable[1]/@transCode"
///
/// If absolute is 0 the result is a relative XPath suitable for evaluating
/// against a context node.
///
/// Every fragment is appended to a buffer that grows on demand: a segment is
/// never copied through a fixed-size buffer, so a long path cannot be silently
/// truncated into a query for some other element.
static char* _translate_path(const char* path, int absolute)
{
    size_t cap = 128, len = 0;
    char* out = (char*)malloc(cap);
    if (out == NULL) {
        return NULL;
    }
    out[0] = '\0';

    if (absolute && _append(&out, &len, &cap, "/") != 0) {
        goto fail;
    }

    int first = 1;
    const char* p = path;

    while (*p) {
        while (*p == '.' || *p == '/') {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        const char* seg_start = p;
        while (*p && *p != '.' && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg_start);

        // The first step carries no separator of its own: an absolute path has
        // already emitted its leading '/', a relative one must not gain one.
        if (!first && _append_n(&out, &len, &cap, "/", 1) != 0) {
            goto fail;
        }

        if (seg_start[0] == '@') {
            // attribute access: "@name" -> "/@name"
            if (_append_n(&out, &len, &cap, "@", 1) != 0 ||
                _append_n(&out, &len, &cap, seg_start + 1, seg_len - 1) != 0) {
                goto fail;
            }
        } else if (seg_len == sizeof("text()") - 1 &&
                   memcmp(seg_start, "text()", sizeof("text()") - 1) == 0) {
            if (_append_n(&out, &len, &cap, "text()", sizeof("text()") - 1) != 0) {
                goto fail;
            }
        } else {
            // element, optionally with a [n] index (0-based in the query)
            const char* bracket = (const char*)memchr(seg_start, '[', seg_len);
            long idx = 0;
            int indexed = 0;

            if (bracket != NULL) {
                char* endp = NULL;
                long parsed = strtol(bracket + 1, &endp, 10);
                // Only "[digits]" as the whole tail is an index; anything else is
                // part of the name and will fail to evaluate, which beats quietly
                // querying a different element.
                if (endp != NULL && *endp == ']' && (size_t)(endp - seg_start) == seg_len - 1) {
                    idx = parsed;
                    indexed = 1;
                }
            }

            size_t name_len = indexed ? (size_t)(bracket - seg_start) : seg_len;
            if (_append_n(&out, &len, &cap, seg_start, name_len) != 0) {
                goto fail;
            }

            if (indexed) {
                // A long is at most 20 digits, so 32 bytes cannot truncate.
                char index_buf[32];
                int n = snprintf(index_buf, sizeof(index_buf), "[%ld]", idx + 1);
                if (n <= 0 || (size_t)n >= sizeof(index_buf) ||
                    _append_n(&out, &len, &cap, index_buf, (size_t)n) != 0) {
                    goto fail;
                }
            }
        }

        first = 0;
    }

    return out;

fail:
    free(out);
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// Extract text content from an attribute / text / element node.
static const char* _node_text(xmlNodePtr node)
{
    if (node == NULL) {
        return NULL;
    }

    if (node->type == XML_ATTRIBUTE_NODE) {
        if (node->children && node->children->content) {
            return (const char*)node->children->content;
        }

        if (node->content) {
            return (const char*)node->content;
        }

        return "";
    }

    if (node->type == XML_TEXT_NODE) {
        return (node->content != NULL) ? (const char*)node->content : "";
    }

    if (node->type == XML_ELEMENT_NODE) {
        xmlNodePtr c = node->children;
        while (c != NULL) {
            if (c->type == XML_TEXT_NODE && c->content != NULL) {
                const char* t = (const char*)c->content;
                while (*t == ' ' || *t == '\n' || *t == '\t' || *t == '\r') {
                    t++;
                }
                if (*t != '\0') {
                    return (const char*)c->content;
                }
            }
            c = c->next;
        }

        return "";
    }

    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// Evaluate a ready-made XPath expression with `node` as the context node
/// (NULL leaves the context node unset, i.e. the document itself).
static xmlXPathObjectPtr _eval_expr(hes_query_ctx_t* ctx, xmlNodePtr node, const char* xpath)
{
    if (ctx == NULL || xpath == NULL) {
        return NULL;
    }

    xmlXPathContextPtr xpc = xmlXPathNewContext(ctx->doc);
    if (xpc == NULL) {
        return NULL;
    }
    if (node != NULL) {
        xpc->node = node;
    }

    xmlXPathObjectPtr res = xmlXPathEvalExpression(BAD_CAST xpath, xpc);
    if (res == NULL) {
        const xmlError* err = xmlGetLastError();
        if (err != NULL && err->message != NULL) {
            snprintf(ctx->last_error, sizeof(ctx->last_error), "%s", err->message);
        } else {
            snprintf(ctx->last_error, sizeof(ctx->last_error), "XPath evaluation failed: %s",
                     xpath);
        }
    } else {
        ctx->last_error[0] = '\0';
    }

    xmlXPathFreeContext(xpc);
    return res;
}

////////////////////////////////////////////////////////////////////////////////
/// Translate a simplified path and evaluate it against `node`.
///
/// Every query in this file goes through here, so the path language is turned
/// into XPath in exactly one place.
static xmlXPathObjectPtr _eval(hes_query_ctx_t* ctx,
                               xmlNodePtr node,
                               const char* path,
                               int absolute)
{
    if (ctx == NULL || path == NULL) {
        return NULL;
    }

    char* xpath = _translate_path(path, absolute);
    if (xpath == NULL) {
        return NULL;
    }

    xmlXPathObjectPtr res = _eval_expr(ctx, node, xpath);
    free(xpath);
    return res;
}

////////////////////////////////////////////////////////////////////////////////
/// Evaluate an absolute path, i.e. one rooted at the document's root element.
/// Takes the context first and alone, so a NULL context is answered without
/// dereferencing it.
static xmlXPathObjectPtr _eval_root(hes_query_ctx_t* ctx, const char* path)
{
    if (ctx == NULL) {
        return NULL;
    }

    return _eval(ctx, ctx->root, path, 1);
}

////////////////////////////////////////////////////////////////////////////////
/// Wrap the node set of an evaluated expression as a NULL-terminated array of
/// handles, consuming `res` either way. Returns NULL when there is no match
/// (which is not an error) and sets *count accordingly.
static hes_query_elem_t** _handle_list(hes_query_ctx_t* ctx, xmlXPathObjectPtr res, size_t* count)
{
    if (count != NULL) {
        *count = 0;
    }

    if (res == NULL || res->type != XPATH_NODESET || res->nodesetval == NULL ||
        res->nodesetval->nodeNr == 0) {
        if (res != NULL) {
            xmlXPathFreeObject(res);
        }

        return NULL;
    }

    int n = res->nodesetval->nodeNr;
    hes_query_elem_t** arr = (hes_query_elem_t**)calloc((size_t)n + 1, sizeof(*arr));
    if (arr == NULL) {
        xmlXPathFreeObject(res);
        return NULL;
    }

    for (int i = 0; i < n; i++) {
        arr[i] = (hes_query_elem_t*)malloc(sizeof(**arr));
        if (arr[i] == NULL) {
            for (int j = 0; j < i; j++) {
                free(arr[j]);
            }
            free(arr);

            xmlXPathFreeObject(res);
            return NULL;
        }

        arr[i]->ctx = ctx;
        arr[i]->node = res->nodesetval->nodeTab[i];
    }
    arr[n] = NULL;

    if (count != NULL) {
        *count = (size_t)n;
    }

    xmlXPathFreeObject(res);
    return arr;
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

hes_query_ctx_t* hes_query_open(const char* filepath)
{
    if (filepath == NULL) {
        return NULL;
    }

    // libxml2 initializes itself lazily and every one of its entry points calls
    // this, so it is belt-and-braces -- but doing it here keeps the library's
    // lifecycle inside this translation unit.
    xmlInitParser();

    xmlDocPtr doc = xmlParseFile(filepath);
    if (doc == NULL) {
        return NULL;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root == NULL) {
        // Paths are rooted at the root element, so a document without one has
        // nothing this API could address: treat it as a failed load.
        xmlFreeDoc(doc);
        return NULL;
    }

    hes_query_ctx_t* ctx = (hes_query_ctx_t*)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        xmlFreeDoc(doc);
        return NULL;
    }

    ctx->doc = doc;
    ctx->root = root;
    return ctx;
}

void hes_query_close(hes_query_ctx_t* ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->doc != NULL) {
        xmlFreeDoc(ctx->doc);
    }

    free(ctx);

    // Deliberately no xmlCleanupParser() here: that discards libxml2's global
    // state and is unsafe while another thread may still be parsing. Dropping
    // one document is not a reason to unload the library.
}

const char* hes_query_string(hes_query_ctx_t* ctx, const char* path)
{
    xmlXPathObjectPtr res = _eval_root(ctx, path);
    if (res == NULL) {
        return NULL;
    }

    // Every path this dialect can express is a location path, so the result is
    // always a node set, whose text lives in the document. A non-node-set (only
    // reachable if the expression language ever grows function calls, e.g.
    // string()) owns its string and would be freed by xmlXPathFreeObject()
    // below, so it is reported as "no value" rather than returned dangling.
    const char* out = NULL;
    if (res->type == XPATH_NODESET && res->nodesetval != NULL && res->nodesetval->nodeNr > 0) {
        out = _node_text(res->nodesetval->nodeTab[0]);
    }

    xmlXPathFreeObject(res);
    return out;
}

long hes_query_long(hes_query_ctx_t* ctx, const char* path, long def)
{
    const char* s = hes_query_string(ctx, path);
    if (s == NULL || *s == '\0') {
        return def;
    }

    char* end = NULL;
    long v = strtol(s, &end, 10);
    return (end == s) ? def : v;
}

double hes_query_double(hes_query_ctx_t* ctx, const char* path, double def)
{
    const char* s = hes_query_string(ctx, path);
    if (s == NULL || *s == '\0') {
        return def;
    }

    char* end = NULL;
    double v = strtod(s, &end);
    return (end == s) ? def : v;
}

int hes_query_bool(hes_query_ctx_t* ctx, const char* path, int def)
{
    const char* s = hes_query_string(ctx, path);
    if (s == NULL) {
        return def;
    }

    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 ||
        strcmp(s, "on") == 0) {
        return 1;
    }

    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0 || strcmp(s, "no") == 0 ||
        strcmp(s, "off") == 0) {
        return 0;
    }

    return def;
}

hes_query_elem_t* hes_query_elem(hes_query_ctx_t* ctx, const char* path)
{
    xmlXPathObjectPtr res = _eval_root(ctx, path);
    if (res == NULL || res->type != XPATH_NODESET || res->nodesetval == NULL ||
        res->nodesetval->nodeNr == 0) {
        if (res != NULL) {
            xmlXPathFreeObject(res);
        }

        return NULL;
    }

    xmlNodePtr node = res->nodesetval->nodeTab[0];
    xmlXPathFreeObject(res);

    hes_query_elem_t* elem = (hes_query_elem_t*)malloc(sizeof(*elem));
    if (elem == NULL) {
        return NULL;
    }

    elem->ctx = ctx;
    elem->node = node;
    return elem;
}

void hes_query_elem_free(hes_query_elem_t* elem)
{
    free(elem);
}

const char* hes_query_elem_name(hes_query_elem_t* elem)
{
    if (elem == NULL || elem->node == NULL) {
        return NULL;
    }

    return (const char*)elem->node->name;
}

const char* hes_query_elem_text(hes_query_elem_t* elem)
{
    if (elem == NULL) {
        return NULL;
    }

    return _node_text(elem->node);
}

const char* hes_query_elem_attr(hes_query_elem_t* elem, const char* attr)
{
    if (elem == NULL || elem->node == NULL || attr == NULL) {
        return NULL;
    }

    xmlAttrPtr a = elem->node->properties;
    while (a != NULL) {
        if (strcmp((const char*)a->name, attr) == 0) {
            if (a->children != NULL && a->children->content != NULL) {
                return (const char*)a->children->content;
            }
            return "";
        }
        a = a->next;
    }

    return NULL;
}

const char* hes_query_elem_string(hes_query_elem_t* elem, const char* rel_path)
{
    if (elem == NULL || elem->ctx == NULL || elem->node == NULL || rel_path == NULL) {
        return NULL;
    }

    // Same contract as hes_query_string(): node-set results only, borrowed from
    // the document.
    xmlXPathObjectPtr res = _eval(elem->ctx, elem->node, rel_path, 0);
    if (res == NULL) {
        return NULL;
    }

    const char* out = NULL;
    if (res->type == XPATH_NODESET && res->nodesetval != NULL && res->nodesetval->nodeNr > 0) {
        out = _node_text(res->nodesetval->nodeTab[0]);
    }

    xmlXPathFreeObject(res);
    return out;
}

int hes_query_count(hes_query_ctx_t* ctx, const char* path)
{
    xmlXPathObjectPtr res = _eval_root(ctx, path);
    if (res == NULL) {
        return 0;
    }

    int n = 0;
    if (res->type == XPATH_NODESET && res->nodesetval != NULL) {
        n = res->nodesetval->nodeNr;
    }

    xmlXPathFreeObject(res);
    return n;
}

hes_query_elem_t** hes_query_elem_list(hes_query_ctx_t* ctx, const char* path, size_t* count)
{
    if (ctx == NULL || path == NULL) {
        if (count != NULL) {
            *count = 0;
        }

        return NULL;
    }

    return _handle_list(ctx, _eval_root(ctx, path), count);
}

hes_query_elem_t** hes_query_elem_children(hes_query_elem_t* elem, const char* name, size_t* count)
{
    if (elem == NULL || elem->ctx == NULL || name == NULL) {
        if (count != NULL) {
            *count = 0;
        }

        return NULL;
    }

    return _handle_list(elem->ctx, _eval(elem->ctx, elem->node, name, 0), count);
}

hes_query_elem_t** hes_query_find_all(hes_query_ctx_t* ctx, const char* name, size_t* count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (ctx == NULL || name == NULL || *name == '\0') {
        return NULL;
    }

    // '//name' is the descendant axis the path language has no syntax for, so
    // this single expression is assembled here instead of translated.
    size_t len = strlen(name);
    char* expr = (char*)malloc(len + sizeof("//"));
    if (expr == NULL) {
        return NULL;
    }
    memcpy(expr, "//", 2);
    memcpy(expr + 2, name, len + 1);

    hes_query_elem_t** list = _handle_list(ctx, _eval_expr(ctx, ctx->root, expr), count);
    free(expr);
    return list;
}

void hes_query_elem_list_free(hes_query_elem_t** list)
{
    if (list == NULL) {
        return;
    }

    for (size_t i = 0; list[i] != NULL; i++) {
        hes_query_elem_free(list[i]);
    }

    free(list);
}

const char* hes_query_last_error(hes_query_ctx_t* ctx)
{
    if (ctx == NULL) {
        return "";
    }

    return ctx->last_error;
}
