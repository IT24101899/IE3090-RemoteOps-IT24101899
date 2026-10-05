/*
 * RemoteOps Agent — IE3090 Network Programming
 * Registration: IT24101899
 * Personalised: Port 9410, SID 9981, Token OPS-1899
 * Log: remoteops_IT24101899.log
 * Storage: ./agentfiles/IT24101899/
 *
 * Compile: make -f Makefile_899
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
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ===== PERSONALISED CONSTANTS ===== */
#define AGENT_PORT       9410
#define SID_TAG          "9981"
#define TOKEN            "OPS-1899"
#define LOG_FILE         "remoteops_IT24101899.log"
#define STORE_DIR        "./agentfiles/IT24101899"
#define MAX_UPLOAD_MB    10
#define MAX_UPLOAD_BYTES (MAX_UPLOAD_MB * 1024 * 1024)
#define MON_INTERVAL_SEC 2
#define BUF_SIZE         4096
#define MAX_CLIENTS      64

/* ===== GLOBAL LOG ===== */
static FILE *g_logfp = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    time_t now = time(NULL);
    struct tm tm_info;
    char ts[32];
    localtime_r(&now, &tm_info);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_info);

    pthread_mutex_lock(&g_log_mutex);
    if (g_logfp) {
        fprintf(g_logfp, "[%s] ", ts);
        va_start(ap, fmt);
        vfprintf(g_logfp, fmt, ap);
        va_end(ap);
        fprintf(g_logfp, "\n");
        fflush(g_logfp);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

/* ===== SESSION ===== */
typedef struct {
    int   fd;
    char  peer[INET_ADDRSTRLEN];
    int   peer_port;
    int   authed;
    int   mon_active;
    int   mon_stop;
    int   udp_fd;
    struct sockaddr_in ctrl_udp;
    pthread_t mon_tid;
    pthread_mutex_t mon_mutex;
    char  rxbuf[BUF_SIZE];
    int   rxlen;
} session_t;

/* ===== REPLY (appends SID in one place) ===== */
static void reply(int fd, const char *fmt, ...)
{
    char body[BUF_SIZE];
    char full[BUF_SIZE + 64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    snprintf(full, sizeof(full), "%s SID:%s\n", body, SID_TAG);
    send(fd, full, strlen(full), MSG_NOSIGNAL);
}

/* ===== LINE FRAMING ===== */
static int read_line(session_t *s, char *out, size_t outsz)
{
    while (1) {
        char *nl = memchr(s->rxbuf, '\n', s->rxlen);
        if (nl) {
            size_t linelen = nl - s->rxbuf;
            if (linelen >= outsz) linelen = outsz - 1;
            memcpy(out, s->rxbuf, linelen);
            out[linelen] = '\0';
            int rem = s->rxlen - (nl - s->rxbuf) - 1;
            memmove(s->rxbuf, nl + 1, rem);
            s->rxlen = rem;
            return (int)linelen;
        }
        if (s->rxlen >= BUF_SIZE - 1) { errno = EMSGSIZE; return -1; }
        int n = recv(s->fd, s->rxbuf + s->rxlen, BUF_SIZE - 1 - s->rxlen, 0);
        if (n <= 0) return n;
        s->rxlen += n;
    }
}

static int recv_bytes(session_t *s, void *dst, size_t n)
{
    char *p = dst;
    size_t got = 0;
    while (got < n && s->rxlen > 0) {
        size_t take = (n - got < (size_t)s->rxlen) ? (n - got) : (size_t)s->rxlen;
        memcpy(p + got, s->rxbuf, take);
        memmove(s->rxbuf, s->rxbuf + take, s->rxlen - take);
        s->rxlen -= take;
        got += take;
    }
    while (got < n) {
        ssize_t r = recv(s->fd, p + got, n - got, 0);
        if (r <= 0) return (int)r;
        got += r;
    }
    return (int)got;
}

/* ===== SYSTEM INFO (reads /proc) ===== */
static void get_sysinfo(double *cpu, long *mem_mb, long *uptime)
{
    FILE *f = fopen("/proc/loadavg", "r");
    *cpu = 0.0;
    if (f) { fscanf(f, "%lf", cpu); fclose(f); }

    *mem_mb = 0;
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        long total = 0, avail = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %ld kB", &total) == 1) continue;
            if (sscanf(line, "MemAvailable: %ld kB", &avail) == 1) break;
        }
        fclose(f);
        if (total > 0) *mem_mb = (total - avail) / 1024;
    }

    *uptime = 0;
    f = fopen("/proc/uptime", "r");
    if (f) {
        double up = 0.0;
        if (fscanf(f, "%lf", &up) == 1) *uptime = (long)up;
        fclose(f);
    }
}

