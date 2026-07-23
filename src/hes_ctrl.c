#include <stdio.h>
#include <string.h>
#include <curl/curl.h>

// Very small helper to call NanoMQ's REST API from your own backend code.
// Your users never see this — they call *your* function/endpoint instead.

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    fwrite(ptr, size, nmemb, stdout); // in real code, capture into a buffer
    return size * nmemb;
}

int nanomq_list_clients(void) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:8081/api/v4/clients/");
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERPWD, "admin:public"); // load from secure config, not hardcoded
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        fprintf(stderr, "request failed: %s\n", curl_easy_strerror(res));

    curl_easy_cleanup(curl);
    return res == CURLE_OK ? 0 : -1;
}

int nanomq_publish(const char *topic, const char *payload, int qos) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char body[512];
    snprintf(body, sizeof(body),
        "{\"topic\":\"%s\",\"payload\":\"%s\",\"qos\":%d,\"retain\":false}",
        topic, payload, qos);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:8081/api/v4/mqtt/publish");
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERPWD, "admin:public");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res == CURLE_OK ? 0 : -1;
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    nanomq_list_clients();
    nanomq_publish("sensors/temp", "23.5", 1);
    curl_global_cleanup();
    return 0;
}
