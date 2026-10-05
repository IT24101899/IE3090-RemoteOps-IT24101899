/*
 * RemoteOps Controller — IE3090 Network Programming
 * Registration: IT24101899
 * Connects to Agent at port 9410 with token OPS-1899
 *
 * Interactive commands:
 *   auth <token>              -> AUTH <token>
 *   sysinfo                   -> SYSINFO
 *   listproc                  -> LISTPROC
 *   exec <name>               -> EXEC <name>
 *   put <local> <remote>      -> PUT <remote> <size> + bytes
 *   get <remote> <local>      -> GET <remote> + bytes saved locally
 *   monitor start <port>      -> bind UDP, then MONITOR START <port>
 *   monitor stop              -> MONITOR STOP
 *   quit                      -> QUIT
 *   help                      -> show commands
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define AGENT_PORT   9410
#define TOKEN        "OPS-1899"
#define SID_TAG      "SID:9981"
#define BUF_SIZE     4096

/* ===== LINE BUFFER ===== */
typedef struct { char buf[BUF_SIZE]; int len; } linebuf_t;

/* ===== UDP LISTENER ===== */
static int g_udp_fd = -1;
static volatile int g_udp_run = 1;

static void *udp_thread(void *arg)
{
    (void)arg;
    char buf[BUF_SIZE];
    while (g_udp_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(g_udp_fd, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fl);
        if (n <= 0) { if (!g_udp_run) break; continue; }
        buf[n] = '\0';
        time_t now = time(NULL);
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
        printf("[UDP %s] %s", ts, buf);
        fflush(stdout);
    }
    return NULL;
}

static int start_udp_listener(int port)
{
    if (g_udp_fd >= 0) close(g_udp_fd);
    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) { perror("udp socket"); return -1; }
    int opt = 1;
    setsockopt(g_udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);
    if (bind(g_udp_fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("udp bind"); close(g_udp_fd); g_udp_fd = -1; return -1;
    }
    pthread_t tid;
    pthread_create(&tid, NULL, udp_thread, NULL);
    pthread_detach(tid);
    return 0;
}

static void stop_udp_listener(void)
{
    g_udp_run = 0;
    if (g_udp_fd >= 0) { close(g_udp_fd); g_udp_fd = -1; }
}

/* ===== LINE READER ===== */
static int read_line(int fd, linebuf_t *lb, char *out, size_t outsz)
{
    while (1) {
        char *nl = memchr(lb->buf, '\n', lb->len);
        if (nl) {
            size_t n = nl - lb->buf;
            if (n >= outsz) n = outsz - 1;
            memcpy(out, lb->buf, n);
            out[n] = '\0';
            int rem = lb->len - (nl - lb->buf) - 1;
            memmove(lb->buf, nl + 1, rem);
            lb->len = rem;
            return (int)n;
        }
        if (lb->len >= BUF_SIZE - 1) { errno = EMSGSIZE; return -1; }
        int n = recv(fd, lb->buf + lb->len, BUF_SIZE - 1 - lb->len, 0);
        if (n <= 0) return n;
        lb->len += n;
    }
}

/* ===== EXACT-BYTE READER (for GET) ===== */
static int recv_bytes(int fd, linebuf_t *lb, void *dst, size_t n)
{
    char *p = dst;
    size_t got = 0;
    while (got < n && lb->len > 0) {
        size_t take = (n - got < (size_t)lb->len) ? (n - got) : (size_t)lb->len;
        memcpy(p + got, lb->buf, take);
        memmove(lb->buf, lb->buf + take, lb->len - take);
        lb->len -= take;
        got += take;
    }
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r <= 0) return (int)r;
        got += r;
    }
    return (int)got;
}

/* ===== SEND HELPERS ===== */
static void send_line(int fd, const char *fmt, ...)
{
    char body[BUF_SIZE], full[BUF_SIZE + 4];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    int n = snprintf(full, sizeof(full), "%s\n", body);
    send(fd, full, n, MSG_NOSIGNAL);
}

