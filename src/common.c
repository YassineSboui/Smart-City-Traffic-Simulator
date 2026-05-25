#define _GNU_SOURCE

#include "smartcross.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

const char *ROUTE_NAMES[MAX_ROUTES] = {"Nord", "Sud", "Est", "Ouest"};
const char *STRATEGY_NAMES[STRATEGY_COUNT] = {"fcfs", "rr", "priority", "sjf", "dynamic"};

long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

long long rusage_total_us(void) {
    struct rusage self_usage;
    struct rusage child_usage;
    getrusage(RUSAGE_SELF, &self_usage);
    getrusage(RUSAGE_CHILDREN, &child_usage);

    long long self_us = (long long)self_usage.ru_utime.tv_sec * 1000000LL + self_usage.ru_utime.tv_usec;
    self_us += (long long)self_usage.ru_stime.tv_sec * 1000000LL + self_usage.ru_stime.tv_usec;

    long long child_us = (long long)child_usage.ru_utime.tv_sec * 1000000LL + child_usage.ru_utime.tv_usec;
    child_us += (long long)child_usage.ru_stime.tv_sec * 1000000LL + child_usage.ru_stime.tv_usec;

    return self_us + child_us;
}

int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

void die(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}

const char *strategy_name(Strategy strategy) {
    if (strategy < 0 || strategy >= STRATEGY_COUNT) {
        return "unknown";
    }
    return STRATEGY_NAMES[strategy];
}

int parse_strategy(const char *value, Strategy *strategy) {
    for (int i = 0; i < STRATEGY_COUNT; i++) {
        if (strcmp(value, STRATEGY_NAMES[i]) == 0) {
            *strategy = (Strategy)i;
            return 1;
        }
    }
    return 0;
}

int route_opposite(int route) {
    switch (route) {
        case ROUTE_NORTH: return ROUTE_SOUTH;
        case ROUTE_SOUTH: return ROUTE_NORTH;
        case ROUTE_EAST: return ROUTE_WEST;
        case ROUTE_WEST: return ROUTE_EAST;
        default: return -1;
    }
}

int write_full(int fd, const void *buffer, size_t size) {
    const char *cursor = (const char *)buffer;
    size_t left = size;
    while (left > 0) {
        ssize_t written = write(fd, cursor, left);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        cursor += written;
        left -= (size_t)written;
    }
    return 0;
}

int read_full(int fd, void *buffer, size_t size) {
    char *cursor = (char *)buffer;
    size_t left = size;
    while (left > 0) {
        ssize_t received = read(fd, cursor, left);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            return 0;
        }
        cursor += received;
        left -= (size_t)received;
    }
    return 1;
}
