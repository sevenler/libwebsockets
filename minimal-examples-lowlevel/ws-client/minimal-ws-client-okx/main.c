#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

/* OKX WebSocket endpoint */
#define WS_SERVER "ws.okx.com"
#define WS_PORT 8443
#define SUBSCRIBE_MSG "{\"op\":\"subscribe\",\"args\":[{\"channel\":\"books\",\"instId\":\"BTC-USDT\"}]}"

struct range {
    unsigned long long lowest;
    unsigned long long highest;
    unsigned long long sum;
    unsigned int samples;
};

struct msg_client_okx {
    lws_sorted_usec_list_t sul;  /* schedule connection retry */
    lws_sorted_usec_list_t sul_hz;  /* 1hz summary */
    struct range price_range;
    struct range e_lat_range;
    struct lws *client_wsi;
    uint16_t retry_count;
};

static struct msg_client_okx mco;
static struct lws_context *context;
static int interrupted;

static void range_reset(struct range *r)
{
    r->lowest = 0xffffffffffffffffull;
    r->highest = 0;
    r->sum = 0;
    r->samples = 0;
}

static void range_add(struct range *r, unsigned long long val)
{
    if (!r->samples) {
        r->lowest = val;
        r->highest = val;
    } else {
        if (val < r->lowest)
            r->lowest = val;
        if (val > r->highest)
            r->highest = val;
    }
    r->sum += val;
    r->samples++;
}

/* Forward declarations */
static int callback_okx(struct lws *wsi, enum lws_callback_reasons reason,
        void *user, void *in, size_t len);

static const struct lws_protocols protocols[] = {
    { "okx-protocol", callback_okx, sizeof(struct msg_client_okx), 0, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

static void sul_hz_cb(struct lws_sorted_usec_list *sul)
{
    struct msg_client_okx *mco = lws_container_of(sul, struct msg_client_okx, sul);

    if (mco->price_range.samples)
        lwsl_notice("%s: price: min: %llu, max: %llu, avg: %llu, samples: %d\n",
                __func__,
                (unsigned long long)mco->price_range.lowest,
                (unsigned long long)mco->price_range.highest,
                (unsigned long long)(mco->price_range.sum / mco->price_range.samples),
                mco->price_range.samples);

    if (mco->e_lat_range.samples)
        lwsl_notice("%s: elatency: min: %llums, max: %llums, avg: %llums, (%d msg/s)\n",
                __func__,
                (unsigned long long)mco->e_lat_range.lowest / 1000,
                (unsigned long long)mco->e_lat_range.highest / 1000,
                (unsigned long long)(mco->e_lat_range.sum / mco->e_lat_range.samples) / 1000,
                mco->e_lat_range.samples);

    range_reset(&mco->e_lat_range);
    range_reset(&mco->price_range);

    lws_sul_schedule(context, 0, &mco->sul_hz, sul_hz_cb, LWS_US_PER_SEC);
}

static void connect_client(struct lws_sorted_usec_list *sul)
{
    struct msg_client_okx *mco = lws_container_of(sul, struct msg_client_okx, sul);
    struct lws_client_connect_info i;

    memset(&i, 0, sizeof(i));

    i.context = context;
    i.port = WS_PORT;
    i.address = WS_SERVER;
    i.path = "/ws/v5/public";
    i.host = i.address;
    i.origin = i.address;
    i.ssl_connection = LCCSCF_USE_SSL | LCCSCF_PRIORITIZE_READS;
    i.protocol = protocols[0].name;
    i.local_protocol_name = protocols[0].name;
    i.pwsi = &mco->client_wsi;

    if (!lws_client_connect_via_info(&i)) {
        lwsl_err("%s: client connect failed\n", __func__);
        if (mco->retry_count++ < 10)
            lws_sul_schedule(context, 0, &mco->sul, connect_client,
                    (uint64_t)LWS_US_PER_SEC * (1 << mco->retry_count));
        return;
    }
}

__attribute__((used)) static int callback_okx(struct lws *wsi, enum lws_callback_reasons reason,
        void *user, void *in, size_t len)
{
    struct msg_client_okx *mco = (struct msg_client_okx *)user;
    static struct timeval start;
    struct timeval now;
    uint64_t latency_us;

    switch (reason) {
        case LWS_CALLBACK_PROTOCOL_INIT:
            mco->client_wsi = NULL;
            mco->retry_count = 0;
            /* Don't reset ranges here as they're already initialized in main */

            /* schedule first connection */
            lws_sul_schedule(context, 0, &mco->sul, connect_client, 1);

            /* schedule the 1Hz callback */
            lws_sul_schedule(context, 0, &mco->sul_hz, sul_hz_cb, LWS_US_PER_SEC);
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            lwsl_err("CLIENT_CONNECTION_ERROR: %s\n", in ? (char *)in : "(null)");
            mco->client_wsi = NULL;

            if (mco->retry_count++ < 10)
                lws_sul_schedule(context, 0, &mco->sul, connect_client,
                        (uint64_t)LWS_US_PER_SEC * (1 << mco->retry_count));
            break;

        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            lwsl_user("Connected to OKX WebSocket server\n");
            mco->retry_count = 0;

            /* set a timer for 1 second */
            lws_set_timer_usecs(wsi, LWS_US_PER_SEC);
            /* subscribe to orderbook */
            if (lws_write(wsi, (unsigned char *)SUBSCRIBE_MSG,
                        strlen(SUBSCRIBE_MSG), LWS_WRITE_TEXT) < 0) {
                lwsl_err("Failed to send subscription message\n");
                return -1;
            }
            gettimeofday(&start, NULL);
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (!len)
                break;

            gettimeofday(&now, NULL);
            latency_us = ((uint64_t)(now.tv_sec - start.tv_sec)) * LWS_US_PER_SEC +
                (uint64_t)(now.tv_usec - start.tv_usec);
            range_add(&mco->e_lat_range, latency_us);

            /* Process received data here */
            /* Note: For production, you should use proper JSON parsing */
            lwsl_user("Received: %.*s\n", (int)len, (char *)in);
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            lwsl_user("Connection closed\n");
            mco->client_wsi = NULL;
            if (mco->retry_count++ < 10)
                lws_sul_schedule(context, 0, &mco->sul, connect_client,
                        (uint64_t)LWS_US_PER_SEC * (1 << mco->retry_count));
            break;

        default:
            break;
    }

    return lws_callback_http_dummy(wsi, reason, user, in, len);
}

static void sigint_handler(int sig)
{
    interrupted = 1;
}

int main(int argc, char **argv)
{
    struct lws_context_creation_info info;
    int n = 0;

    signal(SIGINT, sigint_handler);

    memset(&info, 0, sizeof info);
    memset(&mco, 0, sizeof mco);
    
    range_reset(&mco.price_range);
    range_reset(&mco.e_lat_range);

    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.fd_limit_per_thread = 1 + 1 + 1;

    context = lws_create_context(&info);
    if (!context) {
        lwsl_err("lws init failed\n");
        return 1;
    }

    lwsl_user("LWS minimal OKX client\n");

    /* Schedule the first connection attempt */
    lws_sul_schedule(context, 0, &mco.sul, connect_client, 1);

    /* Schedule the 1Hz statistics callback */
    lws_sul_schedule(context, 0, &mco.sul_hz, sul_hz_cb, LWS_US_PER_SEC);

    while (n >= 0 && !interrupted)
        n = lws_service(context, 0);

    lws_context_destroy(context);
    lwsl_user("Completed\n");

    return 0;
}
