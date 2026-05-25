#define _GNU_SOURCE

#include "smartcross.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <unistd.h>

typedef struct {
    /* Private state of one route process. This memory is not shared with other
       processes, but it is shared by the threads inside this route process. */
    Config cfg;
    int route;
    int target_cars;
    int msgid;
    int cmd_read_fd;
    int report_write_fd;
    SharedState *shared;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    Car *queue;
    int capacity;
    int head;
    int tail;
    int count;
    int tokens;
    int shutdown;
    int generator_done;
    unsigned int rng;
} RouteContext;

static void send_report(RouteContext *ctx, int type, int value) {
    /* Lightweight pipe message back to the controller. This channel is used for
       scheduling correctness, unlike the System V queue which is mostly events. */
    RouteReport report;
    report.type = type;
    report.route = ctx->route;
    report.value = value;
    (void)write_full(ctx->report_write_fd, &report, sizeof(report));
}

static void send_message(RouteContext *ctx, int event, const Car *car, long long wait_us, const char *fmt, ...) {
    /* System V message queue event. The GUI eventually sees these messages via
       the controller writing them to smartcross_events.txt. */
    TrafficMessage message;
    memset(&message, 0, sizeof(message));
    message.mtype = 1;
    message.route = ctx->route;
    message.event = event;
    message.car_id = car ? car->id : 0;
    message.urgent = car ? car->urgent : 0;
    message.wait_us = wait_us;

    va_list args;
    va_start(args, fmt);
    vsnprintf(message.text, sizeof(message.text), fmt, args);
    va_end(args);

    pthread_mutex_lock(&ctx->lock);
    message.waiting = ctx->count;
    pthread_mutex_unlock(&ctx->lock);

    if (msgsnd(ctx->msgid, &message, sizeof(message) - sizeof(long), IPC_NOWAIT) < 0) {
        if (errno != EAGAIN && errno != EIDRM) {
            perror("msgsnd");
        }
    }
}

static void enqueue_car(RouteContext *ctx, Car car) {
    /* Protected in-process FIFO. Ambulances are inserted at the head so the
       worker threads serve them first once the controller gives this route green. */
    pthread_mutex_lock(&ctx->lock);
    while (ctx->count == ctx->capacity && !ctx->shutdown) {
        pthread_cond_wait(&ctx->cond, &ctx->lock);
    }
    if (!ctx->shutdown) {
        if (car.urgent) {
            ctx->head = (ctx->head - 1 + ctx->capacity) % ctx->capacity;
            ctx->queue[ctx->head] = car;
        } else {
            ctx->queue[ctx->tail] = car;
            ctx->tail = (ctx->tail + 1) % ctx->capacity;
        }
        ctx->count++;
        pthread_cond_broadcast(&ctx->cond);
    }
    pthread_mutex_unlock(&ctx->lock);
}

static int dequeue_car(RouteContext *ctx, Car *car) {
    /* Worker threads cannot take cars freely. They need both a queued car and a
       token from the controller, which represents a green-light permission. */
    int found = 0;
    pthread_mutex_lock(&ctx->lock);
    while (!ctx->shutdown && (ctx->tokens <= 0 || ctx->count <= 0)) {
        pthread_cond_wait(&ctx->cond, &ctx->lock);
    }
    if (ctx->tokens > 0 && ctx->count > 0) {
        *car = ctx->queue[ctx->head];
        ctx->head = (ctx->head + 1) % ctx->capacity;
        ctx->count--;
        ctx->tokens--;
        pthread_cond_broadcast(&ctx->cond);
        found = 1;
    }
    pthread_mutex_unlock(&ctx->lock);
    return found;
}

static void update_shared_on_arrival(RouteContext *ctx, const Car *car) {
    /* Shared counters are protected by a process-shared POSIX semaphore because
       route processes and the controller all access the same mmap() region. */
    long long arrival = car->arrival_us;
    sem_wait(&ctx->shared->stats_lock);
    if (ctx->shared->waiting[ctx->route] == 0) {
        ctx->shared->first_arrival_us[ctx->route] = arrival;
    }
    ctx->shared->waiting[ctx->route]++;
    ctx->shared->generated[ctx->route]++;
    ctx->shared->total_generated++;
    if (car->urgent) {
        ctx->shared->priority_waiting[ctx->route]++;
        ctx->shared->ambulance_seen = 1;
    }
    sem_post(&ctx->shared->stats_lock);
}

