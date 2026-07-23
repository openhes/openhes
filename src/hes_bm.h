#ifndef HES_BM_H
#define HES_BM_H

#include <libxml/parser.h>
#include <libxml/tree.h>

/**
 * HES Binding Map (BM) Module
 *
 * This module provides APIs to parse and query HES binding map XML files.
 * A binding map describes the structure of data flows and operations in the HES.
 */

/** Opaque handle to a BM document */
typedef struct hes_bm_doc hes_bm_doc_t;

/** Opaque handle to a BM node (XML element) */
typedef struct hes_bm_node hes_bm_node_t;

/**
 * Parse an HES binding map XML file.
 *
 * @param filepath Path to the XML file
 * @return Document handle on success, NULL on failure
 */
hes_bm_doc_t* hes_bm_load(const char *filepath);

/**
 * Free a BM document and all associated resources.
 *
 * @param doc Document handle
 */
void hes_bm_free(hes_bm_doc_t *doc);

/**
 * Get the root node of the document (typically the <file> element).
 *
 * @param doc Document handle
 * @return Root node handle, NULL on failure
 */
hes_bm_node_t* hes_bm_get_root(hes_bm_doc_t *doc);

/**
 * Get the first child node with the specified name.
 *
 * @param node Parent node
 * @param name Element name to search for
 * @return Child node handle, NULL if not found
 */
hes_bm_node_t* hes_bm_find_child(hes_bm_node_t *node, const char *name);

/**
 * Get the next sibling node with the specified name.
 *
 * @param node Current node
 * @param name Element name to search for
 * @return Next sibling node with the given name, NULL if not found
 */
hes_bm_node_t* hes_bm_find_next(hes_bm_node_t *node, const char *name);

/**
 * Get all child nodes with the specified name.
 * Returns a dynamically allocated array of node pointers.
 * The array is terminated with a NULL pointer.
 *
 * @param node Parent node
 * @param name Element name to search for
 * @param count Output parameter: number of nodes found (not including NULL terminator)
 * @return Array of node handles, must be freed with free()
 */
hes_bm_node_t** hes_bm_find_all_children(hes_bm_node_t *node, const char *name, int *count);

/**
 * Get the element name of a node.
 *
 * @param node Node handle
 * @return Element name (e.g., "data", "operationTable"), NULL on failure
 */
const char* hes_bm_get_name(hes_bm_node_t *node);

/**
 * Get text content of a node.
 * Returns the text content directly under this node.
 *
 * @param node Node handle
 * @return Text content, NULL if no text content or on failure
 */
const char* hes_bm_get_text(hes_bm_node_t *node);

/**
 * Get an attribute value by name.
 *
 * @param node Node handle
 * @param attr_name Attribute name (e.g., "versionNumber", "descriptiveName")
 * @return Attribute value, NULL if not found
 */
const char* hes_bm_get_attr(hes_bm_node_t *node, const char *attr_name);

/**
 * Get all attributes of a node.
 * Returns a dynamically allocated array of attribute names.
 * The array is terminated with a NULL pointer.
 *
 * @param node Node handle
 * @param count Output parameter: number of attributes found
 * @return Array of attribute names, must be freed with free()
 */
const char** hes_bm_get_all_attrs(hes_bm_node_t *node, int *count);

/**
 * Get the first child node (any name).
 *
 * @param node Parent node
 * @return First child node, NULL if no children
 */
hes_bm_node_t* hes_bm_get_first_child(hes_bm_node_t *node);

/**
 * Get the next sibling node (any name).
 *
 * @param node Current node
 * @return Next sibling node, NULL if this is the last sibling
 */
hes_bm_node_t* hes_bm_get_next_sibling(hes_bm_node_t *node);

/**
 * Iterate through all child nodes with a given name.
 * Convenience macro for common iteration patterns.
 *
 * Usage:
 *   hes_bm_node_t *child;
 *   hes_bm_for_each_child(parent, "data", child) {
 *       // Process child
 *   }
 */
#define hes_bm_for_each_child(parent, name, node) \
    for ((node) = hes_bm_find_child((parent), (name)); \
         (node) != NULL; \
         (node) = hes_bm_find_next((node), (name)))

/**
 * Pretty-print node information for debugging.
 *
 * @param node Node to print
 */
void hes_bm_print_node(hes_bm_node_t *node);

/**
 * Pretty-print the entire tree structure (useful for debugging).
 *
 * @param node Root node to start printing from
 * @param indent Current indentation level (use 0 for initial call)
 */
void hes_bm_print_tree(hes_bm_node_t *node, int indent);

#endif /* HES_BM_H */
