// schedule_server.c - Tiny HTTP server in C for APD schedules
// Endpoints:
//   GET  /api/schedule   -> text/plain schedule file (7 lines)
//   POST /api/schedule   -> text/plain schedule file contents to save
//   GET  /api/next       -> application/json {"ok":true,"next_epoch":...,"next_label":"..."}
//   GET  /api/ping       -> application/json {"ok":true,"msg":"pong"}
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

static const char *DAYS[7] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};

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

static bool valid_hhmm(const char *s) {
    if (!s) return false;
    if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) return false;
    if (s[2] != ':') return false;
    if (!isdigit((unsigned char)s[3]) || !isdigit((unsigned char)s[4])) return false;
    if (s[5] != '\0' && !isspace((unsigned char)s[5])) return false;

    int hh = (s[0]-'0')*10 + (s[1]-'0');
    int mm = (s[3]-'0')*10 + (s[4]-'0');
    return (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59);
}

static void ensure_default_schedule_file(void) {
    char path[512];
    get_schedule_path(path, sizeof(path));

    struct stat st;
    if (stat(path, &st) == 0) return;

    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i=0;i<7;i++) {
        fprintf(f, "%s 08:00 20:00\n", DAYS[i]);
    }
    fclose(f);
}

static bool read_file_to_buf(const char *path, char *buf, size_t bufsz) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, bufsz-1, f);
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
    if (ferror(f)) { fclose(f); unlink(tmp); return false; }
    fclose(f);

    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