static void do_sysinfo(session_t *s)
{
    double cpu; long mem, up;
    get_sysinfo(&cpu, &mem, &up);
    reply(s->fd, "OK SYSINFO %.2f %ld %ld", cpu, mem, up);
    logmsg("SYSINFO -> cpu=%.2f mem=%ld uptime=%ld", cpu, mem, up);
}

static void do_listproc(session_t *s)
{
    FILE *p = popen("ps -eo pid=,comm= --no-headers 2>/dev/null | head -40", "r");
    if (!p) { reply(s->fd, "ERR 011 INTERNAL_ERROR"); return; }
    char out[BUF_SIZE];
    size_t pos = 0;
    out[0] = '\0';
    char line[256];
    while (fgets(line, sizeof(line), p)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') continue;
        for (char *q = line; *q; q++)
            if (*q == ' ' || *q == ',') *q = '_';
        int need = snprintf(NULL, 0, "%s%s", pos ? "," : "", line);
        if (pos + need + 1 >= sizeof(out)) break;
        pos += sprintf(out + pos, "%s%s", pos ? "," : "", line);
    }
    pclose(p);
    reply(s->fd, "OK PROCs %s", out);
    logmsg("LISTPROC -> %s", out);
}

/* ===== RESTRICTED EXEC ===== */
typedef struct { const char *name; const char *cmd; } wl_entry_t;

static const wl_entry_t WHITELIST[] = {
    { "DATE",     "date" },
    { "UPTIME",   "uptime" },
    { "DISKFREE", "df -h /" },
    { "HOSTNAME", "hostname" },
    { "WHOAMI",   "whoami" },
    { NULL, NULL }
};

static void do_exec(session_t *s, const char *name)
{
    const char *cmd = NULL;
    for (int i = 0; WHITELIST[i].name; i++) {
        if (strcmp(WHITELIST[i].name, name) == 0) {
            cmd = WHITELIST[i].cmd;
            break;
        }
    }
    if (!cmd) {
        reply(s->fd, "ERR 002 COMMAND_NOT_ALLOWED");
        logmsg("EXEC rejected: %s", name);
        return;
    }
    FILE *p = popen(cmd, "r");
    if (!p) { reply(s->fd, "ERR 011 INTERNAL_ERROR"); return; }
    char out[BUF_SIZE];
    size_t pos = 0;
    out[0] = '\0';
    char line[256];
    while (fgets(line, sizeof(line), p) && pos < sizeof(out) - 2) {
        line[strcspn(line, "\n")] = '\0';
        int n = snprintf(out + pos, sizeof(out) - pos, "%s%s",
                         pos ? " | " : "", line);
        if (n < 0) break;
        pos += n;
    }
    pclose(p);
    if (pos == 0) strcpy(out, "(no output)");
    reply(s->fd, "OK EXEC_RESULT %s", out);
    logmsg("EXEC %s -> %s", name, out);
}

/* ===== FILENAME VALIDATION ===== */
static int valid_filename(const char *n)
{
    if (!n || !*n || n[0] == '.') return 0;
    for (const char *p = n; *p; p++) {
        if (!( (*p >= 'a' && *p <= 'z') ||
               (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') ||
                *p == '.' || *p == '_' || *p == '-' ))
            return 0;
    }
    return 1;
}

/* ===== PUT / GET ===== */
static void reject_put(session_t *s, long size, int code, const char *reason)
{
    char discard[BUF_SIZE];
    long rem = size;
    while (rem > 0) {
        size_t chunk = (rem < (long)sizeof(discard)) ? (size_t)rem : sizeof(discard);
        int r = recv_bytes(s, discard, chunk);
        if (r <= 0) return;
        rem -= chunk;
    }
    reply(s->fd, "ERR %03d %s", code, reason);
}

static void do_put(session_t *s, const char *fname, long size)
{
    if (!valid_filename(fname)) {
        reject_put(s, size, 006, "INVALID_FILENAME");
        return;
    }
    if (size < 0 || size > MAX_UPLOAD_BYTES) {
        reject_put(s, size, 004, "FILE_TOO_LARGE");
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", STORE_DIR, fname);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        reject_put(s, size, 011, "INTERNAL_ERROR");
        return;
    }
    char buf[BUF_SIZE];
    long rem = size;
    while (rem > 0) {
        size_t chunk = (rem < (long)sizeof(buf)) ? (size_t)rem : sizeof(buf);
        int r = recv_bytes(s, buf, chunk);
        if (r <= 0) {
            close(fd); unlink(path);
            logmsg("PUT aborted: %s", fname);
            return;
        }
        if (write(fd, buf, chunk) != (ssize_t)chunk) {
            close(fd); unlink(path);
            reply(s->fd, "ERR 011 INTERNAL_ERROR");
            return;
        }
        rem -= chunk;
    }
    close(fd);
    reply(s->fd, "OK FILE_RECEIVED %s", fname);
    logmsg("PUT %s (%ld bytes) from %s:%d", fname, size, s->peer, s->peer_port);
}

static void do_get(session_t *s, const char *fname)
{
    if (!valid_filename(fname)) {
        reply(s->fd, "ERR 005 FILE_NOT_FOUND");
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", STORE_DIR, fname);
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) {
        reply(s->fd, "ERR 005 FILE_NOT_FOUND");
        logmsg("GET %s -> NOT FOUND", fname);
        return;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { reply(s->fd, "ERR 005 FILE_NOT_FOUND"); return; }

    char hdr[256];
    snprintf(hdr, sizeof(hdr), "OK FILE_SEND %s %ld SID:%s\n",
             fname, (long)st.st_size, SID_TAG);
    send(s->fd, hdr, strlen(hdr), MSG_NOSIGNAL);

    char buf[BUF_SIZE];
    ssize_t r;
    long total = 0;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = send(s->fd, buf + off, r - off, MSG_NOSIGNAL);
            if (w <= 0) { close(fd); logmsg("GET send aborted"); return; }
            off += w;
        }
        total += r;
    }
    close(fd);
    logmsg("GET %s (%ld bytes) to %s:%d", fname, total, s->peer, s->peer_port);
}