static void update_shared_on_pass(RouteContext *ctx, const Car *car, long long wait) {
    sem_wait(&ctx->shared->stats_lock);
    if (ctx->shared->waiting[ctx->route] > 0) {
        ctx->shared->waiting[ctx->route]--;
    }
    if (ctx->shared->waiting[ctx->route] == 0) {
        ctx->shared->first_arrival_us[ctx->route] = 0;
    }
    if (car->urgent && ctx->shared->priority_waiting[ctx->route] > 0) {
        ctx->shared->priority_waiting[ctx->route]--;
    }
    ctx->shared->passed[ctx->route]++;
    ctx->shared->total_passed++;
    ctx->shared->wait_us[ctx->route] += wait;
    sem_post(&ctx->shared->stats_lock);
}

static void turn_zones(int route, int turn, int zones[3], int *zone_count) {
    /* A turn maps to conflict zones in the intersection:
       right=1 zone, straight=2 zones, left=3 zones. The zones are sorted so all
       cars acquire semaphores in the same order, preventing deadlock. */
    int start;
    switch (route) {
        case ROUTE_NORTH: start = 0; break;
        case ROUTE_EAST: start = 1; break;
        case ROUTE_SOUTH: start = 2; break;
        case ROUTE_WEST: start = 3; break;
        default: start = 0; break;
    }

    if (turn == TURN_RIGHT) {
        *zone_count = 1;
    } else if (turn == TURN_STRAIGHT) {
        *zone_count = 2;
    } else {
        *zone_count = 3;
    }

    for (int i = 0; i < *zone_count; i++) {
        zones[i] = (start + i) % 4;
    }

    for (int i = 0; i < *zone_count; i++) {
        for (int j = i + 1; j < *zone_count; j++) {
            if (zones[j] < zones[i]) {
                int tmp = zones[i];
                zones[i] = zones[j];
                zones[j] = tmp;
            }
        }
    }
}

static void lock_intersection_zones(RouteContext *ctx, const Car *car) {
    /* Try-lock is used to demonstrate avoided collisions/deadlock retries.
       If one zone is busy, already-acquired zones are released and retried. */
    int zones[3];
    int zone_count = 0;
    turn_zones(car->route, car->turn, zones, &zone_count);

    for (;;) {
        int acquired = 0;
        for (; acquired < zone_count; acquired++) {
            if (sem_trywait(&ctx->shared->zones[zones[acquired]]) != 0) {
                break;
            }
        }
        if (acquired == zone_count) {
            return;
        }
        for (int i = acquired - 1; i >= 0; i--) {
            sem_post(&ctx->shared->zones[zones[i]]);
        }
        sem_wait(&ctx->shared->stats_lock);
        ctx->shared->collisions_avoided++;
        ctx->shared->deadlock_retries++;
        sem_post(&ctx->shared->stats_lock);
        usleep(1000);
    }
}

static void unlock_intersection_zones(RouteContext *ctx, const Car *car) {
    int zones[3];
    int zone_count = 0;
    turn_zones(car->route, car->turn, zones, &zone_count);
    for (int i = zone_count - 1; i >= 0; i--) {
        sem_post(&ctx->shared->zones[zones[i]]);
    }
}

static void *generator_thread(void *arg) {
    /* Producer thread: creates cars with random turns and arrival times. */
    RouteContext *ctx = (RouteContext *)arg;
    for (int i = 0; i < ctx->target_cars; i++) {
        if (ctx->cfg.base_sleep_us > 0) {
            int jitter = rand_r(&ctx->rng) % (ctx->cfg.base_sleep_us + 1);
            usleep((useconds_t)(ctx->cfg.base_sleep_us / 2 + jitter));
        }

        Car car;
        car.id = i + 1;
        car.route = ctx->route;
        car.turn = rand_r(&ctx->rng) % 3;
        car.urgent = (ctx->cfg.mode == MODE_PRIORITY && ctx->route == ctx->cfg.emergency_route && i == ctx->target_cars / 3);
        car.arrival_us = now_us();

        enqueue_car(ctx, car);
        update_shared_on_arrival(ctx, &car);
        send_report(ctx, car.urgent ? REPORT_PRIORITY : REPORT_REQUEST, car.id);

        if (car.urgent) {
            send_message(ctx, MSG_PRIORITY, &car, 0, "Ambulance detectee sur %s", ROUTE_NAMES[ctx->route]);
        } else {
            send_message(ctx, MSG_ARRIVAL, &car, 0, "Voiture %d arrivee sur %s", car.id, ROUTE_NAMES[ctx->route]);
        }
    }

    pthread_mutex_lock(&ctx->lock);
    ctx->generator_done = 1;
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);

    sem_wait(&ctx->shared->stats_lock);
    ctx->shared->done_generating[ctx->route] = 1;
    sem_post(&ctx->shared->stats_lock);
    send_report(ctx, REPORT_DONE, ctx->target_cars);
    return NULL;
}

