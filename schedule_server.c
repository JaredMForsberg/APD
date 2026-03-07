// schedule_server.c - Tiny HTTP server in C for APD schedules
// Endpoints:
//   GET  /api/schedule   -> text/plain schedule file
//   POST /api/schedule   -> text/plain schedule file contents to save
//   GET  /api/next       -> application/json {"ok":true,"next_epoch":...,"next_label":"..."}
//   GET  /api/ping       -> application/json {"ok":true,"msg":"pong"}
//
// NEW FORMAT PER DAY:
// Mon 1:08:00 0:08:00 0:08:00 0:08:00 0:08:00 0:08:00 0:08:00 0:08:00 0:08:00 0:08:00 | 1:20:00 0:20:00 0:20:00 0:20:00 0:20:00 0:20:00 0:20:00 0:20:00 0:20:00 0:20:00
//
// Each token is:
// enabled:HH:MM
//
// Build:
//   gcc -O2 -Wall -Wextra -std=gnu99 schedule_server.c -o schedule_server
//
// Run:
//   ./schedule_server
//
// Default port: 5050

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PORT 5050
#define MAX_REQ (64 * 1024)
#define MAX_BODY (64 * 1024)
#define SCHEDULE_FILE ".pill_dispenser_schedule.txt"
#define MAX_TIMES 10

static const char *DAYS[7] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};

typedef struct {
    int enabled;
    int h;
    int m;
} TimeEntry;

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s+i, strlen(s+i)+1);
}

static void get_schedule_path(char *out, size_t outsz) {
    const char *home = getenv("HOME");
    if (!home) home = "/home/pi";
    snprintf(out, outsz, "%s/%s", home, SCHEDULE_FILE);
}

static bool valid_hhmm_parts(int hh, int mm) {
    return (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59);
}

static bool parse_enabled_hhmm(const char *tok, int *enabled, int *hh, int *mm) {
    if (!tok || !enabled || !hh || !mm) return false;

    int en = -1, h = -1, m = -1;
    if (sscanf(tok, "%d:%d:%d", &en, &h, &m) != 3) return false;
    if (!(en == 0 || en == 1)) return false;
    if (!valid_hhmm_parts(h, m)) return false;

    *enabled = en;
    *hh = h;
    *mm = m;
    return true;
}

static void ensure_default_schedule_file(void) {
    char path[512];
    get_schedule_path(path, sizeof(path));

    struct stat st;
    if (stat(path, &st) == 0) return;

    FILE *f = fopen(path, "w");
    if (!f) return;

    for (int d = 0; d < 7; d++) {
        fprintf(f, "%s ", DAYS[d]);

        for (int i = 0; i < MAX_TIMES; i++) {
            if (i == 0) fprintf(f, "1:08:00 ");
            else        fprintf(f, "0:08:00 ");
        }

        fprintf(f, "| ");

        for (int i = 0; i < MAX_TIMES; i++) {
            if (i == 0) fprintf(f, "1:20:00 ");
            else        fprintf(f, "0:20:00 ");
        }

        fprintf(f, "\n");
    }

    fclose(f);
}

static bool read_file_to_buf(const char *path, char *buf, size_t bufsz) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, bufsz - 1, f);
    buf[n] = 0;
    fclose(f);
    return true;
}

static bool write_buf_to_file_atomic(const char *path, const char *buf) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs(buf, f);
    if (ferror(f)) {
        fclose(f);
        unlink(tmp);
        return false;
    }
    fclose(f);

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static int day_index_from_name(const char *day) {
    for (int i = 0; i < 7; i++) {
        if (strcmp(day, DAYS[i]) == 0) return i;
    }
    return -1;
}

