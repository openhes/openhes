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
/// @brief Unit tests for the XML layer (src/common/hes_query.c), the one
/// translation unit in the tree that talks to libxml2.
///
/// @details
/// What is covered:
///   - the path language: '.'/'/' separators, 0-based indices, @attributes,
///     text(), the '*' wildcard, relative paths from an element, and the
///     difference between "no match" (NULL, not an error) and "malformed path"
///     (NULL plus a message from hes_query_last_error())
///   - scalars and their defaults (long/double/bool), including that a missing
///     or empty value yields the caller's default
///   - element handles: name/text/attr, and attribute NULL (absent) vs ""
///     (present but empty)
///   - collections: count, elem_list (NULL-terminated, document order) and the
///     documented invariant that list position == path index
///   - tree navigation: elem_children (direct children only) vs find_all (any
///     depth) -- the pair that lets bm.c find its tables without libxml2
///   - two regressions: a segment longer than the translator's old fixed-size
///     buffer must still match, and a deeply nested path must not be truncated
///   - failure modes: unopenable file, unparsable XML, document without a root
///     element, NULL arguments, double free-safe list freeing
///
/// Fixture: tests/data/hes_query_sample.xml. ctest runs this from the tests
/// source directory (see tests/CMakeLists.txt), so the path needs no argv. The
/// cases are munit tests (deps/munit).
///
/// Build/run: see tests/CMakeLists.txt (ctest).

#include "common/hes_query.h"

#include <munit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FIXTURE "data/hes_query_sample.xml"

/// Paths used by several checks.
#define OT0 "file.overallProcess.lexiconType[0].objectType[0].operationTable[0]"
#define OT0_DATA OT0 ".data"

////////////////////////////////////////////////////////////////////////////////
/// Write 'xml' to a temporary file; returns its path (caller unlinks + frees).
static char* write_temp(const char* xml)
{
    char* path = strdup("/tmp/hes_query_test_XXXXXX");
    if (path == NULL) {
        return NULL;
    }

    int fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        return NULL;
    }

    FILE* f = fdopen(fd, "w");
    if (f == NULL) {
        close(fd);
        unlink(path);
        free(path);
        return NULL;
    }

    fputs(xml, f);
    if (fclose(f) != 0) {
        unlink(path);
        free(path);
        return NULL;
    }

    return path;
}

