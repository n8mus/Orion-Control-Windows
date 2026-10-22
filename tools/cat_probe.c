// SPDX-License-Identifier: GPL-2.0-or-later
// Ground-truth CAT probe: send each argument as a command (CR appended) and print
// every raw response line for a settling window. For mapping real Orion firmware
// behavior (response formats, filter/PBT semantics per mode) against the manual.
//
//   ./cat_probe /dev/orion "?RMM" "?RMF" "?RMP"
//   ./cat_probe /dev/orion "*RMP300" "?RMP" "*RMP0"     # set, read back, restore
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
    struct termios t;
    if (tcgetattr(fd, &t)) { perror("tcgetattr"); close(fd); return -1; }
    cfmakeraw(&t);
    cfsetispeed(&t, B57600); cfsetospeed(&t, B57600);
    t.c_cflag |= (CLOCAL | CREAD);
    t.c_cflag &= ~CSTOPB; t.c_cflag &= ~PARENB;
    t.c_cflag = (t.c_cflag & ~CSIZE) | CS8;
    t.c_cflag &= ~CRTSCTS;
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t)) { perror("tcsetattr"); close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

// Print raw response lines (CR-delimited) arriving within window_ms.
static void drain(int fd, double window_ms) {
    char buf[512]; int len = 0;
    double start = now_ms();
    while (now_ms() - start < window_ms) {
        fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
        struct timeval tv = {0, 5000};
        if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) continue;
        char c; ssize_t n = read(fd, &c, 1);
        if (n != 1) continue;
        if (c == '\r' || c == '\n') {
            if (len) { buf[len] = 0; printf("    <- \"%s\"  (+%.0f ms)\n", buf, now_ms() - start); len = 0; }
        } else if (len < (int)sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
    if (len) { buf[len] = 0; printf("    <- (partial) \"%s\"\n", buf); }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <device> [-burst] <cmd> [cmd...]\n", argv[0]);
        return 1;
    }
    int burst = argc > 2 && strcmp(argv[2], "-burst") == 0;
    int first = burst ? 3 : 2;

    int fd = open_serial(argv[1]);
    if (fd < 0) return 1;

    if (burst) {
        // Send everything back-to-back (like the app's startup/poll bursts),
        // then read: shows whether the radio glues or drops responses.
        for (int i = first; i < argc; ++i) {
            char cmd[128];
            snprintf(cmd, sizeof cmd, "%s\r", argv[i]);
            printf("-> %s\n", argv[i]);
            if (write(fd, cmd, strlen(cmd)) < 0) perror("write");
        }
        drain(fd, 1200);
    } else {
        for (int i = first; i < argc; ++i) {
            char cmd[128];
            snprintf(cmd, sizeof cmd, "%s\r", argv[i]);
            printf("-> %s\n", argv[i]);
            if (write(fd, cmd, strlen(cmd)) < 0) perror("write");
            drain(fd, 400);        // generous window past the ~110 ms firmware tail
        }
    }
    close(fd);
    return 0;
}
