#include <nng/nng.h>
#include <nng/mqtt/mqtt_client.h>
#include <nng/supplemental/util/platform.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BROKER_URL "mqtt-tcp://127.0.0.1:1883"
// #define BROKER_URL "ipc:///tmp/mqtt.sock"
#define TOPIC      "test/topic"
#define CLIENT_ID  "nanosdk-subscriber"

typedef struct {
    nng_mtx *mtx;
    nng_cv  *cv;
    bool     connected;
} connect_state;

static volatile sig_atomic_t running = 1;
static void handle_sigint(int sig) { (void)sig; running = 0; }

static void fatal(const char *what, int rv) {
    fprintf(stderr, "%s: %s\n", what, nng_strerror(rv));
    exit(1);
}

static void send_disconnect(nng_socket sock) {
    nng_msg *discmsg;
    int      rv;

    if ((rv = nng_mqtt_msg_alloc(&discmsg, 0)) != 0) {
        fprintf(stderr, "nng_mqtt_msg_alloc (disconnect): %s\n", nng_strerror(rv));
        return;
    }

    nng_mqtt_msg_set_packet_type(discmsg, NNG_MQTT_DISCONNECT);
    if ((rv = nng_sendmsg(sock, discmsg, 0)) != 0) {
        fprintf(stderr, "nng_sendmsg (disconnect): %s\n", nng_strerror(rv));
    }
}

static void on_connect(nng_pipe pipe, nng_pipe_ev event, void *arg) {
    (void)pipe;
    (void)event;

    connect_state *state = arg;

    nng_mtx_lock(state->mtx);
    state->connected = true;
    nng_cv_wake(state->cv);
    nng_mtx_unlock(state->mtx);
}

int main(void) {
    signal(SIGINT, handle_sigint);

    nng_socket sock;
    int rv;
    connect_state state = { 0 };

    if ((rv = nng_mqtt_client_open(&sock)) != 0) {
        fatal("nng_mqtt_client_open", rv);
    }
    if ((rv = nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 200)) != 0) {
        nng_close(sock);
        fatal("nng_socket_set_ms (recv timeout)", rv);
    }

    if ((rv = nng_mtx_alloc(&state.mtx)) != 0) {
        fatal("nng_mtx_alloc", rv);
    }
    if ((rv = nng_cv_alloc(&state.cv, state.mtx)) != 0) {
        nng_mtx_free(state.mtx);
        fatal("nng_cv_alloc", rv);
    }
    if ((rv = nng_mqtt_set_connect_cb(sock, on_connect, &state)) != 0) {
        nng_cv_free(state.cv);
        nng_mtx_free(state.mtx);
        fatal("nng_mqtt_set_connect_cb", rv);
    }

    nng_dialer dialer;
    if ((rv = nng_dialer_create(&dialer, sock, BROKER_URL)) != 0) {
        nng_cv_free(state.cv);
        nng_mtx_free(state.mtx);
        nng_close(sock);
        fatal("nng_dialer_create", rv);
    }

    nng_msg *connmsg;
    nng_mqtt_msg_alloc(&connmsg, 0);
    nng_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_proto_version(connmsg, MQTT_PROTOCOL_VERSION_v311);
    nng_mqtt_msg_set_connect_client_id(connmsg, CLIENT_ID);
    nng_mqtt_msg_set_connect_clean_session(connmsg, true);
    nng_mqtt_msg_set_connect_keep_alive(connmsg, 60);

    nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, connmsg);
    if ((rv = nng_dialer_start(dialer, NNG_FLAG_NONBLOCK)) != 0) {
        nng_cv_free(state.cv);
        nng_mtx_free(state.mtx);
        nng_close(sock);
        fatal("nng_dialer_start", rv);
    }

    nng_mtx_lock(state.mtx);
    while (!state.connected) {
        nng_cv_wait(state.cv);
    }
    nng_mtx_unlock(state.mtx);

    /* --- Build and send a SUBSCRIBE message ---
     * NOTE: the helper functions/argument list for building a topic-QoS
     * array (nng_mqtt_topic_qos_array_create/_set) have changed shape
     * across NanoSDK releases (extra MQTT5-only fields were added later).
     * I could not check your installed header from this environment, so
     * verify this against yours with:
     *   grep -A3 "nng_mqtt_topic_qos_array_set" /path/to/NanoSDK/include/nng/mqtt/mqtt_client.h
     * and adjust the call below if the parameter list differs. */
    nng_msg *submsg;
    nng_mqtt_msg_alloc(&submsg, 0);
    nng_mqtt_msg_set_packet_type(submsg, NNG_MQTT_SUBSCRIBE);

    nng_mqtt_topic_qos *topics_qos = nng_mqtt_topic_qos_array_create(1);
    nng_mqtt_topic_qos_array_set(topics_qos, 0, TOPIC, 0 /* QoS */, 0, 0, 0);
    nng_mqtt_msg_set_subscribe_topics(submsg, topics_qos, 1);

    if ((rv = nng_sendmsg(sock, submsg, 0)) != 0) {
        fatal("nng_sendmsg (subscribe)", rv);
    }
    nng_mqtt_topic_qos_array_free(topics_qos, 1);

    printf("Subscribed to '%s'. Waiting for messages (Ctrl+C to quit)...\n", TOPIC);

    while (running) {
        nng_msg *msg;
        rv = nng_recvmsg(sock, &msg, 0);
        if (rv != 0) {
            if (rv == NNG_ETIMEDOUT) {
                continue;
            }
            fprintf(stderr, "nng_recvmsg: %s\n", nng_strerror(rv));
            continue;
        }

        uint8_t packet_type = nng_mqtt_msg_get_packet_type(msg);
        if (packet_type == NNG_MQTT_PUBLISH) {
            uint32_t payload_len = 0;
            uint8_t *payload = nng_mqtt_msg_get_publish_payload(msg, &payload_len);

            uint32_t topic_len = 0;
            const char *recv_topic = nng_mqtt_msg_get_publish_topic(msg, &topic_len);

            printf("Received on '%.*s': %.*s\n",
                   (int)topic_len, recv_topic,
                   (int)payload_len, (char *)payload);
            fflush(stdout);
        }

        nng_msg_free(msg);
    }

    send_disconnect(sock);
    nng_close(sock);
    nng_cv_free(state.cv);
    nng_mtx_free(state.mtx);
    printf("\nSubscriber exiting.\n");
    return 0;
}