static int check_sid(const char *line)
{
    return strstr(line, SID_TAG) != NULL;
}

static void print_reply(const char *line)
{
    if (check_sid(line)) printf("<< %s\n", line);
    else                 printf("<< %s   [WARN: missing SID]\n", line);
    fflush(stdout);
}

/* ===== SIMPLE COMMANDS ===== */
static void cmd_simple(int fd, linebuf_t *lb, const char *cmd)
{
    send_line(fd, "%s", cmd);
    char line[BUF_SIZE];
    int n = read_line(fd, lb, line, sizeof(line));
    if (n <= 0) { fprintf(stderr, "no response\n"); return; }
    print_reply(line);
}

/* ===== PUT ===== */
static void cmd_put(int fd, linebuf_t *lb, const char *local, const char *remote)
{
    struct stat st;
    if (stat(local, &st) < 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "PUT: cannot stat %s\n", local); return;
    }

    send_line(fd, "PUT %s %ld", remote, (long)st.st_size);

    int lfd = open(local, O_RDONLY);
    if (lfd < 0) { perror("open"); return; }

    char buf[BUF_SIZE];
    long rem = st.st_size;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (rem > 0) {
        ssize_t r = read(lfd, buf, sizeof(buf));
        if (r <= 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = send(fd, buf + off, r - off, MSG_NOSIGNAL);
            if (w <= 0) { close(lfd); return; }
            off += w;
        }
        rem -= r;
    }
    close(lfd);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) +
                  (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (secs <= 0) secs = 0.001;
    printf("[throughput] PUT %.0f bytes/s\n", st.st_size / secs);

    char line[BUF_SIZE];
    int n = read_line(fd, lb, line, sizeof(line));
    if (n > 0) print_reply(line);
}

/* ===== GET ===== */
static void cmd_get(int fd, linebuf_t *lb, const char *remote, const char *local)
{
    send_line(fd, "GET %s", remote);

    char line[BUF_SIZE];
    int n = read_line(fd, lb, line, sizeof(line));
    if (n <= 0) { fprintf(stderr, "GET: no response\n"); return; }

    if (strncmp(line, "OK FILE_SEND ", 13) != 0) {
        print_reply(line);
        return;
    }

    char name[256];
    long size;
    if (sscanf(line + 13, "%255s %ld", name, &size) != 2) {
        fprintf(stderr, "GET: bad header: %s\n", line);
        return;
    }
    printf("<< %s\n", line);

    int ofd = open(local, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) { perror("open local"); return; }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    char buf[BUF_SIZE];
    long rem = size;
    while (rem > 0) {
        size_t chunk = (rem < (long)sizeof(buf)) ? (size_t)rem : sizeof(buf);
        int r = recv_bytes(fd, lb, buf, chunk);
        if (r <= 0) { close(ofd); return; }
        if (write(ofd, buf, chunk) != (ssize_t)chunk) { close(ofd); return; }
        rem -= chunk;
    }
    close(ofd);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) +
                  (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (secs <= 0) secs = 0.001;
    printf("[throughput] GET %.0f bytes/s\n", size / secs);
    printf("Saved %s (%ld bytes)\n", local, size);
}

/* ===== MONITOR ===== */
static void cmd_monitor(int fd, linebuf_t *lb, const char *arg)
{
    if (strcasecmp(arg, "start") == 0) {
        char sub[BUF_SIZE];
        printf("UDP listen port (e.g. 9500): ");
        fflush(stdout);
        if (!fgets(sub, sizeof(sub), stdin)) return;
        sub[strcspn(sub, "\n")] = '\0';
        int port = atoi(sub);
        if (port <= 0 || port > 65535) { fprintf(stderr, "bad port\n"); return; }
        if (start_udp_listener(port) < 0) return;

        char cmdline[64];
        snprintf(cmdline, sizeof(cmdline), "MONITOR START %d", port);
        cmd_simple(fd, lb, cmdline);
    }
    else if (strcasecmp(arg, "stop") == 0) {
        cmd_simple(fd, lb, "MONITOR STOP");
        stop_udp_listener();
    }
    else {
        fprintf(stderr, "usage: monitor start|stop\n");
    }
}

