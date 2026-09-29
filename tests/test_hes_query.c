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
/// Fixture: tests/hes_query_sample.xml. ctest runs this from the tests source
/// directory (see tests/CMakeLists.txt), so the path needs no argv.
///
/// Build/run: see tests/CMakeLists.txt (ctest).
#include "common/hes_query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#define FIXTURE "hes_query_sample.xml"

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
static int test_open_failures(void)
{
    hes_query_close(NULL);  // must not crash

    CHECK(hes_query_open(NULL) == NULL);
    CHECK(hes_query_open("/nonexistent/hes_query_sample.xml") == NULL);
    CHECK(hes_query_open("/tmp") == NULL);  // a directory, not a document

    // A NULL context is answered, not dereferenced, by every entry point.
    CHECK(hes_query_string(NULL, "file") == NULL);
    CHECK(hes_query_count(NULL, "file") == 0);
    CHECK(hes_query_elem(NULL, "file") == NULL);
    CHECK(hes_query_long(NULL, "file", 7) == 7);

    // Malformed XML, and a well-formed document with no root element: the
    // first fails in libxml2, the second has nothing a path could address.
    char* path = write_temp("<?xml version=\"1.0\"?>\n");
    CHECK(path != NULL);
    CHECK(hes_query_open(path) == NULL);
    unlink(path);
    free(path);

    path = write_temp("<file><unclosed></file>\n");
    CHECK(path != NULL);
    CHECK(hes_query_open(path) == NULL);
    unlink(path);
    free(path);

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Path language + scalar accessors, and "missing" vs "empty".
static int test_scalars(void)
{
    hes_query_ctx_t* ctx = hes_query_open(FIXTURE);
    CHECK(ctx != NULL);

    // element text, text(), attribute, '/' as separator as well as '.'
    CHECK(strcmp(hes_query_string(ctx, "file.fileFormat"), "HES") == 0);
    CHECK(strcmp(hes_query_string(ctx, "file.fileFormat.text()"), "HES") == 0);
    CHECK(strcmp(hes_query_string(ctx, "file.@transCode"), "lx") == 0);
    CHECK(strcmp(hes_query_string(ctx, "file/overallProcess/lexiconType[0]/@transCode"), "l1") ==
          0);
    CHECK(strcmp(hes_query_string(ctx, "file.overallProcess.lexiconType[1].@transCode"), "l2") ==
          0);

    // 0-based index reaches the right repeated element ...
    CHECK(strcmp(hes_query_string(ctx, OT0 ".@transCode"), "ot1") == 0);
    CHECK(strcmp(hes_query_string(ctx,
                                  "file.overallProcess.lexiconType[0].objectType[0]."
                                  "operationTable[1].@transCode"),
                 "ot2") == 0);
    CHECK(strcmp(hes_query_string(ctx, OT0 ".data[1]"), "gt") == 0);
    // ... and a path without an index selects the first match.
    CHECK(strcmp(hes_query_string(ctx, "file.overallProcess.lexiconType.objectType.@transCode"),
                 "o1") == 0);
    // An index past the end is simply no match.
    CHECK(hes_query_string(ctx, "file.overallProcess.lexiconType[2]") == NULL);

    // Missing element / missing attribute: NULL, and NOT an error.
    CHECK(hes_query_string(ctx, "file.noSuchElement") == NULL);
    CHECK(hes_query_string(ctx, "file.fileFormat.@nope") == NULL);
    CHECK(hes_query_last_error(ctx)[0] == '\0');

    // Present but empty, in all three shapes: NULL must not be returned.
    const char* emptyText = hes_query_string(ctx, "file.empty");
    CHECK(emptyText != NULL && emptyText[0] == '\0');
    const char* blankText = hes_query_string(ctx, "file.blank");
    CHECK(blankText != NULL && blankText[0] == '\0');  // whitespace-only text
    const char* emptyAttr = hes_query_string(ctx, "file.emptyAttr.@code");
    CHECK(emptyAttr != NULL && emptyAttr[0] == '\0');

    // A malformed path is an error, and reports one.
    CHECK(hes_query_string(ctx, "file.[") == NULL);
    CHECK(hes_query_last_error(ctx)[0] != '\0');
    // A later successful query clears it again.
    CHECK(hes_query_string(ctx, "file.fileFormat") != NULL);
    CHECK(hes_query_last_error(ctx)[0] == '\0');

    // Scalars: parsed values, and the caller's default whenever the value is
    // missing, empty or not of that type.
    CHECK(hes_query_long(ctx, OT0_DATA "[0]", -1) == 1);
    CHECK(hes_query_long(ctx, "file.fileFormat", -1) == -1);
    CHECK(hes_query_long(ctx, "file.noSuchElement", 42) == 42);
    CHECK(hes_query_long(ctx, "file.empty", -1) == -1);
    CHECK(hes_query_double(ctx, OT0_DATA "[0]", -1.0) == 1.0);
    CHECK(hes_query_double(ctx, "file.fileFormat", -1.0) == -1.0);
    CHECK(hes_query_bool(ctx, OT0_DATA "[0]", 0) == 1);     // "1"
    CHECK(hes_query_bool(ctx, "file.fileFormat", 5) == 5);  // not a boolean
    CHECK(hes_query_bool(ctx, "file.noSuchElement", 5) == 5);

    // count(): all matches when no index is given, 0 when nothing matches.
    CHECK(hes_query_count(ctx, "file.overallProcess.lexiconType") == 2);
    CHECK(hes_query_count(ctx, "file.overallProcess.lexiconType[0].objectType.operationTable") ==
          2);
    CHECK(hes_query_count(ctx, "file.overallProcess.lexiconType[1].objectType.operationTable") ==
          1);
    CHECK(hes_query_count(ctx, "file.*") == 5);  // wildcard: 5 child elements
    CHECK(hes_query_count(ctx, "file.noSuchElement") == 0);
    CHECK(hes_query_count(NULL, "file") == 0);

    hes_query_close(ctx);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Element handles: name, text, attributes, and relative queries.
static int test_element_handles(void)
{
    hes_query_ctx_t* ctx = hes_query_open(FIXTURE);
    CHECK(ctx != NULL);

    hes_query_elem_t* ot = hes_query_elem(ctx, OT0);
    CHECK(ot != NULL);
    CHECK(strcmp(hes_query_elem_name(ot), "operationTable") == 0);
    CHECK(strcmp(hes_query_elem_attr(ot, "transCode"), "ot1") == 0);
    CHECK(strcmp(hes_query_elem_attr(ot, "descriptiveName"), "operationTable") == 0);

    // Relative paths: same 0-based indexing, rooted at this element.
    CHECK(strcmp(hes_query_elem_string(ot, "data[0]"), "1") == 0);
    CHECK(strcmp(hes_query_elem_string(ot, "data[1].@descriptiveName"), "operation") == 0);
    CHECK(strcmp(hes_query_elem_string(ot, "inputs.data[0]"), "6") == 0);
    CHECK(hes_query_elem_string(ot, "data[9]") == NULL);
    CHECK(hes_query_elem_string(ot, "@nope") == NULL);
    CHECK(hes_query_elem_string(NULL, "data") == NULL);
    CHECK(hes_query_elem_string(ot, NULL) == NULL);

    hes_query_elem_free(ot);

    // Missing path -> no handle; a handle's text of an empty element is "".
    CHECK(hes_query_elem(ctx, "file.noSuchElement") == NULL);
    CHECK(hes_query_elem(NULL, "file") == NULL);

    hes_query_elem_t* empty = hes_query_elem(ctx, "file.empty");
    CHECK(empty != NULL);
    CHECK(strcmp(hes_query_elem_text(empty), "") == 0);
    hes_query_elem_free(empty);

    // Attribute present-but-empty vs absent.
    hes_query_elem_t* ea = hes_query_elem(ctx, "file.emptyAttr");
    CHECK(ea != NULL);
    const char* code = hes_query_elem_attr(ea, "code");
    CHECK(code != NULL && code[0] == '\0');
    CHECK(hes_query_elem_attr(ea, "missing") == NULL);
    hes_query_elem_free(ea);

    hes_query_elem_free(NULL);  // must not crash

    hes_query_close(ctx);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// elem_list: document order, NULL termination, and list position == index.
static int test_element_lists(void)
{
    hes_query_ctx_t* ctx = hes_query_open(FIXTURE);
    CHECK(ctx != NULL);

    size_t n = 0;
    hes_query_elem_t** list = hes_query_elem_list(ctx, OT0_DATA, &n);
    CHECK(list != NULL && n == 2);
    CHECK(list[n] == NULL);  // NULL-terminated
    CHECK(strcmp(hes_query_elem_text(list[0]), "1") == 0);
    CHECK(strcmp(hes_query_elem_text(list[1]), "gt") == 0);

    // The documented invariant: list position answers the same element as the
    // same index in the path language.
    CHECK(strcmp(hes_query_elem_text(list[0]), hes_query_string(ctx, OT0_DATA "[0]")) == 0);
    CHECK(strcmp(hes_query_elem_text(list[1]), hes_query_string(ctx, OT0_DATA "[1]")) == 0);
    hes_query_elem_list_free(list);

    // Repeated across the tree, the path walks all of them in document order,
    // and stops at each level's own children (it does not descend into
    // <inputs> or <addressingTable>).
    const char* all = "file.overallProcess.lexiconType.objectType.operationTable.data";
    list = hes_query_elem_list(ctx, all, &n);
    CHECK(list != NULL && n == 5);
    CHECK(strcmp(hes_query_elem_text(list[0]), "1") == 0);
    CHECK(strcmp(hes_query_elem_text(list[1]), "gt") == 0);
    CHECK(strcmp(hes_query_elem_text(list[2]), "2") == 0);
    CHECK(strcmp(hes_query_elem_text(list[3]), "lt") == 0);
    CHECK(strcmp(hes_query_elem_text(list[4]), "ad") == 0);
    hes_query_elem_list_free(list);

    // No match is NULL with a zero count, not an allocated empty array.
    list = hes_query_elem_list(ctx, "file.noSuchElement", &n);
    CHECK(list == NULL && n == 0);

    list = hes_query_elem_list(NULL, "file", &n);
    CHECK(list == NULL && n == 0);

    list = hes_query_elem_list(ctx, NULL, &n);
    CHECK(list == NULL && n == 0);

    hes_query_elem_list_free(NULL);  // must not crash

    hes_query_close(ctx);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Navigation: direct children vs any depth.
static int test_navigation(void)
{
    hes_query_ctx_t* ctx = hes_query_open(FIXTURE);
    CHECK(ctx != NULL);

    hes_query_elem_t* obj = hes_query_elem(ctx, "file.overallProcess.lexiconType[0].objectType[0]");
    CHECK(obj != NULL);

    // The objectType has no <data> of its own: the ones in this fixture sit
    // inside its operationTable/inputs, i.e. they are descendants.
    size_t n = 0;
    hes_query_elem_t** kids = hes_query_elem_children(obj, "data", &n);
    CHECK(kids == NULL && n == 0);

    // Its direct children of interest, in document order.
    kids = hes_query_elem_children(obj, "operationTable", &n);
    CHECK(kids != NULL && n == 2);
    CHECK(strcmp(hes_query_elem_attr(kids[0], "transCode"), "ot1") == 0);
    CHECK(strcmp(hes_query_elem_attr(kids[1], "transCode"), "ot2") == 0);
    CHECK(kids[n] == NULL);
    hes_query_elem_list_free(kids);

    // <inputs> is a child of the operationTable, and the nested <data> is a
    // child of that -- children() only ever looks one level down.
    hes_query_elem_t* ot = hes_query_elem(ctx, OT0);
    CHECK(ot != NULL);
    kids = hes_query_elem_children(ot, "inputs", &n);
    CHECK(kids != NULL && n == 1);
    hes_query_elem_t** inner = hes_query_elem_children(kids[0], "data", &n);
    CHECK(inner != NULL && n == 1);
    CHECK(strcmp(hes_query_elem_text(inner[0]), "6") == 0);
    hes_query_elem_list_free(inner);
    hes_query_elem_list_free(kids);
    hes_query_elem_free(ot);
    hes_query_elem_free(obj);

    CHECK(hes_query_elem_children(NULL, "data", &n) == NULL && n == 0);
    CHECK(hes_query_elem_children(NULL, NULL, &n) == NULL && n == 0);

    // find_all reaches every depth, in document order: the 7 <data> elements
    // include the ones nested in <inputs> and <addressingTable>.
    hes_query_elem_t** found = hes_query_find_all(ctx, "data", &n);
    CHECK(found != NULL && n == 7);
    CHECK(strcmp(hes_query_elem_text(found[0]), "1") == 0);
    CHECK(strcmp(hes_query_elem_text(found[1]), "gt") == 0);
    CHECK(strcmp(hes_query_elem_text(found[2]), "6") == 0);  // inside <inputs>
    CHECK(strcmp(hes_query_elem_attr(found[2], "descriptiveName"), "deviceIndex") == 0);
    CHECK(strcmp(hes_query_elem_text(found[5]), "6") == 0);   // inside <addressingTable>
    CHECK(strcmp(hes_query_elem_text(found[6]), "ad") == 0);  // deepest level
    hes_query_elem_list_free(found);

    // The same tables that bm.c looks for, at two different depths.
    found = hes_query_find_all(ctx, "operationTable", &n);
    CHECK(found != NULL && n == 3);
    hes_query_elem_list_free(found);

    found = hes_query_find_all(ctx, "noSuchElement", &n);
    CHECK(found == NULL && n == 0);

    found = hes_query_find_all(ctx, "", &n);
    CHECK(found == NULL && n == 0);

    found = hes_query_find_all(NULL, "data", &n);
    CHECK(found == NULL && n == 0);

    hes_query_close(ctx);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Regressions for the translator's old fixed-size segment buffer: a segment
/// longer than that buffer, and a path made of many segments, must both still
/// resolve. A truncating translator would query a different element and return
/// NULL here.
static int test_no_truncation(void)
{
    enum { NAME_LEN = 400, DEPTH = 60 };

    // One element whose name alone exceeds the old 320-byte buffer.
    char name[NAME_LEN + 1];
    memset(name, 'n', NAME_LEN);
    name[NAME_LEN] = '\0';

    char* xml = (char*)malloc(2 * NAME_LEN + 64);
    CHECK(xml != NULL);
    snprintf(xml, 2 * NAME_LEN + 64, "<?xml version=\"1.0\"?><file><%s>long-name</%s></file>", name,
             name);

    char* file = write_temp(xml);
    free(xml);
    CHECK(file != NULL);

    hes_query_ctx_t* ctx = hes_query_open(file);
    unlink(file);
    free(file);
    CHECK(ctx != NULL);

    char* path = (char*)malloc(NAME_LEN + 8);
    CHECK(path != NULL);
    snprintf(path, NAME_LEN + 8, "file.%s", name);
    CHECK(hes_query_string(ctx, path) != NULL);
    CHECK(strcmp(hes_query_string(ctx, path), "long-name") == 0);
    free(path);
    hes_query_close(ctx);

    // A 60-segment path, and the same elements reachable by children/find_all.
    size_t cap = (size_t)DEPTH * 8 + 64;
    xml = (char*)malloc(cap);
    CHECK(xml != NULL);
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
    CHECK(file != NULL);
    ctx = hes_query_open(file);
    unlink(file);
    free(file);
    CHECK(ctx != NULL);

    path = (char*)malloc((size_t)DEPTH * 2 + 8);
    CHECK(path != NULL);
    strcpy(path, "file");
    for (int i = 0; i < DEPTH; i++) {
        strcat(path, ".n");
    }
    CHECK(hes_query_string(ctx, path) != NULL);
    CHECK(strcmp(hes_query_string(ctx, path), "bottom") == 0);
    free(path);

    // Only the outermost <n> is a child of <file>; all 60 are descendants.
    size_t n = 0;
    hes_query_elem_t* root = hes_query_elem(ctx, "file");
    CHECK(root != NULL);
    hes_query_elem_t** kids = hes_query_elem_children(root, "n", &n);
    CHECK(kids != NULL && n == 1);
    hes_query_elem_list_free(kids);
    hes_query_elem_free(root);

    hes_query_elem_t** deep = hes_query_find_all(ctx, "n", &n);
    CHECK(deep != NULL && n == DEPTH);
    hes_query_elem_list_free(deep);

    hes_query_close(ctx);
    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_open_failures();
    failures += test_scalars();
    failures += test_element_handles();
    failures += test_element_lists();
    failures += test_navigation();
    failures += test_no_truncation();

    if (failures != 0) {
        fprintf(stderr, "test_hes_query: %d test group(s) failed\n", failures);
        return 1;
    }

    printf("test_hes_query: ok\n");
    return 0;
}
