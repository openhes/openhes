#include "hes_bm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/** Simple wrapper around xmlDocPtr */
struct hes_bm_doc {
    xmlDocPtr xml_doc;
};

/** Simple wrapper around xmlNodePtr */
struct hes_bm_node {
    xmlNodePtr xml_node;
};

/**
 * Internal helper: Convert xmlNodePtr to hes_bm_node_t
 */
static hes_bm_node_t* _node_wrap(xmlNodePtr xml_node) {
    if (xml_node == NULL) return NULL;

    hes_bm_node_t *node = (hes_bm_node_t *)malloc(sizeof(hes_bm_node_t));
    if (node == NULL) return NULL;

    node->xml_node = xml_node;
    return node;
}

/**
 * Internal helper: Skip non-element nodes (text, comments, etc.)
 */
static xmlNodePtr _skip_non_elements(xmlNodePtr node) {
    while (node != NULL && node->type != XML_ELEMENT_NODE) {
        node = node->next;
    }
    return node;
}

hes_bm_doc_t* hes_bm_load(const char *filepath) {
    if (filepath == NULL) return NULL;

    // Parse the XML file
    xmlDocPtr xml_doc = xmlParseFile(filepath);
    if (xml_doc == NULL) {
        fprintf(stderr, "Error: Failed to parse XML file '%s'\n", filepath);
        return NULL;
    }

    hes_bm_doc_t *doc = (hes_bm_doc_t *)malloc(sizeof(hes_bm_doc_t));
    if (doc == NULL) {
        xmlFreeDoc(xml_doc);
        return NULL;
    }

    doc->xml_doc = xml_doc;
    return doc;
}

void hes_bm_free(hes_bm_doc_t *doc) {
    if (doc == NULL) return;

    if (doc->xml_doc != NULL) {
        xmlFreeDoc(doc->xml_doc);
    }
    free(doc);
}

hes_bm_node_t* hes_bm_get_root(hes_bm_doc_t *doc) {
    if (doc == NULL || doc->xml_doc == NULL) return NULL;

    xmlNodePtr root = xmlDocGetRootElement(doc->xml_doc);
    return _node_wrap(root);
}

hes_bm_node_t* hes_bm_find_child(hes_bm_node_t *node, const char *name) {
    if (node == NULL || node->xml_node == NULL || name == NULL) return NULL;

    xmlNodePtr child = _skip_non_elements(node->xml_node->children);
    while (child != NULL) {
        if (strcmp((const char *)child->name, name) == 0) {
            return _node_wrap(child);
        }
        child = _skip_non_elements(child->next);
    }

    return NULL;
}

hes_bm_node_t* hes_bm_find_next(hes_bm_node_t *node, const char *name) {
    if (node == NULL || node->xml_node == NULL || name == NULL) return NULL;

    xmlNodePtr sibling = _skip_non_elements(node->xml_node->next);
    while (sibling != NULL) {
        if (strcmp((const char *)sibling->name, name) == 0) {
            return _node_wrap(sibling);
        }
        sibling = _skip_non_elements(sibling->next);
    }

    return NULL;
}

hes_bm_node_t** hes_bm_find_all_children(hes_bm_node_t *node, const char *name, int *count) {
    if (node == NULL || node->xml_node == NULL || name == NULL || count == NULL) {
        if (count != NULL) *count = 0;
        return NULL;
    }

    // First pass: count matching children
    *count = 0;
    xmlNodePtr child = _skip_non_elements(node->xml_node->children);
    while (child != NULL) {
        if (strcmp((const char *)child->name, name) == 0) {
            (*count)++;
        }
        child = _skip_non_elements(child->next);
    }

    if (*count == 0) {
        return NULL;
    }

    // Allocate array (count + 1 for NULL terminator)
    hes_bm_node_t **result = (hes_bm_node_t **)malloc((*count + 1) * sizeof(hes_bm_node_t *));
    if (result == NULL) {
        *count = 0;
        return NULL;
    }

    // Second pass: populate array
    int idx = 0;
    child = _skip_non_elements(node->xml_node->children);
    while (child != NULL) {
        if (strcmp((const char *)child->name, name) == 0) {
            result[idx++] = _node_wrap(child);
        }
        child = _skip_non_elements(child->next);
    }
    result[idx] = NULL;  // NULL terminator

    return result;
}

const char* hes_bm_get_name(hes_bm_node_t *node) {
    if (node == NULL || node->xml_node == NULL) return NULL;
    return (const char *)node->xml_node->name;
}

