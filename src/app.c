#include <nng/nng.h>
#include <nng/mqtt/mqtt_client.h>
#include <stdio.h>
#include <string.h>

#define MQTT_URL "mqtt-tcp://127.0.0.1:1883"

int main() {
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
    return 0;
}
