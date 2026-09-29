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

#ifndef OPENHES_SRC_COMMON_HES_QUERY_H
#define OPENHES_SRC_COMMON_HES_QUERY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief The document layer: a thin, type-safe path-query API over libxml2 XPath
/// (Pattern 3) -- the only place in the tree that touches libxml2.
///
/// @details
/// A thin, type-safe layer over libxml2 XPath.  Instead of writing raw XPath,
/// the caller uses compact dot-notation paths.  The path is translated into a
/// real XPath expression and evaluated with libxml2's XPath engine.
///
/// ---------------------------------------------------------------------------
/// Path syntax
/// ---------------------------------------------------------------------------
///   - Paths are absolute, rooted at the document root element (usually "file").
///   - "." (or "/") separates path segments.
///   - Each segment names an element and may carry a 0-based index: [n]
///   - A trailing "@attr" reads an attribute of the last element.
///   - A trailing "text()" forces text access (optional; elements return
///     their text content by default).
///   - "*" matches any element name.
///
/// Examples:
///   file.fileFormat
///   file.overallProcess.@versionNumber
///   file.overallProcess.lexiconType.objectType.operationTable[0].@transCode
///   file.overallProcess.lexiconType.objectType.operationTable[1].inputs.data[0].@descriptiveName
///   file.overallProcess.lexiconType.objectType.addressingTable
///       (a path without an index selects ALL matches -> see hes_query_count())
///
/// Document order is preserved throughout, and the same element always answers
/// the same handle for a given document, so a list returned by
/// hes_query_elem_list() can be indexed the same way the path language indexes.
/// Paths and segment names are not bounded by any fixed-size buffer, so no path
/// can be silently truncated into a query for a different element.
///
/// The paths are for documents without namespaces (typical HES binding maps).
/// Returned strings are borrowed from the document and remain valid until
/// hes_query_close(); do not free them.
///
/// Memory/ownership: a hes_query_ctx_t owns the parsed document and every handle
/// handed out of it -- hes_query_close() releases the lot, and none of those
/// handles may be freed by the caller. Element lists are the exception: those
/// arrays are allocated for the caller and released with
/// hes_query_elem_list_free().
///
/// Threading: a context is not thread-safe. libxml2's parser state is initialized
/// once for the process and deliberately never torn down, so the rule is one
/// context per thread.

typedef struct hes_query_ctx hes_query_ctx_t;
typedef struct hes_query_elem hes_query_elem_t;

////////////////////////////////////////////////////////////////////////////////
/// Parse an XML file and prepare it for path queries.
/// @param filepath Path to the XML file
/// @return Query context, or NULL on failure.
hes_query_ctx_t* hes_query_open(const char* filepath);

////////////////////////////////////////////////////////////////////////////////
/// Free a query context and the underlying XML document.
void hes_query_close(hes_query_ctx_t* ctx);

////////////////////////////////////////////////////////////////////////////////
// Scalar queries
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// Return the text content (or @attribute) at the given path, or NULL.
const char* hes_query_string(hes_query_ctx_t* ctx, const char* path);

////////////////////////////////////////////////////////////////////////////////
/// Like hes_query_string() but parsed as a long; returns def on missing/error.
long hes_query_long(hes_query_ctx_t* ctx, const char* path, long def);

////////////////////////////////////////////////////////////////////////////////
/// Like hes_query_string() but parsed as a double; returns def on missing/error.
double hes_query_double(hes_query_ctx_t* ctx, const char* path, double def);

////////////////////////////////////////////////////////////////////////////////
/// Like hes_query_string() but parsed as a boolean; returns def on missing.
int hes_query_bool(hes_query_ctx_t* ctx, const char* path, int def);

////////////////////////////////////////////////////////////////////////////////
// Element handles
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// Return a handle to the first element matching path (or NULL).
hes_query_elem_t* hes_query_elem(hes_query_ctx_t* ctx, const char* path);

////////////////////////////////////////////////////////////////////////////////
/// Free an element handle (does NOT free the underlying document).
void hes_query_elem_free(hes_query_elem_t* elem);

////////////////////////////////////////////////////////////////////////////////
/// Element name of the handle.
const char* hes_query_elem_name(hes_query_elem_t* elem);

////////////////////////////////////////////////////////////////////////////////
/// Text content of the element handle (empty string if none).
const char* hes_query_elem_text(hes_query_elem_t* elem);

////////////////////////////////////////////////////////////////////////////////
/// Attribute value of the element handle: NULL when the attribute is absent,
/// "" when it is present with no value.
const char* hes_query_elem_attr(hes_query_elem_t* elem, const char* attr);

////////////////////////////////////////////////////////////////////////////////
/// Query a path RELATIVE to this element (no leading root segment).
const char* hes_query_elem_string(hes_query_elem_t* elem, const char* rel_path);

////////////////////////////////////////////////////////////////////////////////
// Collections
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// Number of nodes matching path (a path without a trailing index selects
/// all of the named children under the resolved parent).
int hes_query_count(hes_query_ctx_t* ctx, const char* path);

////////////////////////////////////////////////////////////////////////////////
/// Return an array of element handles for all nodes matching path.
/// The array is NULL-terminated; count receives the number of entries.
/// Free the whole array with hes_query_elem_list_free().
hes_query_elem_t** hes_query_elem_list(hes_query_ctx_t* ctx, const char* path, size_t* count);

////////////////////////////////////////////////////////////////////////////////
// Tree navigation
//
// For callers that walk a document instead of addressing it by path -- a
// loader that finds its tables wherever a document nests them, for instance.
// These exist so that no caller needs the underlying XML library's node API;
// the path language stays the primary way to read a value.
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// Direct element children of elem named name, in document order.
/// The array is NULL-terminated; count receives the number of entries.
/// Free the whole array with hes_query_elem_list_free().
hes_query_elem_t** hes_query_elem_children(hes_query_elem_t* elem, const char* name, size_t* count);

////////////////////////////////////////////////////////////////////////////////
/// Every element named name at any depth under the document root, in document
/// order. This is the one traversal the path language cannot express (it has no
/// descendant axis), and it is how a loader picks up constructs that a document
/// may nest at any depth.
/// The array is NULL-terminated; free it with hes_query_elem_list_free().
hes_query_elem_t** hes_query_find_all(hes_query_ctx_t* ctx, const char* name, size_t* count);

////////////////////////////////////////////////////////////////////////////////
/// Free a NULL-terminated array of handles returned by the functions above.
void hes_query_elem_list_free(hes_query_elem_t** list);

////////////////////////////////////////////////////////////////////////////////
/// Human-readable description of the last XPath error ("" if none).
const char* hes_query_last_error(hes_query_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif  // #ifndef OPENHES_SRC_COMMON_HES_QUERY_H
