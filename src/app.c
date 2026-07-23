#include <nng/nng.h>
#include <nng/mqtt/mqtt_client.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "hes_bm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MQTT_URL "mqtt-tcp://127.0.0.1:1883"

static void test_hes_bm_api() {
    printf("========== Testing HES Binding Map API ==========\n\n");

    // Load the binding map XML file
    hes_bm_doc_t *doc = hes_bm_load("../tests/hes_gateway_product.xml");
    if (doc == NULL) {
        printf("Failed to load XML file\n");
        return;
    }

    // Get the root element
    hes_bm_node_t *root = hes_bm_get_root(doc);
    if (root == NULL) {
        printf("Failed to get root element\n");
        hes_bm_free(doc);
        return;
    }

    printf("Root element: %s\n", hes_bm_get_name(root));
    printf("\n--- File Metadata ---\n");

    // Query file metadata elements
    hes_bm_node_t *node = NULL;
    const char *metadata[] = {"fileFormat", "fileType", "filePurpose", "fileVersion", NULL};

    for (int i = 0; metadata[i] != NULL; i++) {
        node = hes_bm_find_child(root, metadata[i]);
        if (node != NULL) {
            const char *value = hes_bm_get_text(node);
            printf("%s: %s\n", metadata[i], value ? value : "");
            free(node);
        }
    }

    printf("\n--- Overall Process ---\n");

    // Find overallProcess element
    hes_bm_node_t *overall = hes_bm_find_child(root, "overallProcess");
    if (overall != NULL) {
        const char *version = hes_bm_get_attr(overall, "versionNumber");
        const char *desc = hes_bm_get_attr(overall, "descriptiveName");
        printf("overallProcess version=%s descriptiveName=%s\n", version, desc);

        // Find lexiconType
        hes_bm_node_t *lexicon = hes_bm_find_child(overall, "lexiconType");
        if (lexicon != NULL) {
            printf("  Found lexiconType\n");

            // Find objectType (bindingMap)
            hes_bm_node_t *obj_type = hes_bm_find_child(lexicon, "objectType");
            if (obj_type != NULL) {
                const char *obj_desc = hes_bm_get_attr(obj_type, "descriptiveName");
                printf("    Found objectType: %s\n", obj_desc);

                printf("\n--- Binding Map Data Elements ---\n");

                // Find all direct data children
                int data_count = 0;
                hes_bm_node_t **data_nodes = hes_bm_find_all_children(obj_type, "data", &data_count);

                printf("Found %d direct data elements:\n", data_count);
                for (int i = 0; i < data_count; i++) {
                    const char *trans_code = hes_bm_get_attr(data_nodes[i], "transCode");
                    const char *desc_name = hes_bm_get_attr(data_nodes[i], "descriptiveName");
                    const char *text = hes_bm_get_text(data_nodes[i]);
                    printf("  [%s] %s = %s\n", trans_code, desc_name, text ? text : "");
                    free(data_nodes[i]);
                }
                if (data_nodes) free(data_nodes);

                printf("\n--- Operation Tables ---\n");

                // Find operationTable
                hes_bm_node_t *op_table = hes_bm_find_child(obj_type, "operationTable");
                if (op_table != NULL) {
                    const char *op_desc = hes_bm_get_attr(op_table, "descriptiveName");
                    printf("Found operationTable: %s\n", op_desc);

                    // Find inputs
                    hes_bm_node_t *inputs = hes_bm_find_child(op_table, "inputs");
                    if (inputs != NULL) {
                        int input_count = 0;
                        hes_bm_node_t **input_nodes = hes_bm_find_all_children(inputs, "data", &input_count);
                        printf("  Inputs: %d data elements\n", input_count);
                        for (int i = 0; i < input_count; i++) {
                            const char *name = hes_bm_get_attr(input_nodes[i], "descriptiveName");
                            printf("    - %s\n", name);
                            free(input_nodes[i]);
                        }
                        if (input_nodes) free(input_nodes);
                        free(inputs);
                    }

                    // Find operation element
                    hes_bm_node_t *operation = hes_bm_find_child(op_table, "operation");
                    if (operation != NULL) {
                        const char *op_value = hes_bm_get_text(operation);
                        printf("  Operation: %s\n", op_value ? op_value : "");
                        free(operation);
                    }

                    // Find addressing tables
                    int addr_count = 0;
                    hes_bm_node_t **addr_tables = hes_bm_find_all_children(obj_type, "addressingTable", &addr_count);
                    printf("\n--- Addressing Tables ---\n");
                    printf("Found %d addressing tables:\n", addr_count);

                    for (int i = 0; i < addr_count; i++) {
                        const char *trans = hes_bm_get_attr(addr_tables[i], "transCode");
                        printf("  Addressing Table [%s]:\n", trans);

                        // Find data elements in this addressing table
                        int addr_data_count = 0;
                        hes_bm_node_t **addr_data = hes_bm_find_all_children(addr_tables[i], "data", &addr_data_count);
                        for (int j = 0; j < addr_data_count; j++) {
                            const char *name = hes_bm_get_attr(addr_data[j], "descriptiveName");
                            const char *text = hes_bm_get_text(addr_data[j]);
                            printf("    %s = %s\n", name, text ? text : "");
                            free(addr_data[j]);
                        }
                        if (addr_data) free(addr_data);
                        free(addr_tables[i]);
                    }
                    if (addr_tables) free(addr_tables);

                    free(op_table);
                }

                free(obj_type);
            }

            free(lexicon);
        }

        free(overall);
    }

    free(root);
    hes_bm_free(doc);

    printf("\n========== HES BM API Test Complete ==========\n");
}

int main() {
    // Initialize the XML library once at startup
    xmlInitParser();

    printf("Starting HES Gateway Server\n\n");

    // Test the HES Binding Map API
    test_hes_bm_api();

    printf("\nXML tests completed!\n");

    // Uncomment MQTT code after XML is working
    /*
    nng_socket sock;
    nng_msg *msg;

    // 1. Initialize the MQTT client socket
    nng_mqtt_client_open(&sock);

    // 2. Create the connection message (Connect to broker)
    nng_msg_alloc(&msg, 0);
    nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_client_id(msg, "nano_c_client");

    // 3. Dial the broker (Connect phase)
    nng_dialer dialer;
    nng_dialer_create(&dialer, sock, MQTT_URL);
    nng_dialer_start(dialer, NNG_FLAG_ALLOC);

    // 4. Create and send a PUBLISH message
    nng_msg *pubmsg;
    nng_msg_alloc(&pubmsg, 0);
    nng_mqtt_msg_set_packet_type(pubmsg, NNG_MQTT_PUBLISH);
    nng_mqtt_msg_set_publish_topic(pubmsg, "test/topic");
    nng_mqtt_msg_set_publish_payload(pubmsg, (uint8_t *)"Hello NanoMQ!", strlen("Hello NanoMQ!"));
    nng_mqtt_msg_set_publish_qos(pubmsg, 0);

    nng_sendmsg(sock, pubmsg, 0);

    // 5. Clean up resources
    nng_close(sock);
    */

    // Cleanup the XML library once at shutdown
    xmlCleanupParser();

    return 0;
}
