#include <nng/nng.h>
#include <nng/mqtt/mqtt_client.h>
#include <nng/supplemental/util/platform.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BROKER_URL "mqtt-tcp://127.0.0.1:1883"
// #define BROKER_URL "ipc:///tmp/mqtt.sock"
#define TOPIC      "test/topic"
#define CLIENT_ID  "nanosdk-publisher"

typedef struct {
    nng_mtx *mtx;
    nng_cv  *cv;
    bool     connected;
} connect_state;

static volatile sig_atomic_t running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

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

int main(int argc, char *argv[]) {
    const char *message = (argc > 1) ? argv[1] : "hello from NanoSDK publisher";

    signal(SIGINT, handle_sigint);

    nng_socket sock;
    int rv;
    connect_state state = { 0 };

    if ((rv = nng_mqtt_client_open(&sock)) != 0) {
        fatal("nng_mqtt_client_open", rv);
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

    /* Build the CONNECT message and attach it to the dialer: NanoSDK sends
     * it automatically as soon as the underlying TCP connection opens. */
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

    while (running) {
        nng_msg *pubmsg;

        if ((rv = nng_mqtt_msg_alloc(&pubmsg, 0)) != 0) {
            fatal("nng_mqtt_msg_alloc (publish)", rv);
        }

        nng_mqtt_msg_set_packet_type(pubmsg, NNG_MQTT_PUBLISH);
        nng_mqtt_msg_set_publish_topic(pubmsg, TOPIC);
        nng_mqtt_msg_set_publish_payload(pubmsg, (uint8_t *)message, (uint32_t)strlen(message));
        nng_mqtt_msg_set_publish_qos(pubmsg, 0);

        if ((rv = nng_sendmsg(sock, pubmsg, 0)) != 0) {
            fatal("nng_sendmsg (publish)", rv);
        }
        printf("Published to '%s': %s\n", TOPIC, message);

        if (!running) {
            break;
        }

        sleep(1);
    }

    send_disconnect(sock);
    nng_close(sock);
    nng_cv_free(state.cv);
    nng_mtx_free(state.mtx);
    return 0;
}