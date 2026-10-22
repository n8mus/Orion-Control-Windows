// SPDX-License-Identifier: GPL-2.0-or-later
// Phase-0 latency probe for the Ten-Tec Orion CAT interface.
//
// Sends a read-only query (default ?RMF) repeatedly over the serial port and
// times the round trip to the '@'-prefixed response. This number decides how the
// panadapter drag-to-filter feature will feel. Read-only: never changes radio state.
//
//   cc -O2 -o latency_probe tools/latency_probe.c
//   ./latency_probe /dev/orion 50 ?RMF
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int open_serial(const char* dev) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", dev, strerror(errno)); return -1; }
    struct termios t; if (tcgetattr(fd, &t)) { perror("tcgetattr"); close(fd); return -1; }
    cfmakeraw(&t);
    cfsetispeed(&t, B57600); cfsetospeed(&t, B57600);
    t.c_cflag |= (CLOCAL | CREAD);
    t.c_cflag &= ~CSTOPB; t.c_cflag &= ~PARENB;
    t.c_cflag = (t.c_cflag & ~CSIZE) | CS8;
    t.c_cflag &= ~CRTSCTS;                 // Orion needs no handshake
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t)) { perror("tcsetattr"); close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

// Read until CR or timeout. Returns 1 on a full line, 0 on timeout.
static int read_line(int fd, char* buf, int max, double timeout_ms, double* first_byte_ms) {
    double start = now_ms(); int len = 0; *first_byte_ms = -1;
    while (now_ms() - start < timeout_ms) {
        fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
        struct timeval tv = {0, 2000};     // 2 ms slices
        int s = select(fd + 1, &r, NULL, NULL, &tv);
        if (s <= 0) continue;
        char c; ssize_t n = read(fd, &c, 1);
        if (n == 1) {
            if (*first_byte_ms < 0) *first_byte_ms = now_ms() - start;
            if (c == '\r') { buf[len] = 0; return 1; }
            if (len < max - 1) buf[len++] = c;
        }
    }
    buf[len] = 0; return 0;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

int main(int argc, char** argv) {
    const char* dev   = argc > 1 ? argv[1] : "/dev/orion";
    int         iters = argc > 2 ? atoi(argv[2]) : 50;
    const char* query = argc > 3 ? argv[3] : "?RMF";

    int fd = open_serial(dev);
    if (fd < 0) return 1;

    char cmd[64]; snprintf(cmd, sizeof cmd, "%s\r", query);
    char resp[256]; double fb;

    // Warm-up (discarded).
    (void)write(fd, cmd, strlen(cmd)); read_line(fd, resp, sizeof resp, 500, &fb);

    double* rt = calloc(iters, sizeof(double));
    int ok = 0, timeouts = 0; char lastResp[256] = "";
    for (int i = 0; i < iters; ++i) {
        tcflush(fd, TCIFLUSH);
        double t0 = now_ms();
        (void)write(fd, cmd, strlen(cmd));
        if (read_line(fd, resp, sizeof resp, 500, &fb)) {
            rt[ok++] = now_ms() - t0;
            strncpy(lastResp, resp, sizeof lastResp - 1);
        } else timeouts++;
        usleep(5000);                      // 5 ms gap between probes
    }
    close(fd);

    printf("device   : %s @ 57600 8N1\n", dev);
    printf("query    : %s   ->   response: \"%s\"\n", query, lastResp);
    printf("samples  : %d ok, %d timeouts\n", ok, timeouts);
    if (ok) {
        qsort(rt, ok, sizeof(double), cmp_d);
        double sum = 0; for (int i = 0; i < ok; ++i) sum += rt[i];
        printf("round-trip ms  min %.1f  median %.1f  mean %.1f  p90 %.1f  max %.1f\n",
               rt[0], rt[ok/2], sum/ok, rt[(int)(ok*0.9)], rt[ok-1]);
    }
    free(rt);
    return 0;
}