////////////////////////////////////////////////////////////////////////////////
/// A document that cannot be opened, parsed, or has no root: all must fail.
static MunitResult test_open_failures(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    hes_query_close(NULL);  // must not crash

    munit_assert_null(hes_query_open(NULL));
    munit_assert_null(hes_query_open("/nonexistent/hes_query_sample.xml"));
    munit_assert_null(hes_query_open("/tmp"));  // a directory, not a document

    // A NULL context is answered, not dereferenced, by every entry point.
    munit_assert_null(hes_query_string(NULL, "file"));
    munit_assert_int(hes_query_count(NULL, "file"), ==, 0);
    munit_assert_null(hes_query_elem(NULL, "file"));
    munit_assert_long(hes_query_long(NULL, "file", 7), ==, 7);

    // Malformed XML, and a well-formed document with no root element: the
    // first fails in libxml2, the second has nothing a path could address.
    char* path = write_temp("<?xml version=\"1.0\"?>\n");
    munit_assert_not_null(path);
    munit_assert_null(hes_query_open(path));
    unlink(path);
    free(path);

    path = write_temp("<file><unclosed></file>\n");
    munit_assert_not_null(path);
    munit_assert_null(hes_query_open(path));
    unlink(path);
    free(path);

    return MUNIT_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// Path language + scalar accessors, and "missing" vs "empty".
static MunitResult test_scalars(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    hes_query_ctx_t* ctx = hes_query_open(FIXTURE);
    munit_assert_not_null(ctx);

    // element text, text(), attribute, '/' as separator as well as '.'
    munit_assert_string_equal(hes_query_string(ctx, "file.fileFormat"), "HES");
    munit_assert_string_equal(hes_query_string(ctx, "file.fileFormat.text()"), "HES");
    munit_assert_string_equal(hes_query_string(ctx, "file.@transCode"), "lx");
    munit_assert_string_equal(hes_query_string(ctx, "file/overallProcess/lexiconType[0]/@transCode"),
                              "l1");
    munit_assert_string_equal(hes_query_string(ctx, "file.overallProcess.lexiconType[1].@transCode"),
                              "l2");

    // 0-based index reaches the right repeated element ...
    munit_assert_string_equal(hes_query_string(ctx, OT0 ".@transCode"), "ot1");
    munit_assert_string_equal(hes_query_string(ctx,
                                               "file.overallProcess.lexiconType[0].objectType[0]."
                                               "operationTable[1].@transCode"),
                              "ot2");
    munit_assert_string_equal(hes_query_string(ctx, OT0 ".data[1]"), "gt");
    // ... and a path without an index selects the first match.
    munit_assert_string_equal(
            hes_query_string(ctx, "file.overallProcess.lexiconType.objectType.@transCode"), "o1");
    // An index past the end is simply no match.
    munit_assert_null(hes_query_string(ctx, "file.overallProcess.lexiconType[2]"));

    // Missing element / missing attribute: NULL, and NOT an error.
    munit_assert_null(hes_query_string(ctx, "file.noSuchElement"));
    munit_assert_null(hes_query_string(ctx, "file.fileFormat.@nope"));
    munit_assert_char(hes_query_last_error(ctx)[0], ==, '\0');

    // Present but empty, in all three shapes: NULL must not be returned.
    const char* emptyText = hes_query_string(ctx, "file.empty");
    munit_assert_not_null(emptyText);
    munit_assert_char(emptyText[0], ==, '\0');
    const char* blankText = hes_query_string(ctx, "file.blank");
    munit_assert_not_null(blankText);
    munit_assert_char(blankText[0], ==, '\0');  // whitespace-only text
    const char* emptyAttr = hes_query_string(ctx, "file.emptyAttr.@code");
    munit_assert_not_null(emptyAttr);
    munit_assert_char(emptyAttr[0], ==, '\0');

    // A malformed path is an error, and reports one.
    munit_assert_null(hes_query_string(ctx, "file.["));
    munit_assert_char(hes_query_last_error(ctx)[0], !=, '\0');
    // A later successful query clears it again.
    munit_assert_not_null(hes_query_string(ctx, "file.fileFormat"));
    munit_assert_char(hes_query_last_error(ctx)[0], ==, '\0');

    // Scalars: parsed values, and the caller's default whenever the value is
    // missing, empty or not of that type.
    munit_assert_long(hes_query_long(ctx, OT0_DATA "[0]", -1), ==, 1);
    munit_assert_long(hes_query_long(ctx, "file.fileFormat", -1), ==, -1);
    munit_assert_long(hes_query_long(ctx, "file.noSuchElement", 42), ==, 42);
    munit_assert_long(hes_query_long(ctx, "file.empty", -1), ==, -1);
    munit_assert_double(hes_query_double(ctx, OT0_DATA "[0]", -1.0), ==, 1.0);
    munit_assert_double(hes_query_double(ctx, "file.fileFormat", -1.0), ==, -1.0);
    munit_assert_int(hes_query_bool(ctx, OT0_DATA "[0]", 0), ==, 1);     // "1"
    munit_assert_int(hes_query_bool(ctx, "file.fileFormat", 5), ==, 5);  // not a boolean
    munit_assert_int(hes_query_bool(ctx, "file.noSuchElement", 5), ==, 5);

    // count(): all matches when no index is given, 0 when nothing matches.
    munit_assert_int(hes_query_count(ctx, "file.overallProcess.lexiconType"), ==, 2);
    munit_assert_int(
            hes_query_count(ctx, "file.overallProcess.lexiconType[0].objectType.operationTable"),
            ==, 2);
    munit_assert_int(
            hes_query_count(ctx, "file.overallProcess.lexiconType[1].objectType.operationTable"),
            ==, 1);
    munit_assert_int(hes_query_count(ctx, "file.*"), ==, 5);  // wildcard: 5 child elements
    munit_assert_int(hes_query_count(ctx, "file.noSuchElement"), ==, 0);
    munit_assert_int(hes_query_count(NULL, "file"), ==, 0);

    hes_query_close(ctx);
    return MUNIT_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// Element handles: name, text, attributes, and relative queries.
static MunitResult test_element_handles(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    hes_query_ctx_t* ctx = hes_query_open(FIXTURE);
    munit_assert_not_null(ctx);

    hes_query_elem_t* ot = hes_query_elem(ctx, OT0);
    munit_assert_not_null(ot);
    munit_assert_string_equal(hes_query_elem_name(ot), "operationTable");
    munit_assert_string_equal(hes_query_elem_attr(ot, "transCode"), "ot1");
    munit_assert_string_equal(hes_query_elem_attr(ot, "descriptiveName"), "operationTable");

    // Relative paths: same 0-based indexing, rooted at this element.
    munit_assert_string_equal(hes_query_elem_string(ot, "data[0]"), "1");
    munit_assert_string_equal(hes_query_elem_string(ot, "data[1].@descriptiveName"), "operation");
    munit_assert_string_equal(hes_query_elem_string(ot, "inputs.data[0]"), "6");
    munit_assert_null(hes_query_elem_string(ot, "data[9]"));
    munit_assert_null(hes_query_elem_string(ot, "@nope"));
    munit_assert_null(hes_query_elem_string(NULL, "data"));
    munit_assert_null(hes_query_elem_string(ot, NULL));

    hes_query_elem_free(ot);

    // Missing path -> no handle; a handle's text of an empty element is "".
    munit_assert_null(hes_query_elem(ctx, "file.noSuchElement"));
    munit_assert_null(hes_query_elem(NULL, "file"));

    hes_query_elem_t* empty = hes_query_elem(ctx, "file.empty");
    munit_assert_not_null(empty);
    munit_assert_string_equal(hes_query_elem_text(empty), "");
    hes_query_elem_free(empty);

    // Attribute present-but-empty vs absent.
    hes_query_elem_t* ea = hes_query_elem(ctx, "file.emptyAttr");
    munit_assert_not_null(ea);
    const char* code = hes_query_elem_attr(ea, "code");
    munit_assert_not_null(code);
    munit_assert_char(code[0], ==, '\0');
    munit_assert_null(hes_query_elem_attr(ea, "missing"));
    hes_query_elem_free(ea);

    hes_query_elem_free(NULL);  // must not crash

    hes_query_close(ctx);
    return MUNIT_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// elem_list: document order, NULL termination, and list position == index.
static MunitResult test_element_lists(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    hes_query_ctx_t* ctx = hes_query_open(FIXTURE);
    munit_assert_not_null(ctx);

    size_t n = 0;
    hes_query_elem_t** list = hes_query_elem_list(ctx, OT0_DATA, &n);
    munit_assert_not_null(list);
    munit_assert_size(n, ==, 2);
    munit_assert_null(list[n]);  // NULL-terminated
    munit_assert_string_equal(hes_query_elem_text(list[0]), "1");
    munit_assert_string_equal(hes_query_elem_text(list[1]), "gt");

    // The documented invariant: list position answers the same element as the
    // same index in the path language.
    munit_assert_string_equal(hes_query_elem_text(list[0]), hes_query_string(ctx, OT0_DATA "[0]"));
    munit_assert_string_equal(hes_query_elem_text(list[1]), hes_query_string(ctx, OT0_DATA "[1]"));
    hes_query_elem_list_free(list);

    // Repeated across the tree, the path walks all of them in document order,
    // and stops at each level's own children (it does not descend into
    // <inputs> or <addressingTable>).
    const char* all = "file.overallProcess.lexiconType.objectType.operationTable.data";
    list = hes_query_elem_list(ctx, all, &n);
    munit_assert_not_null(list);
    munit_assert_size(n, ==, 5);
    munit_assert_string_equal(hes_query_elem_text(list[0]), "1");
    munit_assert_string_equal(hes_query_elem_text(list[1]), "gt");
    munit_assert_string_equal(hes_query_elem_text(list[2]), "2");
    munit_assert_string_equal(hes_query_elem_text(list[3]), "lt");
    munit_assert_string_equal(hes_query_elem_text(list[4]), "ad");
    hes_query_elem_list_free(list);

    // No match is NULL with a zero count, not an allocated empty array.
    list = hes_query_elem_list(ctx, "file.noSuchElement", &n);
    munit_assert_null(list);
    munit_assert_size(n, ==, 0);

    list = hes_query_elem_list(NULL, "file", &n);
    munit_assert_null(list);
    munit_assert_size(n, ==, 0);

    list = hes_query_elem_list(ctx, NULL, &n);
    munit_assert_null(list);
    munit_assert_size(n, ==, 0);

    hes_query_elem_list_free(NULL);  // must not crash

    hes_query_close(ctx);
    return MUNIT_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// Navigation: direct children vs any depth.
static MunitResult test_navigation(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    hes_query_ctx_t* ctx = hes_query_open(FIXTURE);
    munit_assert_not_null(ctx);

    hes_query_elem_t* obj = hes_query_elem(ctx, "file.overallProcess.lexiconType[0].objectType[0]");
    munit_assert_not_null(obj);

    // The objectType has no <data> of its own: the ones in this fixture sit
    // inside its operationTable/inputs, i.e. they are descendants.
    size_t n = 0;
    hes_query_elem_t** kids = hes_query_elem_children(obj, "data", &n);
    munit_assert_null(kids);
    munit_assert_size(n, ==, 0);

    // Its direct children of interest, in document order.
    kids = hes_query_elem_children(obj, "operationTable", &n);
    munit_assert_not_null(kids);
    munit_assert_size(n, ==, 2);
    munit_assert_string_equal(hes_query_elem_attr(kids[0], "transCode"), "ot1");
    munit_assert_string_equal(hes_query_elem_attr(kids[1], "transCode"), "ot2");
    munit_assert_null(kids[n]);
    hes_query_elem_list_free(kids);

    // <inputs> is a child of the operationTable, and the nested <data> is a
    // child of that -- children() only ever looks one level down.
    hes_query_elem_t* ot = hes_query_elem(ctx, OT0);
    munit_assert_not_null(ot);
    kids = hes_query_elem_children(ot, "inputs", &n);
    munit_assert_not_null(kids);
    munit_assert_size(n, ==, 1);
    hes_query_elem_t** inner = hes_query_elem_children(kids[0], "data", &n);
    munit_assert_not_null(inner);
    munit_assert_size(n, ==, 1);
    munit_assert_string_equal(hes_query_elem_text(inner[0]), "6");
    hes_query_elem_list_free(inner);
    hes_query_elem_list_free(kids);
    hes_query_elem_free(ot);
    hes_query_elem_free(obj);

    munit_assert_null(hes_query_elem_children(NULL, "data", &n));
    munit_assert_size(n, ==, 0);
    munit_assert_null(hes_query_elem_children(NULL, NULL, &n));
    munit_assert_size(n, ==, 0);

    // find_all reaches every depth, in document order: the 7 <data> elements
    // include the ones nested in <inputs> and <addressingTable>.
    hes_query_elem_t** found = hes_query_find_all(ctx, "data", &n);
    munit_assert_not_null(found);
    munit_assert_size(n, ==, 7);
    munit_assert_string_equal(hes_query_elem_text(found[0]), "1");
    munit_assert_string_equal(hes_query_elem_text(found[1]), "gt");
    munit_assert_string_equal(hes_query_elem_text(found[2]), "6");  // inside <inputs>
    munit_assert_string_equal(hes_query_elem_attr(found[2], "descriptiveName"), "deviceIndex");
    munit_assert_string_equal(hes_query_elem_text(found[5]), "6");   // inside <addressingTable>
    munit_assert_string_equal(hes_query_elem_text(found[6]), "ad");  // deepest level
    hes_query_elem_list_free(found);

    // The same tables that bm.c looks for, at two different depths.
    found = hes_query_find_all(ctx, "operationTable", &n);
    munit_assert_not_null(found);
    munit_assert_size(n, ==, 3);
    hes_query_elem_list_free(found);

    found = hes_query_find_all(ctx, "noSuchElement", &n);
    munit_assert_null(found);
    munit_assert_size(n, ==, 0);

    found = hes_query_find_all(ctx, "", &n);
    munit_assert_null(found);
    munit_assert_size(n, ==, 0);

    found = hes_query_find_all(NULL, "data", &n);
    munit_assert_null(found);
    munit_assert_size(n, ==, 0);

    hes_query_close(ctx);
    return MUNIT_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// Regressions for the translator's old fixed-size segment buffer: a segment
/// longer than that buffer, and a path made of many segments, must both still
/// resolve. A truncating translator would query a different element and return
/// NULL here.
static MunitResult test_no_truncation(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    enum { NAME_LEN = 400, DEPTH = 60 };

    // One element whose name alone exceeds the old 320-byte buffer.
    char name[NAME_LEN + 1];
    memset(name, 'n', NAME_LEN);
    name[NAME_LEN] = '\0';

    char* xml = (char*)malloc(2 * NAME_LEN + 64);
    munit_assert_not_null(xml);
    snprintf(xml, 2 * NAME_LEN + 64, "<?xml version=\"1.0\"?><file><%s>long-name</%s></file>", name,
             name);

    char* file = write_temp(xml);
    free(xml);
    munit_assert_not_null(file);

    hes_query_ctx_t* ctx = hes_query_open(file);
    unlink(file);
    free(file);
    munit_assert_not_null(ctx);

    char* path = (char*)malloc(NAME_LEN + 8);
    munit_assert_not_null(path);
    snprintf(path, NAME_LEN + 8, "file.%s", name);
    munit_assert_not_null(hes_query_string(ctx, path));
    munit_assert_string_equal(hes_query_string(ctx, path), "long-name");
    free(path);
    hes_query_close(ctx);

    // A 60-segment path, and the same elements reachable by children/find_all.
    size_t cap = (size_t)DEPTH * 8 + 64;
    xml = (char*)malloc(cap);
    munit_assert_not_null(xml);
    size_t used = (size_t)snprintf(xml, cap, "<?xml version=\"1.0\"?><file>");
    for (int i = 0; i < DEPTH; i++) {
        used += (size_t)snprintf(xml + used, cap - used, "<n>");
    }
    used += (size_t)snprintf(xml + used, cap - used, "bottom");
    for (int i = 0; i < DEPTH; i++) {
        used += (size_t)snprintf(xml + used, cap - used, "</n>");
    }
    snprintf(xml + used, cap - used, "</file>");

    file = write_temp(xml);
    free(xml);
    munit_assert_not_null(file);
    ctx = hes_query_open(file);
    unlink(file);
    free(file);
    munit_assert_not_null(ctx);

    path = (char*)malloc((size_t)DEPTH * 2 + 8);
    munit_assert_not_null(path);
    strcpy(path, "file");
    for (int i = 0; i < DEPTH; i++) {
        strcat(path, ".n");
    }
    munit_assert_not_null(hes_query_string(ctx, path));
    munit_assert_string_equal(hes_query_string(ctx, path), "bottom");
    free(path);

    // Only the outermost <n> is a child of <file>; all 60 are descendants.
    size_t n = 0;
    hes_query_elem_t* root = hes_query_elem(ctx, "file");
    munit_assert_not_null(root);
    hes_query_elem_t** kids = hes_query_elem_children(root, "n", &n);
    munit_assert_not_null(kids);
    munit_assert_size(n, ==, 1);
    hes_query_elem_list_free(kids);
    hes_query_elem_free(root);

    hes_query_elem_t** deep = hes_query_find_all(ctx, "n", &n);
    munit_assert_not_null(deep);
    munit_assert_size(n, ==, DEPTH);
    hes_query_elem_list_free(deep);

    hes_query_close(ctx);
    return MUNIT_OK;
}

static MunitTest hes_query_tests[] = {
  { (char*)"/open-failures", test_open_failures, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/scalars", test_scalars, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/element-handles", test_element_handles, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/element-lists", test_element_lists, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/navigation", test_navigation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/no-truncation", test_no_truncation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite hes_query_suite = {
  (char*)"/hes-query", hes_query_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[])
{
  return munit_suite_main(&hes_query_suite, NULL, argc, argv);
}