/* ===== HELP ===== */
static void show_help(void)
{
    printf(
        "Commands:\n"
        "  auth <token>          e.g. auth OPS-1899\n"
        "  sysinfo\n"
        "  listproc\n"
        "  exec <name>           DATE | UPTIME | DISKFREE | HOSTNAME | WHOAMI\n"
        "  put <local> <remote>\n"
        "  get <remote> <local>\n"
        "  monitor start         (prompts for UDP port)\n"
        "  monitor stop\n"
        "  quit\n"
        "  help\n");
}

/* ===== MAIN LOOP ===== */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <agent-ip>\n", argv[0]);
        return 1;
    }
    const char *ip = argv[1];

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port   = htons(AGENT_PORT);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        fprintf(stderr, "bad ip %s\n", ip); close(fd); return 1;
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("connect"); close(fd); return 1;
    }
    printf("Connected to Agent %s:%d (SID expected SID:9981)\n\n", ip, AGENT_PORT);
    show_help();
    printf("\n");

    linebuf_t lb = { .len = 0 };
    char in[BUF_SIZE];

    while (1) {
        printf("controller> ");
        fflush(stdout);
        if (!fgets(in, sizeof(in), stdin)) break;
        in[strcspn(in, "\n")] = '\0';
        if (in[0] == '\0') continue;

        char *save = NULL;
        char *cmd = strtok_r(in, " ", &save);
        if (!cmd) continue;

        if (strcasecmp(cmd, "quit") == 0 || strcasecmp(cmd, "exit") == 0) {
            cmd_simple(fd, &lb, "QUIT");
            break;
        }
        else if (strcasecmp(cmd, "help") == 0) {
            show_help();
        }
        else if (strcasecmp(cmd, "auth") == 0) {
            char *tok = strtok_r(NULL, " ", &save);
            if (!tok) { fprintf(stderr, "usage: auth <token>\n"); continue; }
            char line[128];
            snprintf(line, sizeof(line), "AUTH %s", tok);
            cmd_simple(fd, &lb, line);
        }
        else if (strcasecmp(cmd, "sysinfo") == 0) {
            cmd_simple(fd, &lb, "SYSINFO");
        }
        else if (strcasecmp(cmd, "listproc") == 0) {
            cmd_simple(fd, &lb, "LISTPROC");
        }
        else if (strcasecmp(cmd, "exec") == 0) {
            char *name = strtok_r(NULL, " ", &save);
            if (!name) { fprintf(stderr, "usage: exec <name>\n"); continue; }
            char line[128];
            snprintf(line, sizeof(line), "EXEC %s", name);
            cmd_simple(fd, &lb, line);
        }
        else if (strcasecmp(cmd, "put") == 0) {
            char *l = strtok_r(NULL, " ", &save);
            char *r = strtok_r(NULL, " ", &save);
            if (!l || !r) { fprintf(stderr, "usage: put <local> <remote>\n"); continue; }
            cmd_put(fd, &lb, l, r);
        }
        else if (strcasecmp(cmd, "get") == 0) {
            char *r = strtok_r(NULL, " ", &save);
            char *l = strtok_r(NULL, " ", &save);
            if (!r || !l) { fprintf(stderr, "usage: get <remote> <local>\n"); continue; }
            cmd_get(fd, &lb, r, l);
        }
        else if (strcasecmp(cmd, "monitor") == 0) {
            char *arg = strtok_r(NULL, " ", &save);
            if (!arg) { fprintf(stderr, "usage: monitor start|stop\n"); continue; }
            cmd_monitor(fd, &lb, arg);
        }
        else {
            fprintf(stderr, "unknown command: %s (try 'help')\n", cmd);
        }
    }

    stop_udp_listener();
    close(fd);
    printf("Bye.\n");
    return 0;
}
