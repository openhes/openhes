#include <nng/nng.h>
#include <nng/mqtt/mqtt_client.h>
#include <nng/supplemental/util/platform.h>
#include <stdio.h>

int main(void) {
    nng_socket sock;
    nng_dialer dialer;

    // MQTT v3.1.1/v5 client socket
    if (nng_mqtt_client_open(&sock) != 0) {
        printf("failed to open client socket\n");
        return 1;
    }

    nng_dialer_create(&dialer, sock, "mqtt-tcp://127.0.0.1:1883");

    // Build CONNECT message
    nng_msg *connmsg;
    nng_mqtt_msg_alloc(&connmsg, 0);
    nng_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_client_id(connmsg, "my-c-client");
    nng_mqtt_msg_set_connect_keep_alive(connmsg, 60);
    nng_mqtt_msg_set_connect_clean_session(connmsg, true);

    nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg);
    nng_dialer_start(dialer, NNG_FLAG_NONBLOCK);

    nng_msleep(1000); // give it time to connect

    // Build and send a PUBLISH message
    nng_msg *pubmsg;
    nng_mqtt_msg_alloc(&pubmsg, 0);
    nng_mqtt_msg_set_packet_type(pubmsg, NNG_MQTT_PUBLISH);
    nng_mqtt_msg_set_publish_topic(pubmsg, "hello/topic");
    nng_mqtt_msg_set_publish_payload(pubmsg, (uint8_t *)"hello nanomq", 12);
    nng_mqtt_msg_set_publish_qos(pubmsg, 1);

    nng_sendmsg(sock, pubmsg, 0);

    nng_msleep(500);
    nng_close(sock);
    return 0;
}