static bool validate_schedule_text(const char *body) {
    // Must contain at least 7 valid day lines:
    // Mon HH:MM HH:MM
    // ...
    // We accept extra whitespace.
    int seen[7] = {0};

    char *copy = strdup(body ? body : "");
    if (!copy) return false;

    char *saveptr = NULL;
    char *line = strtok_r(copy, "\n", &saveptr);
    while (line) {
        trim(line);
        if (!line[0]) { line = strtok_r(NULL, "\n", &saveptr); continue; }

        char day[16]={0}, t1[16]={0}, t2[16]={0};
        if (sscanf(line, "%15s %15s %15s", day, t1, t2) == 3) {
            for (int i=0;i<7;i++) {
                if (strcmp(day, DAYS[i])==0) {
                    // ensure tokens are exactly HH:MM
                    t1[5]=0; t2[5]=0;
                    if (strlen(t1)==5 && strlen(t2)==5 && valid_hhmm(t1) && valid_hhmm(t2)) {
                        seen[i] = 1;
                    }
                    break;
                }
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(copy);

    for (int i=0;i<7;i++) if (!seen[i]) return false;
    return true;
}

static void compute_next_event(const char *sched_text, long *out_epoch, char *out_label, size_t out_label_sz) {
    // Parse schedule into arrays [7][2]
    int hh[7][2], mm[7][2];
    for (int d=0; d<7; d++) { hh[d][0]=8; mm[d][0]=0; hh[d][1]=20; mm[d][1]=0; }

    char *copy = strdup(sched_text ? sched_text : "");
    if (copy) {
        char *saveptr=NULL;
        char *line=strtok_r(copy, "\n", &saveptr);
        while (line) {
            trim(line);
            if (!line[0]) { line=strtok_r(NULL,"\n",&saveptr); continue; }

            char day[16]={0}, t1[16]={0}, t2[16]={0};
            if (sscanf(line, "%15s %15s %15s", day, t1, t2) == 3) {
                for (int d=0; d<7; d++) {
                    if (strcmp(day, DAYS[d])==0) {
                        if (strlen(t1)>=5 && strlen(t2)>=5) {
                            t1[5]=0; t2[5]=0;
                            if (valid_hhmm(t1) && valid_hhmm(t2)) {
                                hh[d][0] = (t1[0]-'0')*10 + (t1[1]-'0');
                                mm[d][0] = (t1[3]-'0')*10 + (t1[4]-'0');
                                hh[d][1] = (t2[0]-'0')*10 + (t2[1]-'0');
                                mm[d][1] = (t2[3]-'0')*10 + (t2[4]-'0');
                            }
                        }
                        break;
                    }
                }
            }
            line=strtok_r(NULL,"\n",&saveptr);
        }
        free(copy);
    }

    time_t now = time(NULL);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    long best = -1;
    char best_label[128]={0};

    // Linux tm_wday: Sun=0..Sat=6. Convert to Mon=0..Sun=6
    int today_mon0 = (now_tm.tm_wday + 6) % 7;

    for (int day_off=0; day_off<8; day_off++) {
        int d = (today_mon0 + day_off) % 7;

        for (int slot=0; slot<2; slot++) {
            // Start from "now" + day_off days, then set HH:MM:00
            time_t base = now + (time_t)day_off * 24 * 3600;
            struct tm cand;
            localtime_r(&base, &cand);
            cand.tm_hour = hh[d][slot];
            cand.tm_min  = mm[d][slot];
            cand.tm_sec  = 0;

            time_t cand_epoch = mktime(&cand);
            if (cand_epoch <= now) continue;

            if (best < 0 || cand_epoch < best) {
                best = (long)cand_epoch;
                snprintf(best_label, sizeof(best_label), "%s slot%d %02d:%02d",
                         DAYS[d], slot+1, hh[d][slot], mm[d][slot]);
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
    const char *msg = (code==200) ? "OK" :
                      (code==400) ? "Bad Request" :
                      (code==404) ? "Not Found" :
                      (code==500) ? "Internal Server Error" :
                      (code==502) ? "Bad Gateway" : "OK";

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
    int r = (int)read(cfd, req, sizeof(req)-1);
    if (r <= 0) return;
    req[r] = 0;

    // Find request line
    char method[8]={0}, path[256]={0};
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        send_http(cfd, 400, "text/plain", "bad request\n");
        return;
    }

    // Body (if any)
    const char *body_ptr = strstr(req, "\r\n\r\n");
    int content_len = parse_content_length(req);
    char body[MAX_BODY];
    body[0]=0;

    if (body_ptr) {
        body_ptr += 4;
        int already = r - (int)(body_ptr - req);
        if (content_len > 0) {
            if (content_len >= (int)sizeof(body)) content_len = (int)sizeof(body)-1;
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

    if (strcmp(method, "GET")==0 && strcmp(path, "/api/schedule")==0) {
        char filebuf[4096];
        if (!read_file_to_buf(sched_path, filebuf, sizeof(filebuf))) {
            send_http(cfd, 500, "text/plain", "failed to read schedule\n");
            return;
        }
        send_http(cfd, 200, "text/plain", filebuf);
        return;
    }

    if (strcmp(method, "POST")==0 && strcmp(path, "/api/schedule")==0) {
        if (!body[0]) {
            send_http(cfd, 400, "text/plain", "missing body\n");
            return;
        }
        if (!validate_schedule_text(body)) {
            send_http(cfd, 400, "text/plain", "invalid schedule format\nexpected 7 lines like: Mon 08:00 20:00\n");
            return;
        }
        if (!write_buf_to_file_atomic(sched_path, body)) {
            send_http(cfd, 500, "text/plain", "failed to write schedule\n");
            return;
        }
        send_http(cfd, 200, "application/json", "{\"ok\":true}");
        return;
    }

    if (strcmp(method, "GET")==0 && strcmp(path, "/api/next")==0) {
        char filebuf[4096];
        if (!read_file_to_buf(sched_path, filebuf, sizeof(filebuf))) {
            send_http(cfd, 500, "application/json", "{\"ok\":false,\"error\":\"read failed\"}");
            return;
        }
        long next_epoch=-1;
        char label[128]={0};
        compute_next_event(filebuf, &next_epoch, label, sizeof(label));

        char out[512];
        snprintf(out, sizeof(out),
                 "{\"ok\":true,\"next_epoch\":%ld,\"next_label\":\"%s\"}",
                 next_epoch, label);
        send_http(cfd, 200, "application/json", out);
        return;
    }

    if (strcmp(method, "GET")==0 && strcmp(path, "/api/ping")==0) {
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