static void *command_thread(void *arg) {
    /* Controller listener thread: receives green-light tokens through a pipe. */
    RouteContext *ctx = (RouteContext *)arg;
    for (;;) {
        GreenCommand command;
        int status = read_full(ctx->cmd_read_fd, &command, sizeof(command));
        if (status <= 0) {
            break;
        }
        pthread_mutex_lock(&ctx->lock);
        if (command.shutdown) {
            ctx->shutdown = 1;
            pthread_cond_broadcast(&ctx->cond);
            pthread_mutex_unlock(&ctx->lock);
            break;
        }
        ctx->tokens += command.grant;
        pthread_cond_broadcast(&ctx->cond);
        pthread_mutex_unlock(&ctx->lock);
    }
    return NULL;
}

static void *worker_thread(void *arg) {
    /* Consumer threads: wait for tokens, acquire intersection zones, then mark
       the car as passed. Multiple workers show multithreading per route. */
    RouteContext *ctx = (RouteContext *)arg;
    for (;;) {
        Car car;
        if (!dequeue_car(ctx, &car)) {
            pthread_mutex_lock(&ctx->lock);
            int should_stop = ctx->shutdown;
            pthread_mutex_unlock(&ctx->lock);
            if (should_stop) {
                break;
            }
            continue;
        }

        long long wait = now_us() - car.arrival_us;
        lock_intersection_zones(ctx, &car);
        if (ctx->cfg.crossing_sleep_us > 0) {
            usleep((useconds_t)ctx->cfg.crossing_sleep_us);
        }
        unlock_intersection_zones(ctx, &car);

        update_shared_on_pass(ctx, &car, wait);
        send_report(ctx, REPORT_PASSED, car.id);
        send_message(ctx, MSG_PASSED, &car, wait, "Voiture %d passee depuis %s", car.id, ROUTE_NAMES[ctx->route]);
    }
    return NULL;
}

void route_process_main(Config cfg, int route, int target_cars, SharedState *shared, int msgid, int cmd_fd, int report_fd) {
    /* Entry point executed after fork() for each route. It starts one generator,
       one pipe-listener, and N worker threads configured by -t. */
    RouteContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg;
    ctx.route = route;
    ctx.target_cars = target_cars;
    ctx.msgid = msgid;
    ctx.cmd_read_fd = cmd_fd;
    ctx.report_write_fd = report_fd;
    ctx.shared = shared;
    ctx.capacity = target_cars + cfg.thread_count + 4;
    ctx.queue = calloc((size_t)ctx.capacity, sizeof(Car));
    ctx.rng = cfg.seed ^ (unsigned int)(route * 2654435761U) ^ (unsigned int)getpid();
    if (!ctx.queue) {
        die("calloc queue");
    }
    pthread_mutex_init(&ctx.lock, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    pthread_t generator;
    pthread_t commander;
    pthread_t workers[MAX_THREADS];
    if (pthread_create(&generator, NULL, generator_thread, &ctx) != 0) {
        die("pthread_create generator");
    }
    if (pthread_create(&commander, NULL, command_thread, &ctx) != 0) {
        die("pthread_create command");
    }
    for (int i = 0; i < cfg.thread_count; i++) {
        if (pthread_create(&workers[i], NULL, worker_thread, &ctx) != 0) {
            die("pthread_create worker");
        }
    }

    pthread_join(generator, NULL);
    pthread_join(commander, NULL);
    for (int i = 0; i < cfg.thread_count; i++) {
        pthread_join(workers[i], NULL);
    }

    pthread_cond_destroy(&ctx.cond);
    pthread_mutex_destroy(&ctx.lock);
    free(ctx.queue);
    close(cmd_fd);
    close(report_fd);
    _exit(EXIT_SUCCESS);
}