const char* hes_bm_get_text(hes_bm_node_t *node) {
    if (node == NULL || node->xml_node == NULL) return NULL;

    // Look for text node children
    xmlNodePtr child = node->xml_node->children;
    while (child != NULL) {
        if (child->type == XML_TEXT_NODE && child->content != NULL) {
            // Skip whitespace-only text nodes
            const char *text = (const char *)child->content;
            while (*text && (*text == ' ' || *text == '\n' || *text == '\t' || *text == '\r')) {
                text++;
            }
            if (*text != '\0') {
                return (const char *)child->content;
            }
        }
        child = child->next;
    }

    return NULL;
}

const char* hes_bm_get_attr(hes_bm_node_t *node, const char *attr_name) {
    if (node == NULL || node->xml_node == NULL || attr_name == NULL) return NULL;

    xmlAttrPtr attr = node->xml_node->properties;
    while (attr != NULL) {
        if (strcmp((const char *)attr->name, attr_name) == 0) {
            if (attr->children && attr->children->content) {
                return (const char *)attr->children->content;
            }
            return "";  // Empty attribute
        }
        attr = attr->next;
    }

    return NULL;
}

const char** hes_bm_get_all_attrs(hes_bm_node_t *node, int *count) {
    if (node == NULL || node->xml_node == NULL || count == NULL) {
        if (count != NULL) *count = 0;
        return NULL;
    }

    // First pass: count attributes
    *count = 0;
    xmlAttrPtr attr = node->xml_node->properties;
    while (attr != NULL) {
        (*count)++;
        attr = attr->next;
    }

    if (*count == 0) {
        return NULL;
    }

    // Allocate array (count + 1 for NULL terminator)
    const char **result = (const char **)malloc((*count + 1) * sizeof(const char *));
    if (result == NULL) {
        *count = 0;
        return NULL;
    }

    // Second pass: populate array
    int idx = 0;
    attr = node->xml_node->properties;
    while (attr != NULL) {
        result[idx++] = (const char *)attr->name;
        attr = attr->next;
    }
    result[idx] = NULL;  // NULL terminator

    return result;
}

hes_bm_node_t* hes_bm_get_first_child(hes_bm_node_t *node) {
    if (node == NULL || node->xml_node == NULL) return NULL;

    xmlNodePtr child = _skip_non_elements(node->xml_node->children);
    return _node_wrap(child);
}

hes_bm_node_t* hes_bm_get_next_sibling(hes_bm_node_t *node) {
    if (node == NULL || node->xml_node == NULL) return NULL;

    xmlNodePtr sibling = _skip_non_elements(node->xml_node->next);
    return _node_wrap(sibling);
}

void hes_bm_print_node(hes_bm_node_t *node) {
    if (node == NULL || node->xml_node == NULL) return;

    printf("<%s", (const char *)node->xml_node->name);

    // Print attributes
    xmlAttrPtr attr = node->xml_node->properties;
    while (attr != NULL) {
        printf(" %s=\"", (const char *)attr->name);
        if (attr->children && attr->children->content) {
            printf("%s", (const char *)attr->children->content);
        }
        printf("\"");
        attr = attr->next;
    }

    // Print text content if any
    const char *text = hes_bm_get_text(node);
    if (text != NULL) {
        printf(">%s</%s>\n", text, (const char *)node->xml_node->name);
    } else {
        // Check if has children
        xmlNodePtr child = _skip_non_elements(node->xml_node->children);
        if (child != NULL) {
            printf("> ... </%s>\n", (const char *)node->xml_node->name);
        } else {
            printf(" />\n");
        }
    }
}

void hes_bm_print_tree(hes_bm_node_t *node, int indent) {
    if (node == NULL || node->xml_node == NULL) return;

    // Print indentation
    for (int i = 0; i < indent; i++) printf("  ");

    // Print opening tag with attributes
    printf("<%s", (const char *)node->xml_node->name);

    xmlAttrPtr attr = node->xml_node->properties;
    while (attr != NULL) {
        printf(" %s=\"", (const char *)attr->name);
        if (attr->children && attr->children->content) {
            printf("%s", (const char *)attr->children->content);
        }
        printf("\"");
        attr = attr->next;
    }

    // Check for text content
    const char *text = hes_bm_get_text(node);
    if (text != NULL) {
        printf(">%s</%s>\n", text, (const char *)node->xml_node->name);
    } else {
        // Check if has children
        xmlNodePtr child = _skip_non_elements(node->xml_node->children);
        if (child != NULL) {
            printf(">\n");

            // Recursively print children
            while (child != NULL) {
                hes_bm_node_t *child_node = _node_wrap(child);
                hes_bm_print_tree(child_node, indent + 1);
                free(child_node);
                child = _skip_non_elements(child->next);
            }

            // Print closing tag
            for (int i = 0; i < indent; i++) printf("  ");
            printf("</%s>\n", (const char *)node->xml_node->name);
        } else {
            printf(" />\n");
        }
    }
}