static bool validate_schedule_line(const char *line_in) {
    char line[2048];
    strncpy(line, line_in, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    trim(line);
    if (!line[0]) return false;

    char *saveptr = NULL;
    char *tok = strtok_r(line, " \t", &saveptr);
    if (!tok) return false;

    if (day_index_from_name(tok) < 0) return false;

    for (int slot = 0; slot < 2; slot++) {
        for (int i = 0; i < MAX_TIMES; i++) {
            tok = strtok_r(NULL, " \t", &saveptr);
            if (!tok) return false;

            if (slot == 1 && i == 0 && strcmp(tok, "|") == 0) {
                tok = strtok_r(NULL, " \t", &saveptr);
                if (!tok) return false;
            }

            int en, hh, mm;
            if (!parse_enabled_hhmm(tok, &en, &hh, &mm)) return false;
        }

        if (slot == 0) {
            tok = strtok_r(NULL, " \t", &saveptr);
            if (!tok || strcmp(tok, "|") != 0) return false;
        }
    }

    return true;
}

static bool validate_schedule_text(const char *body) {
    int seen[7] = {0};

    char *copy = strdup(body ? body : "");
    if (!copy) return false;

    char *saveptr = NULL;
    char *line = strtok_r(copy, "\n", &saveptr);

    while (line) {
        trim(line);
        if (!line[0]) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        char day[16] = {0};
        if (sscanf(line, "%15s", day) == 1) {
            int d = day_index_from_name(day);
            if (d >= 0 && validate_schedule_line(line)) {
                seen[d] = 1;
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(copy);

    for (int i = 0; i < 7; i++) {
        if (!seen[i]) return false;
    }
    return true;
}

static void compute_next_event(const char *sched_text, long *out_epoch, char *out_label, size_t out_label_sz) {
    TimeEntry sched[7][2][MAX_TIMES];

    for (int d = 0; d < 7; d++) {
        for (int s = 0; s < 2; s++) {
            for (int i = 0; i < MAX_TIMES; i++) {
                sched[d][s][i].enabled = 0;
                sched[d][s][i].h = 8;
                sched[d][s][i].m = 0;
            }
        }
        sched[d][0][0].enabled = 1;
        sched[d][0][0].h = 8;
        sched[d][0][0].m = 0;

        sched[d][1][0].enabled = 1;
        sched[d][1][0].h = 20;
        sched[d][1][0].m = 0;
    }

    char *copy = strdup(sched_text ? sched_text : "");
    if (copy) {
        char *save_lines = NULL;
        char *line = strtok_r(copy, "\n", &save_lines);

        while (line) {
            char linebuf[2048];
            strncpy(linebuf, line, sizeof(linebuf) - 1);
            linebuf[sizeof(linebuf) - 1] = 0;
            trim(linebuf);

            if (linebuf[0]) {
                char *save_tok = NULL;
                char *tok = strtok_r(linebuf, " \t", &save_tok);
                if (tok) {
                    int d = day_index_from_name(tok);
                    if (d >= 0) {
                        for (int s = 0; s < 2; s++) {
                            for (int i = 0; i < MAX_TIMES; i++) {
                                tok = strtok_r(NULL, " \t", &save_tok);
                                if (!tok) break;

                                if (s == 1 && i == 0 && strcmp(tok, "|") == 0) {
                                    tok = strtok_r(NULL, " \t", &save_tok);
                                    if (!tok) break;
                                }

                                int en, hh, mm;
                                if (parse_enabled_hhmm(tok, &en, &hh, &mm)) {
                                    sched[d][s][i].enabled = en;
                                    sched[d][s][i].h = hh;
                                    sched[d][s][i].m = mm;
                                }
                            }
                            if (s == 0) {
                                tok = strtok_r(NULL, " \t", &save_tok);
                                if (!tok || strcmp(tok, "|") != 0) {
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            line = strtok_r(NULL, "\n", &save_lines);
        }

        free(copy);
    }

    time_t now = time(NULL);
    struct tm now_tm;
    localtime_r(&now, &now_tm);

    long best = -1;
    char best_label[128] = {0};

    int today_mon0 = (now_tm.tm_wday + 6) % 7;

    for (int day_off = 0; day_off < 8; day_off++) {
        int d = (today_mon0 + day_off) % 7;

        for (int slot = 0; slot < 2; slot++) {
            for (int i = 0; i < MAX_TIMES; i++) {
                if (!sched[d][slot][i].enabled) continue;

                time_t base = now + (time_t)day_off * 24 * 3600;
                struct tm cand;
                localtime_r(&base, &cand);
                cand.tm_hour = sched[d][slot][i].h;
                cand.tm_min  = sched[d][slot][i].m;
                cand.tm_sec  = 0;

                time_t cand_epoch = mktime(&cand);
                if (cand_epoch <= now) continue;

                if (best < 0 || cand_epoch < best) {
                    best = (long)cand_epoch;
                    snprintf(best_label, sizeof(best_label),
                             "%s slot%d %02d:%02d",
                             DAYS[d], slot + 1, sched[d][slot][i].h, sched[d][slot][i].m);
                }
            }
        }
    }

    *out_epoch = best;
    if (out_label && out_label_sz) {
        snprintf(out_label, out_label_sz, "%s", best_label[0] ? best_label : "");
    }
}

static void send_http(int fd, int code, const char *ctype, const char *body) {
    if (!ctype) ctype = "text/plain";
    if (!body) body = "";

    char header[512];
    int blen = (int)strlen(body);
    const char *msg = (code == 200) ? "OK" :
                      (code == 400) ? "Bad Request" :
                      (code == 404) ? "Not Found" :
                      (code == 500) ? "Internal Server Error" :
                      (code == 502) ? "Bad Gateway" : "OK";

    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n",
        code, msg, ctype, blen);

    (void)write(fd, header, (size_t)n);
    (void)write(fd, body, (size_t)blen);
}

static int parse_content_length(const char *req) {
    const char *p = strcasestr(req, "\r\nContent-Length:");
    if (!p) p = strcasestr(req, "\nContent-Length:");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return atoi(p);
}

static void handle_client(int cfd) {
    char req[MAX_REQ];
    int r = (int)read(cfd, req, sizeof(req) - 1);
    if (r <= 0) return;
    req[r] = 0;

    char method[8] = {0}, path[256] = {0};
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        send_http(cfd, 400, "text/plain", "bad request\n");
        return;
    }

    const char *body_ptr = strstr(req, "\r\n\r\n");
    int content_len = parse_content_length(req);
    char body[MAX_BODY];
    body[0] = 0;

    if (body_ptr) {
        body_ptr += 4;
        int already = r - (int)(body_ptr - req);
        if (content_len > 0) {
            if (content_len >= (int)sizeof(body)) content_len = (int)sizeof(body) - 1;
            int tocopy = already < content_len ? already : content_len;
            if (tocopy > 0) memcpy(body, body_ptr, (size_t)tocopy);

            int got = tocopy;
            while (got < content_len) {
                int rr = (int)read(cfd, body + got, (size_t)(content_len - got));
                if (rr <= 0) break;
                got += rr;
            }
            body[got] = 0;
        }
    }

    ensure_default_schedule_file();

    char sched_path[512];
    get_schedule_path(sched_path, sizeof(sched_path));

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/schedule") == 0) {
        char filebuf[16384];
        if (!read_file_to_buf(sched_path, filebuf, sizeof(filebuf))) {
            send_http(cfd, 500, "text/plain", "failed to read schedule\n");
            return;
        }
        send_http(cfd, 200, "text/plain", filebuf);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/schedule") == 0) {
        if (!body[0]) {
            send_http(cfd, 400, "text/plain", "missing body\n");
            return;
        }
        if (!validate_schedule_text(body)) {
            send_http(cfd, 400, "text/plain",
                      "invalid schedule format\n"
                      "expected 7 lines like:\n"
                      "Mon 1:08:00 0:08:00 ... | 1:20:00 0:20:00 ...\n");
            return;
        }
        if (!write_buf_to_file_atomic(sched_path, body)) {
            send_http(cfd, 500, "text/plain", "failed to write schedule\n");
            return;
        }
        send_http(cfd, 200, "application/json", "{\"ok\":true}");
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/next") == 0) {
        char filebuf[16384];
        if (!read_file_to_buf(sched_path, filebuf, sizeof(filebuf))) {
            send_http(cfd, 500, "application/json", "{\"ok\":false,\"error\":\"read failed\"}");
            return;
        }

        long next_epoch = -1;
        char label[128] = {0};
        compute_next_event(filebuf, &next_epoch, label, sizeof(label));

        char out[512];
        snprintf(out, sizeof(out),
                 "{\"ok\":true,\"next_epoch\":%ld,\"next_label\":\"%s\"}",
                 next_epoch, label);
        send_http(cfd, 200, "application/json", out);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/ping") == 0) {
        send_http(cfd, 200, "application/json", "{\"ok\":true,\"msg\":\"pong\"}");
        return;
    }

    send_http(cfd, 404, "text/plain", "not found\n");
}

int main(void) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(sfd);
        return 1;
    }

    if (listen(sfd, 16) != 0) {
        perror("listen");
        close(sfd);
        return 1;
    }

    printf("schedule_server listening on 0.0.0.0:%d\n", PORT);

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(sfd, (struct sockaddr*)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_client(cfd);
        close(cfd);
    }

    close(sfd);
    return 0;
}