/* ===== UDP MONITOR ===== */
static void *monitor_thread(void *arg)
{
    session_t *s = (session_t *)arg;
    char buf[256];
    while (1) {
        pthread_mutex_lock(&s->mon_mutex);
        int stop = s->mon_stop;
        pthread_mutex_unlock(&s->mon_mutex);
        if (stop) break;

        double cpu; long mem, up;
        get_sysinfo(&cpu, &mem, &up);
        int n = snprintf(buf, sizeof(buf),
                         "SYSINFO <%.2f> <%ld> <%ld> SID:%s\n",
                         cpu, mem, up, SID_TAG);
        sendto(s->udp_fd, buf, n, 0,
               (struct sockaddr *)&s->ctrl_udp, sizeof(s->ctrl_udp));

        for (int i = 0; i < MON_INTERVAL_SEC * 10; i++) {
            usleep(100000);
            pthread_mutex_lock(&s->mon_mutex);
            stop = s->mon_stop;
            pthread_mutex_unlock(&s->mon_mutex);
            if (stop) return NULL;
        }
    }
    return NULL;
}

static void start_monitor(session_t *s, int port)
{
    if (s->mon_active) {
        reply(s->fd, "ERR 009 ALREADY_MONITORING");
        return;
    }
    if (!s->udp_fd) {
        s->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (s->udp_fd < 0) { reply(s->fd, "ERR 011 INTERNAL_ERROR"); return; }
    }
    memset(&s->ctrl_udp, 0, sizeof(s->ctrl_udp));
    s->ctrl_udp.sin_family = AF_INET;
    s->ctrl_udp.sin_port   = htons(port);
    inet_pton(AF_INET, s->peer, &s->ctrl_udp.sin_addr);

    s->mon_stop = 0;
    s->mon_active = 1;
    if (pthread_create(&s->mon_tid, NULL, monitor_thread, s) != 0) {
        s->mon_active = 0;
        reply(s->fd, "ERR 011 INTERNAL_ERROR");
        return;
    }
    reply(s->fd, "OK MONITOR_STARTED");
    logmsg("MONITOR START udp_port=%d peer=%s", port, s->peer);
}

static void stop_monitor(session_t *s)
{
    if (!s->mon_active) {
        reply(s->fd, "ERR 010 NOT_MONITORING");
        return;
    }
    pthread_mutex_lock(&s->mon_mutex);
    s->mon_stop = 1;
    pthread_mutex_unlock(&s->mon_mutex);
    pthread_join(s->mon_tid, NULL);
    s->mon_active = 0;
    reply(s->fd, "OK MONITOR_STOPPED");
    logmsg("MONITOR STOP");
}

