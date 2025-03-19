#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <libwebsockets.h>
#include <cjson/cJSON.h>

/* ANSI colors for console output */
#define KGRN "\033[0;32;32m"
#define KCYN "\033[0;36m"
#define KRED "\033[0;32;31m"
#define KYEL "\033[1;33m"
#define KBLU "\033[0;32;34m"
#define KCYN_L "\033[1;36m"
#define KBRN "\033[0;33m"
#define RESET "\033[0m"

#define EXAMPLE_RX_BUFFER_BYTES (1024)
#define QUEUE_SIZE 1024
#define NUM_SYMBOLS 8
#define MAX_MA_HISTORY 8
#define RECONNECT_DELAY 15  //seconds between reconnection attempts
#define WATCHDOG_TIMEOUT 60 //time between connection lost

//flags for control
volatile static int destroy_flag = 0;
volatile static int connection_flag = 0;
volatile static int writeable_flag = 0;

static struct lws_context *g_context = NULL;

//variable to track the last time we received data
volatile static time_t last_received_time = 0;

//function that formats a time from unix timestamp to a string
static void format_timestamp(time_t t, char *buf, size_t size) {
    struct tm *tm_info = localtime(&t);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/* ======= Transaction structure ======= */

typedef struct {
    char symbol[20];
    double price;
    double volume;
    time_t timestamp;
} transaction_t;

/* ======= Queue for transactions ======= */
//circular queue for transactions
typedef struct {
    transaction_t buffer[QUEUE_SIZE];
    int front;
    int rear;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} tx_queue_t;

static tx_queue_t g_tx_queue;

//Signal handler for interrupt signals
static void INT_HANDLER(int signo) {
    destroy_flag = 1;
    printf(KRED "\nInterrupt signal received, shutting down...\n" RESET);
}

//Initialize the transaction queue
static void init_queue(tx_queue_t *q) {
    q->front = q->rear = q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

//Clean up the resources used by the transaction queue
static void cleanup_queue(tx_queue_t *q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

//Enqueue a new transaction
static void enqueue_tx(tx_queue_t *q, const transaction_t *tx) {
    pthread_mutex_lock(&q->mutex);
    if (q->count < QUEUE_SIZE) {
        q->buffer[q->rear] = *tx;
        q->rear = (q->rear + 1) % QUEUE_SIZE;
        q->count++;
        pthread_cond_signal(&q->cond);
    } else {
        printf(KRED "[Queue] Warning: Queue is full, dropping transaction\n" RESET);
    }
    pthread_mutex_unlock(&q->mutex);
}

//Dequeue a transaction 
static int dequeue_tx(tx_queue_t *q, transaction_t *tx) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !destroy_flag) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    if (destroy_flag) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    *tx = q->buffer[q->front];
    q->front = (q->front + 1) % QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

//linked list node to store a transaction */
typedef struct tx_node {
    transaction_t tx;
    struct tx_node *next;
} tx_node_t;

//Structure for each symbol with moving average history
typedef struct {
    char symbol[20];
    tx_node_t *head;
    pthread_mutex_t mutex;
    double ma_history[MAX_MA_HISTORY];
    time_t ma_times[MAX_MA_HISTORY];
    int ma_count;
} symbol_store_t;

static symbol_store_t g_stores[NUM_SYMBOLS];

//List of symbols
static const char *symbol_list[NUM_SYMBOLS] = {
    "BTC-USDT", "ADA-USDT", "ETH-USDT", "DOGE-USDT",
    "XRP-USDT", "SOL-USDT", "LTC-USDT", "BNB-USDT"
};

//Find the index of a symbol in the symbol_list
static int symbol_index(const char *sym) {
    if (!sym) return -1;
    for (int i = 0; i < NUM_SYMBOLS; i++) {
        if (strcmp(sym, symbol_list[i]) == 0)
            return i;
    }
    return -1;
}

//Initialize the in-memory stores for each symbol
static void init_stores(void) {
    for (int i = 0; i < NUM_SYMBOLS; i++) {
        strncpy(g_stores[i].symbol, symbol_list[i], sizeof(g_stores[i].symbol) - 1);
        g_stores[i].symbol[sizeof(g_stores[i].symbol) - 1] = '\0';
        g_stores[i].head = NULL;
        pthread_mutex_init(&g_stores[i].mutex, NULL);
        g_stores[i].ma_count = 0;
    }
}

//Clean up the in-memory stores for each symbol
static void cleanup_stores(void) {
    for (int i = 0; i < NUM_SYMBOLS; i++) {
        pthread_mutex_lock(&g_stores[i].mutex);
        while (g_stores[i].head) {
            tx_node_t *temp = g_stores[i].head;
            g_stores[i].head = temp->next;
            free(temp);
        }
        pthread_mutex_unlock(&g_stores[i].mutex);
        pthread_mutex_destroy(&g_stores[i].mutex);
    }
}

/* ======= File operations ======= */
//Log a transaction to the per-symbol transaction file and store it in the memory list
static void log_transaction(const transaction_t *tx) {
    if (!tx || !tx->symbol[0])
        return;
    
    int idx = symbol_index(tx->symbol);
    if (idx < 0) {
        printf(KRED "[Logger] Unknown symbol: %s\n" RESET, tx->symbol);
        return;
    }
    
    char fname[64];
    snprintf(fname, sizeof(fname), "%s_transactions.log", tx->symbol);
    FILE *fp = fopen(fname, "a");
    if (!fp) {
        perror("Failed to open transaction file");
        return;
    }
    char time_buffer[64];
    format_timestamp(tx->timestamp, time_buffer, sizeof(time_buffer));
    fprintf(fp, "%s,%.8f,%.8f\n", time_buffer, tx->price, tx->volume);
    fclose(fp);
    
    pthread_mutex_lock(&g_stores[idx].mutex);
    tx_node_t *node = (tx_node_t *)malloc(sizeof(tx_node_t));
    if (!node) {
        pthread_mutex_unlock(&g_stores[idx].mutex);
        perror("Memory allocation failure for transaction");
        return;
    }
    node->tx = *tx;
    node->next = g_stores[idx].head;
    g_stores[idx].head = node;
    pthread_mutex_unlock(&g_stores[idx].mutex);
}

//Log a line of text to a given log file
static void log_line(const char *fname, const char *line) {
    if (!fname || !line)
        return;
    
    FILE *fp = fopen(fname, "a");
    if (!fp) {
        perror("Failed to open log file");
        return;
    }
    
    fprintf(fp, "%s\n", line);
    fclose(fp);
}

/* ======= Moving Average & Volume Calculations ======= */
//Remove older transactions
static void clean_old_transactions(symbol_store_t *store, time_t cutoff) {
    if (!store)
        return;
    pthread_mutex_lock(&store->mutex);
    tx_node_t **pp = &store->head;
    while (*pp) {
        if ((*pp)->tx.timestamp < cutoff) {
            tx_node_t *old = *pp;
            *pp = old->next;
            free(old);
        } else {
            pp = &((*pp)->next);
        }
    }
    pthread_mutex_unlock(&store->mutex);
}

//Compute the moving average of prices and total volume
static void compute_ma_and_volume(symbol_store_t *store, double *ma, double *vol) {
    if (!store || !ma || !vol)
        return;
    double sum = 0.0, volume = 0.0;
    int count = 0;
    pthread_mutex_lock(&store->mutex);
    tx_node_t *node = store->head;
    while (node) {
        sum += node->tx.price;
        volume += node->tx.volume;
        count++;
        node = node->next;
    }
    pthread_mutex_unlock(&store->mutex);
    *ma = (count > 0) ? (sum / count) : 0.0;
    *vol = volume;
}

//Update the moving average history for the given symbol
static void update_ma_history(symbol_store_t *store, time_t now, double val) {
    if (!store)
        return;
    pthread_mutex_lock(&store->mutex);
    if (store->ma_count < MAX_MA_HISTORY) {
        store->ma_history[store->ma_count] = val;
        store->ma_times[store->ma_count] = now;
        store->ma_count++;
    } else {
        for (int i = 1; i < MAX_MA_HISTORY; i++) {
            store->ma_history[i-1] = store->ma_history[i];
            store->ma_times[i-1] = store->ma_times[i];
        }
        store->ma_history[MAX_MA_HISTORY-1] = val;
        store->ma_times[MAX_MA_HISTORY-1] = now;
    }
    pthread_mutex_unlock(&store->mutex);
}

//Compute Pearson correlation coefficient
static double compute_pearson(const double *x, const double *y, int n) {
    if (!x || !y || n <= 1)
        return 0.0;
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0, sum_y2 = 0;
    for (int i = 0; i < n; i++){
        sum_x += x[i];
        sum_y += y[i];
        sum_xy += x[i] * y[i];
        sum_x2 += x[i] * x[i];
        sum_y2 += y[i] * y[i];
    }
    double numerator = n * sum_xy - sum_x * sum_y;
    double denominator_1 = n * sum_x2 - sum_x * sum_x;
    double denominator_2 = n * sum_y2 - sum_y * sum_y;
    if (denominator_1 <= 0 || denominator_2 <= 0)
        return 0.0;
    double denominator = sqrt(denominator_1) * sqrt(denominator_2);
    return (denominator == 0.0) ? 0.0 : (numerator / denominator);
}

//Retrieve the timestamp corresponding to the latest moving average entry
static time_t get_max_corr_time(symbol_store_t *store) {
    pthread_mutex_lock(&store->mutex);
    time_t t = (store->ma_count > 0) ? store->ma_times[store->ma_count - 1] : 0;
    pthread_mutex_unlock(&store->mutex);
    return t;
}

/* ======= CPU Idle Percentage Measurement ======= */
//Measures the CPU idle percentage
static double get_cpu_idle_percent() {
    FILE *fp;
    char buffer[256];
    long user1, nice1, system1, idle1, iowait1, irq1, softirq1, steal1;
    long user2, nice2, system2, idle2, iowait2, irq2, softirq2, steal2;
    fp = fopen("/proc/stat", "r");
    if (!fp) return -1;
    if (!fgets(buffer, sizeof(buffer), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    sscanf(buffer, "cpu  %ld %ld %ld %ld %ld %ld %ld %ld",
           &user1, &nice1, &system1, &idle1, &iowait1, &irq1, &softirq1, &steal1);
    sleep(1);
    fp = fopen("/proc/stat", "r");
    if (!fp) return -1;
    if (!fgets(buffer, sizeof(buffer), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    sscanf(buffer, "cpu  %ld %ld %ld %ld %ld %ld %ld %ld",
           &user2, &nice2, &system2, &idle2, &iowait2, &irq2, &softirq2, &steal2);
    long idle_time1 = idle1 + iowait1;
    long idle_time2 = idle2 + iowait2;
    long total1 = user1 + nice1 + system1 + idle1 + iowait1 + irq1 + softirq1 + steal1;
    long total2 = user2 + nice2 + system2 + idle2 + iowait2 + irq2 + softirq2 + steal2;
    long total_diff = total2 - total1;
    long idle_diff = idle_time2 - idle_time1;
    double cpu_idle_percent = (total_diff > 0) ? (100.0 * idle_diff / total_diff) : 0.0;
    return cpu_idle_percent;
}


// Sends data over a WebSocket connection
static int websocket_write_back(struct lws *wsi_in, char *str, int str_size_in) {
    if (!str || !wsi_in)
        return -1;
    int n, len = (str_size_in < 1) ? (int)strlen(str) : str_size_in;
    char *out = (char *)malloc(LWS_SEND_BUFFER_PRE_PADDING + len + LWS_SEND_BUFFER_POST_PADDING);
    if (!out) {
        perror("Memory allocation failure for websocket buffer");
        return -1;
    }
    memcpy(out + LWS_SEND_BUFFER_PRE_PADDING, str, len);
    n = lws_write(wsi_in, (unsigned char *)(out + LWS_SEND_BUFFER_PRE_PADDING), len, LWS_WRITE_TEXT);
    printf(KBLU "[WebSocket] Sent: %s\n" RESET, str);
    free(out);
    return n;
}

//Sends subscribe messages for all symbols
static void send_all_subscriptions(struct lws *wsi) {
    if (!wsi) return;
    for (int i = 0; i < NUM_SYMBOLS; i++) {
        char subscribe_msg[256];
        snprintf(subscribe_msg, sizeof(subscribe_msg),
                 "{\"op\":\"subscribe\",\"args\":[{\"channel\":\"tickers\",\"instId\":\"%s\"}]}",
                 symbol_list[i]);
        websocket_write_back(wsi, subscribe_msg, 0);
        usleep(200000);
    }
}

//Callback function handling different WebSocket events
static int ws_service_callback(struct lws *wsi, enum lws_callback_reasons reason,
                               void *user, void *in, size_t len)
{
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            connection_flag = 1;
            lws_callback_on_writable(wsi);
            break;
            
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            cJSON *root = cJSON_Parse((char *)in);
            if (root) {
                cJSON *ev = cJSON_GetObjectItemCaseSensitive(root, "event");
                if (!ev) {
                    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
                    if (data && cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
                        cJSON *trade = cJSON_GetArrayItem(data, 0);
                        if (trade) {
                            cJSON *instId = cJSON_GetObjectItemCaseSensitive(trade, "instId");
                            cJSON *last   = cJSON_GetObjectItemCaseSensitive(trade, "last");
                            cJSON *lastSz = cJSON_GetObjectItemCaseSensitive(trade, "lastSz");
                            if (cJSON_IsString(instId) && cJSON_IsString(last) && cJSON_IsString(lastSz)) {
                                transaction_t tx;
                                memset(&tx, 0, sizeof(tx));
                                strncpy(tx.symbol, instId->valuestring, sizeof(tx.symbol)-1);
                                tx.price = atof(last->valuestring);
                                tx.volume = atof(lastSz->valuestring);
                                tx.timestamp = time(NULL);
                                enqueue_tx(&g_tx_queue, &tx);
                                last_received_time = time(NULL);
                                printf(KGRN "[WebSocket] Transaction: %s, Price: %.8f, Volume: %.8f\n" RESET, 
                                       tx.symbol, tx.price, tx.volume);
                            }
                        }
                    }
                } else {
                    printf(KYEL "[WebSocket] Event: %s\n" RESET, ev->valuestring);
                }
                cJSON_Delete(root);
            }
            break;
        }
        
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            
            break;
            
        case LWS_CALLBACK_CLOSED:
            
            break;
            
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (!writeable_flag) {
                printf(KYEL "[WebSocket] Connection writable, subscribing to tickers\n" RESET);
                send_all_subscriptions(wsi);
                writeable_flag = 1;
            }
            break;
            
        default:
            break;
    }
    return 0;
}

//WebSocket protocols list
static struct lws_protocols ws_protocols[] = {
    { "okx-protocol", ws_service_callback, 0, EXAMPLE_RX_BUFFER_BYTES },
    { NULL, NULL, 0, 0 }
};

/* ======= Reconnect Thread ======= */
//This thread checks every 10 seconds whether data has been received recently, if not, triggers reconnection
static void *watchdog_thread(void *arg) {
    while (!destroy_flag) {
        sleep(10);
        time_t now = time(NULL);
        if ((now - last_received_time) > WATCHDOG_TIMEOUT) {
            printf(KRED "[Watchdog] Timeout: No data received for over %d seconds, reconnecting...\n" RESET, WATCHDOG_TIMEOUT);
            connection_flag = 0;
        }
    }
    return NULL;
}

/* ======= WebSocket Thread (Producer) ======= */
//This thread manages the WebSocket connection
static void *websocket_thread(void *arg) {
    struct lws_context *context = NULL;
    struct lws *wsi = NULL;
    int retry_count = 0;
    
    printf(KGRN "[WebSocket] Thread started\n" RESET);
    
    while (!destroy_flag) {
        if (!context) {
            struct lws_context_creation_info info;
            memset(&info, 0, sizeof(info));
            info.port = CONTEXT_PORT_NO_LISTEN;
            info.protocols = ws_protocols;
            info.gid = -1;
            info.uid = -1;
            info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
            
            context = lws_create_context(&info);
            if (!context) {
                fprintf(stderr, KRED "[WebSocket] Failed to create context\n" RESET);
                sleep(RECONNECT_DELAY);
                continue;
            }
            printf(KGRN "[WebSocket] Context created\n" RESET);
        }
        
        if (!connection_flag && !wsi) {
            char inputURL[300] = "wss://ws.okx.com:8443/ws/v5/public";
            const char *urlProtocol, *urlAddress, *urlPath;
            int urlPort;
            if (lws_parse_uri(inputURL, &urlProtocol, &urlAddress, &urlPort, &urlPath)) {
                fprintf(stderr, KRED "[WebSocket] Failed to parse URL\n" RESET);
                sleep(RECONNECT_DELAY);
                continue;
            }
            
            struct lws_client_connect_info ccinfo;
            memset(&ccinfo, 0, sizeof(ccinfo));
            
            ccinfo.context = context;
            ccinfo.address = urlAddress;
            ccinfo.port = 8443;
            ccinfo.path = urlPath;
            ccinfo.host = ccinfo.address;
            ccinfo.origin = ccinfo.address;
            ccinfo.protocol = ws_protocols[0].name;
            ccinfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED |
                                    LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
            
            writeable_flag = 0;
            
            printf(KYEL "[WebSocket] Connecting to %s:%d%s (attempt %d)\n" RESET,
                   ccinfo.address, ccinfo.port, ccinfo.path, retry_count + 1);
            
            wsi = lws_client_connect_via_info(&ccinfo);
            if (!wsi) {
                fprintf(stderr, KRED "[WebSocket] Connection failed\n" RESET);
                retry_count++;
                int delay = RECONNECT_DELAY * (1 << (retry_count > 3 ? 3 : retry_count));
                printf(KYEL "[WebSocket] Next attempt in %d seconds\n" RESET, delay);
                sleep(delay);
                continue;
            }
            printf(KGRN "[WebSocket] Connection started\n" RESET);
            retry_count = 0;
        }
        
        if (context)
            lws_service(context, 50);
        
        if (!connection_flag && wsi) {
            wsi = NULL;
            sleep(1);
        }
    }
    
    if (context)
        lws_context_destroy(context);
    
    printf(KGRN "[WebSocket] Thread terminated\n" RESET);
    return NULL;
}

/* ======= Logger Thread (Consumer) ======= */
//This thread consumes transactions from the queue and logs them
static void *logger_thread(void *arg) {
    printf(KGRN "[Logger] Thread started\n" RESET);
    
    transaction_t tx;
    while (!destroy_flag) {
        if (dequeue_tx(&g_tx_queue, &tx) == 0) {
            log_transaction(&tx);
        }
    }
    
    printf(KGRN "[Logger] Thread terminated\n" RESET);
    return NULL;
}

/* ======= Scheduler Thread ======= */
//This thread runs every minute to calculate moving averages, volume,Pearson correlations, and CPU idle percentage
static void *scheduler_thread(void *arg) {
    printf(KGRN "[Scheduler] Thread started\n" RESET);
    
    while (!destroy_flag) {
        time_t now = time(NULL);
        int wait_sec = 60 - (now % 60);
        printf(KYEL "[Scheduler] Waiting %d seconds for the next minute\n" RESET, wait_sec);
        for (int i = 0; i < wait_sec && !destroy_flag; i++) {
            sleep(1);
        }
        if (destroy_flag) break;
        
        time_t current_t = time(NULL);
        char current_time_str[64];
        format_timestamp(current_t, current_time_str, sizeof(current_time_str));
        printf(KYEL "[Scheduler] Processing at %s\n" RESET, current_time_str);
        
        for (int i = 0; i < NUM_SYMBOLS; i++) {
            clean_old_transactions(&g_stores[i], current_t - 15 * 60);
            double ma_val = 0.0, total_vol = 0.0;
            compute_ma_and_volume(&g_stores[i], &ma_val, &total_vol);
            update_ma_history(&g_stores[i], current_t, ma_val);
            
            char line[128], fname[64];
            char time_str[64];
            format_timestamp(current_t, time_str, sizeof(time_str));
            snprintf(fname, sizeof(fname), "%s_movingAvg.log", g_stores[i].symbol);
            snprintf(line, sizeof(line), "%s,%.8f,%.8f", time_str, ma_val, total_vol);
            log_line(fname, line);
            
            printf(KGRN "[Scheduler] MA for %s: %.8f, Volume: %.8f\n" RESET, g_stores[i].symbol, ma_val, total_vol);
        }
        
        for (int i = 0; i < NUM_SYMBOLS; i++) {
            double target_hist[MAX_MA_HISTORY];
            int n_target = 0;
            pthread_mutex_lock(&g_stores[i].mutex);
            n_target = g_stores[i].ma_count;
            for (int j = 0; j < n_target; j++)
                target_hist[j] = g_stores[i].ma_history[j];
            pthread_mutex_unlock(&g_stores[i].mutex);
            
            if (n_target <= 1) {
                printf(KYEL "[Scheduler] Not enough data for correlation for %s\n" RESET, g_stores[i].symbol);
                continue;
            }
            
            double best_corr = -2.0;
            int best_idx = -1;
            for (int k = 0; k < NUM_SYMBOLS; k++) {
                if (k == i) continue;
                double other_hist[MAX_MA_HISTORY];
                int n_other = 0;
                pthread_mutex_lock(&g_stores[k].mutex);
                n_other = g_stores[k].ma_count;
                for (int h = 0; h < n_other; h++)
                    other_hist[h] = g_stores[k].ma_history[h];
                pthread_mutex_unlock(&g_stores[k].mutex);
                int n = (n_target < n_other) ? n_target : n_other;
                double corr = compute_pearson(target_hist, other_hist, n);
                if (corr > best_corr) {
                    best_corr = corr;
                    best_idx = k;
                }
            }
            time_t best_time = 0;
            if (best_idx >= 0) {
                pthread_mutex_lock(&g_stores[best_idx].mutex);
                if (g_stores[best_idx].ma_count > 0)
                    best_time = g_stores[best_idx].ma_times[g_stores[best_idx].ma_count - 1];
                pthread_mutex_unlock(&g_stores[best_idx].mutex);
            }
            char corr_line[128], corr_fname[64];
            char best_time_str[64];
            format_timestamp(current_t, best_time_str, sizeof(best_time_str));
            snprintf(corr_fname, sizeof(corr_fname), "%s_correlation.log", g_stores[i].symbol);
            snprintf(corr_line, sizeof(corr_line), "%s,%.4f,%s,%s",
                     current_time_str, best_corr,
                     (best_idx >= 0) ? g_stores[best_idx].symbol : "N/A",
                     (best_idx >= 0) ? best_time_str : "N/A");
            log_line(corr_fname, corr_line);
            
            printf(KGRN "[Scheduler] Correlation for %s: %.4f with %s\n" RESET,
                   g_stores[i].symbol, best_corr,
                   (best_idx >= 0) ? g_stores[best_idx].symbol : "N/A");
        }
        
        double cpu_idle = get_cpu_idle_percent();
        char cpu_line[128];
        snprintf(cpu_line, sizeof(cpu_line), "%s,%.2f", current_time_str, cpu_idle);
        log_line("cpu_idle.log", cpu_line);
        printf(KGRN "[Scheduler] CPU Idle: %.2f%%\n" RESET, cpu_idle);
    }
    
    printf(KGRN "[Scheduler] Thread terminated\n" RESET);
    return NULL;
}

/* ======= Main Function ======= */
static void handle_sigint(int sig) {
    destroy_flag = 1;
}

int main(void) {
    struct sigaction act;
    act.sa_handler = handle_sigint;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGINT, &act, 0);
    
    printf(KGRN "OKX WebSocket Client starting...\n" RESET);
    printf(KGRN "Press Ctrl+C to terminate\n" RESET);
    
    init_queue(&g_tx_queue);
    init_stores();
    
    pthread_t ws_tid, log_tid, sched_tid, watchdog_tid;
    
    //Create the logger thread
    if (pthread_create(&log_tid, NULL, logger_thread, NULL) != 0) {
        perror("Failed to create logger thread");
        return EXIT_FAILURE;
    }
    
    //Create the scheduler thread
    if (pthread_create(&sched_tid, NULL, scheduler_thread, NULL) != 0) {
        perror("Failed to create scheduler thread");
        pthread_cancel(log_tid);
        return EXIT_FAILURE;
    }
    
    //Create the WebSocket thread
    if (pthread_create(&ws_tid, NULL, websocket_thread, NULL) != 0) {
        perror("Failed to create websocket thread");
        pthread_cancel(log_tid);
        pthread_cancel(sched_tid);
        return EXIT_FAILURE;
    }
    
    //Create the reconnect thread
    if (pthread_create(&watchdog_tid, NULL, watchdog_thread, NULL) != 0) {
        perror("Failed to create watchdog thread");
        pthread_cancel(ws_tid);
        pthread_cancel(log_tid);
        pthread_cancel(sched_tid);
        return EXIT_FAILURE;
    }
    
    while (!destroy_flag)
        sleep(1);
    
    pthread_cancel(ws_tid);
    pthread_cancel(log_tid);
    pthread_cancel(sched_tid);
    pthread_cancel(watchdog_tid);
    pthread_join(ws_tid, NULL);
    pthread_join(log_tid, NULL);
    pthread_join(sched_tid, NULL);
    pthread_join(watchdog_tid, NULL);
    
    cleanup_stores();
    cleanup_queue(&g_tx_queue);
    
    printf(KGRN "Program terminated successfully\n" RESET);
    return EXIT_SUCCESS;
}