/* ===== DISPATCH ===== */
static void handle_line(session_t *s, char *line)
{
    char *save = NULL;
    char *cmd = strtok_r(line, " ", &save);
    if (!cmd) return;

    if (strcasecmp(cmd, "AUTH") == 0) {
        if (s->authed) { reply(s->fd, "ERR 008 BAD_REQUEST"); return; }
        char *tok = strtok_r(NULL, " ", &save);
        if (!tok) { reply(s->fd, "ERR 001 AUTH_FAILED"); return; }
        if (strcmp(tok, TOKEN) == 0) {
            s->authed = 1;
            reply(s->fd, "OK AUTHENTICATED");
            logmsg("AUTH OK from %s:%d", s->peer, s->peer_port);
        } else {
            reply(s->fd, "ERR 001 AUTH_FAILED");
            logmsg("AUTH FAILED from %s:%d", s->peer, s->peer_port);
        }
        return;
    }

    if (!s->authed) {
        reply(s->fd, "ERR 003 NOT_AUTHENTICATED");
        logmsg("Pre-auth command rejected: %s", cmd);
        return;
    }

    if (strcasecmp(cmd, "SYSINFO") == 0)        do_sysinfo(s);
    else if (strcasecmp(cmd, "LISTPROC") == 0)  do_listproc(s);
    else if (strcasecmp(cmd, "EXEC") == 0) {
        char *name = strtok_r(NULL, " ", &save);
        if (!name) reply(s->fd, "ERR 002 COMMAND_NOT_ALLOWED");
        else       do_exec(s, name);
    }
    else if (strcasecmp(cmd, "PUT") == 0) {
        char *fn = strtok_r(NULL, " ", &save);
        char *sz = strtok_r(NULL, " ", &save);
        if (!fn || !sz) { reply(s->fd, "ERR 008 BAD_REQUEST"); return; }
        do_put(s, fn, atol(sz));
    }
    else if (strcasecmp(cmd, "GET") == 0) {
        char *fn = strtok_r(NULL, " ", &save);
        if (!fn) { reply(s->fd, "ERR 005 FILE_NOT_FOUND"); return; }
        do_get(s, fn);
    }
    else if (strcasecmp(cmd, "MONITOR") == 0) {
        char *sub = strtok_r(NULL, " ", &save);
        if (!sub) { reply(s->fd, "ERR 008 BAD_REQUEST"); return; }
        if (strcasecmp(sub, "START") == 0) {
            char *p = strtok_r(NULL, " ", &save);
            if (!p) { reply(s->fd, "ERR 008 BAD_REQUEST"); return; }
            start_monitor(s, atoi(p));
        } else if (strcasecmp(sub, "STOP") == 0) {
            stop_monitor(s);
        } else {
            reply(s->fd, "ERR 008 BAD_REQUEST");
        }
    }
    else if (strcasecmp(cmd, "QUIT") == 0) {
        reply(s->fd, "OK BYE");
        logmsg("QUIT from %s:%d", s->peer, s->peer_port);
    }
    else {
        reply(s->fd, "ERR 007 UNKNOWN_COMMAND");
        logmsg("Unknown command: %s", cmd);
    }
}

/* ===== CLIENT THREAD ===== */
static void *client_thread(void *arg)
{
    session_t *s = (session_t *)arg;
    char line[BUF_SIZE];

    logmsg("CONNECTED %s:%d", s->peer, s->peer_port);

    while (1) {
        int r = read_line(s, line, sizeof(line));
        if (r <= 0) break;
        logmsg("CMD [%s:%d] %s", s->peer, s->peer_port, line);
        int is_quit = (strcasecmp(line, "QUIT") == 0);
        handle_line(s, line);
        if (is_quit) break;
    }

    if (s->mon_active) {
        pthread_mutex_lock(&s->mon_mutex);
        s->mon_stop = 1;
        pthread_mutex_unlock(&s->mon_mutex);
        pthread_join(s->mon_tid, NULL);
    }
    if (s->udp_fd) close(s->udp_fd);
    close(s->fd);
    logmsg("DISCONNECTED %s:%d", s->peer, s->peer_port);
    free(s);
    return NULL;
}

/* ===== MAIN ===== */
int main(void)
{
    mkdir("./agentfiles", 0755);
    mkdir(STORE_DIR, 0755);

    g_logfp = fopen(LOG_FILE, "a");
    if (!g_logfp) { perror("log"); return 1; }
    logmsg("=== RemoteOps Agent starting port=%d SID=%s ===", AGENT_PORT, SID_TAG);

    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(AGENT_PORT);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(lfd); return 1;
    }
    if (listen(lfd, MAX_CLIENTS) < 0) { perror("listen"); return 1; }

    printf("RemoteOps Agent listening on port %d (SID %s)\n", AGENT_PORT, SID_TAG);
    fflush(stdout);
    logmsg("Listening on port %d", AGENT_PORT);

    while (1) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }

        session_t *s = calloc(1, sizeof(session_t));
        if (!s) { close(cfd); continue; }
        s->fd = cfd;
        inet_ntop(AF_INET, &ca.sin_addr, s->peer, sizeof(s->peer));
        s->peer_port = ntohs(ca.sin_port);
        pthread_mutex_init(&s->mon_mutex, NULL);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, s) != 0) {
            perror("pthread_create"); close(cfd); free(s); continue;
        }
        pthread_detach(tid);
    }

    close(lfd);
    if (g_logfp) fclose(g_logfp);
    return 0;
}
