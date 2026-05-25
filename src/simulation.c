#define _GNU_SOURCE

#include "smartcross.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <unistd.h>

#define LIVE_STATE_FILE "smartcross_live.txt"
#define LIVE_STATE_TMP "smartcross_live.tmp"
#define LIVE_EVENTS_FILE "smartcross_events.txt"

static void snapshot_shared(SharedState *shared, SharedState *snapshot) {
    sem_wait(&shared->stats_lock);
    *snapshot = *shared;
    sem_post(&shared->stats_lock);
}

static int choose_route(const Config *cfg, const SharedState *state, const int outstanding[MAX_ROUTES], int *rr_cursor) {
    /* This is the central scheduler. It chooses which route gets green-light
       tokens using the selected policy, while subtracting already-granted cars
       that have not yet reported completion. */
    for (int i = 0; i < cfg->active_routes; i++) {
        int available_priority = state->priority_waiting[i] - outstanding[i];
        if (available_priority > 0) {
            return i;
        }
    }

    int best = -1;
    if (cfg->strategy == STRATEGY_RR) {
        for (int offset = 0; offset < cfg->active_routes; offset++) {
            int route = (*rr_cursor + offset) % cfg->active_routes;
            if (state->waiting[route] - outstanding[route] > 0) {
                *rr_cursor = (route + 1) % cfg->active_routes;
                return route;
            }
        }
        return -1;
    }

    if (cfg->strategy == STRATEGY_FCFS) {
        long long oldest = 0;
        for (int i = 0; i < cfg->active_routes; i++) {
            if (state->waiting[i] - outstanding[i] <= 0 || state->first_arrival_us[i] == 0) {
                continue;
            }
            if (best < 0 || state->first_arrival_us[i] < oldest) {
                best = i;
                oldest = state->first_arrival_us[i];
            }
        }
        return best;
    }

    if (cfg->strategy == STRATEGY_PRIORITY) {
        int max_waiting = -1;
        for (int i = 0; i < cfg->active_routes; i++) {
            int available = state->waiting[i] - outstanding[i];
            if (available > max_waiting) {
                max_waiting = available;
                best = available > 0 ? i : best;
            }
        }
        return best;
    }

    if (cfg->strategy == STRATEGY_SJF) {
        int min_waiting = 0;
        for (int i = 0; i < cfg->active_routes; i++) {
            int available = state->waiting[i] - outstanding[i];
            if (available <= 0) {
                continue;
            }
            if (best < 0 || available < min_waiting) {
                best = i;
                min_waiting = available;
            }
        }
        return best;
    }

    if (cfg->strategy == STRATEGY_DYNAMIC) {
        long long current = now_us();
        long long best_score = -1;
        for (int i = 0; i < cfg->active_routes; i++) {
            int available = state->waiting[i] - outstanding[i];
            if (available <= 0) {
                continue;
            }
            long long age_ms = state->first_arrival_us[i] ? (current - state->first_arrival_us[i]) / 1000LL : 0;
            long long score = (long long)available * 1000LL + age_ms;
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        return best;
    }

    return -1;
}

static void send_green(int fd, int grant, int green_id) {
    GreenCommand command;
    command.grant = grant;
    command.shutdown = 0;
    command.green_id = green_id;
    (void)write_full(fd, &command, sizeof(command));
}

static void send_shutdown(int fd) {
    GreenCommand command;
    command.grant = 0;
    command.shutdown = 1;
    command.green_id = -1;
    (void)write_full(fd, &command, sizeof(command));
}

static void drain_reports(int report_fds[MAX_ROUTES], int outstanding[MAX_ROUTES]) {
    /* Reports arrive through route->controller pipes. Passed reports reduce the
       outstanding token count so the scheduler does not over-grant one route. */
    RouteReport report;
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (report_fds[i] < 0) {
            continue;
        }
        for (;;) {
            ssize_t n = read(report_fds[i], &report, sizeof(report));
            if (n == (ssize_t)sizeof(report)) {
                if (report.type == REPORT_PASSED && report.route >= 0 && report.route < MAX_ROUTES && outstanding[report.route] > 0) {
                    outstanding[report.route]--;
                }
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            }
            break;
        }
    }
}

static void append_live_event(const char *text) {
    FILE *file = fopen(LIVE_EVENTS_FILE, "a");
    if (!file) {
        return;
    }
    fprintf(file, "%lld|%s\n", now_us(), text);
    fclose(file);
}

static void drain_messages(int msgid, int quiet) {
    TrafficMessage message;
    for (;;) {
        ssize_t received = msgrcv(msgid, &message, sizeof(message) - sizeof(long), 0, IPC_NOWAIT);
        if (received < 0) {
            if (errno != ENOMSG && errno != EIDRM) {
                perror("msgrcv");
            }
            break;
        }

        if (!quiet) {
            char event[128];
            if (message.event == MSG_PRIORITY) {
                snprintf(event, sizeof(event), "[AMBULANCE] %s", message.text);
                append_live_event(event);
            } else if (message.event == MSG_PASSED && message.urgent) {
                snprintf(event, sizeof(event), "[%s] ambulance passed in %.2f ms", ROUTE_NAMES[message.route], message.wait_us / 1000.0);
                append_live_event(event);
            } else if (message.event == MSG_PASSED && message.car_id % 7 == 0) {
                snprintf(event, sizeof(event), "[%s] car %d passed, wait %.2f ms", ROUTE_NAMES[message.route], message.car_id, message.wait_us / 1000.0);
                append_live_event(event);
            } else if (message.event == MSG_ARRIVAL && message.car_id % 11 == 0) {
                snprintf(event, sizeof(event), "[%s] car %d entered the queue", ROUTE_NAMES[message.route], message.car_id);
                append_live_event(event);
            }
        }
    }
}

static int route_is_green(const SharedState *state, int route) {
    int opposite = route_opposite(state->current_green);
    return state->current_green == route || (opposite == route && opposite < state->active_routes);
}

static void write_live_state(const Config *cfg, const SharedState *state, long long start_us, int running) {
    /* File-based bridge to the Python GUI. The C engine writes a temporary file
       then renames it, so the GUI sees either the old complete state or the new
       complete state, not a half-written snapshot. */
    double elapsed = (now_us() - start_us) / 1000000.0;
    double avg_ms = state->total_passed > 0 ?
        (double)(state->wait_us[0] + state->wait_us[1] + state->wait_us[2] + state->wait_us[3]) / state->total_passed / 1000.0 : 0.0;

    FILE *file = fopen(LIVE_STATE_TMP, "w");
    if (!file) {
        return;
    }

    fprintf(file, "running=%d\n", running);
    fprintf(file, "elapsed=%.3f\n", elapsed);
    fprintf(file, "strategy=%s\n", strategy_name(cfg->strategy));
    fprintf(file, "processes=%d\n", cfg->active_routes + 1);
    fprintf(file, "threads=%d\n", cfg->thread_count);
    fprintf(file, "quantum=%d\n", cfg->quantum);
    fprintf(file, "active_routes=%d\n", state->active_routes);
    fprintf(file, "current_green=%d\n", state->current_green);
    fprintf(file, "total_cars=%d\n", state->total_cars);
    fprintf(file, "total_passed=%d\n", state->total_passed);
    fprintf(file, "avg_wait_ms=%.3f\n", avg_ms);
    fprintf(file, "throughput=%.3f\n", elapsed > 0.0 ? state->total_passed / elapsed : 0.0);
    fprintf(file, "collisions_avoided=%d\n", state->collisions_avoided);
    fprintf(file, "deadlock_retries=%d\n", state->deadlock_retries);
    fprintf(file, "ambulance_seen=%d\n", state->ambulance_seen);

    for (int i = 0; i < MAX_ROUTES; i++) {
        fprintf(file, "waiting%d=%d\n", i, state->waiting[i]);
        fprintf(file, "priority%d=%d\n", i, state->priority_waiting[i]);
        fprintf(file, "passed%d=%d\n", i, state->passed[i]);
        fprintf(file, "green%d=%d\n", i, route_is_green(state, i));
    }

    fclose(file);
    rename(LIVE_STATE_TMP, LIVE_STATE_FILE);
}

static void init_shared_state(SharedState *shared, const Config *cfg) {
    memset(shared, 0, sizeof(*shared));
    shared->active_routes = cfg->active_routes;
    shared->current_green = -1;
    shared->total_cars = cfg->total_cars;
    if (sem_init(&shared->stats_lock, 1, 1) != 0) {
        die("sem_init stats");
    }
    for (int i = 0; i < 4; i++) {
        if (sem_init(&shared->zones[i], 1, 1) != 0) {
            die("sem_init zone");
        }
    }
}

static void destroy_shared_state(SharedState *shared) {
    for (int i = 0; i < 4; i++) {
        sem_destroy(&shared->zones[i]);
    }
    sem_destroy(&shared->stats_lock);
}

static pid_t launch_gui(void) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork gui");
        return -1;
    }
    if (pid == 0) {
        execlp("python3", "python3", "scripts/smartcross_gui.py", (char *)NULL);
        execlp("python", "python", "scripts/smartcross_gui.py", (char *)NULL);
        perror("exec smartcross_gui.py");
        _exit(1);
    }
    printf("SmartCross graphical window launched. Use -quiet to disable the GUI.\n");
    return pid;
}

SimulationResult run_simulation(Config cfg) {
    /* Main controller routine: creates shared memory, message queue, pipes, then
       forks one process per route. The parent remains the traffic scheduler. */
    cfg.process_count = clamp_int(cfg.process_count, 2, 5);
    cfg.thread_count = clamp_int(cfg.thread_count, 1, MAX_THREADS);
    cfg.total_cars = clamp_int(cfg.total_cars, 1, 100000);
    cfg.quantum = clamp_int(cfg.quantum, 1, MAX_QUANTUM);
    cfg.active_routes = cfg.process_count - 1;
    cfg.emergency_route = cfg.active_routes >= 4 ? ROUTE_WEST : cfg.active_routes - 1;

    SharedState *shared = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        die("mmap");
    }
    init_shared_state(shared, &cfg);

    int msgid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (msgid < 0) {
        die("msgget");
    }

    int cmd_pipes[MAX_ROUTES][2];
    int report_pipes[MAX_ROUTES][2];
    int report_read_fds[MAX_ROUTES];
    pid_t children[MAX_ROUTES];
    int targets[MAX_ROUTES] = {0, 0, 0, 0};
    for (int i = 0; i < MAX_ROUTES; i++) {
        cmd_pipes[i][0] = cmd_pipes[i][1] = -1;
        report_pipes[i][0] = report_pipes[i][1] = -1;
        report_read_fds[i] = -1;
        children[i] = -1;
    }

    int remaining = cfg.total_cars;
    for (int i = 0; i < cfg.active_routes; i++) {
        targets[i] = cfg.total_cars / cfg.active_routes + (i < cfg.total_cars % cfg.active_routes ? 1 : 0);
        remaining -= targets[i];
    }
    if (remaining != 0) {
        targets[cfg.active_routes - 1] += remaining;
    }

    for (int i = 0; i < cfg.active_routes; i++) {
        if (pipe(cmd_pipes[i]) != 0 || pipe(report_pipes[i]) != 0) {
            die("pipe");
        }
    }

    long long start_cpu = rusage_total_us();
    long long start = now_us();
    pid_t gui_pid = -1;
    if (!cfg.quiet && !cfg.no_gui) {
        gui_pid = launch_gui();
    }

    for (int route = 0; route < cfg.active_routes; route++) {
        pid_t pid = fork();
        if (pid < 0) {
            die("fork");
        }
        if (pid == 0) {
            for (int i = 0; i < cfg.active_routes; i++) {
                close(cmd_pipes[i][1]);
                close(report_pipes[i][0]);
                if (i != route) {
                    close(cmd_pipes[i][0]);
                    close(report_pipes[i][1]);
                }
            }
            route_process_main(cfg, route, targets[route], shared, msgid, cmd_pipes[route][0], report_pipes[route][1]);
        }
        children[route] = pid;
        close(cmd_pipes[route][0]);
        close(report_pipes[route][1]);
        report_read_fds[route] = report_pipes[route][0];
        int flags = fcntl(report_read_fds[route], F_GETFL, 0);
        if (flags >= 0) {
            fcntl(report_read_fds[route], F_SETFL, flags | O_NONBLOCK);
        }
    }

    int outstanding[MAX_ROUTES] = {0, 0, 0, 0};
    int rr_cursor = 0;
    int green_id = 0;
    long long next_state_write = start;

    for (;;) {
        /* One controller loop iteration: collect events, read shared state,
           choose the next route, and send green-light tokens by pipe. */
        drain_reports(report_read_fds, outstanding);
        drain_messages(msgid, cfg.quiet);

        SharedState snapshot;
        snapshot_shared(shared, &snapshot);
        if (snapshot.total_passed >= cfg.total_cars) {
            break;
        }

        int route = choose_route(&cfg, &snapshot, outstanding, &rr_cursor);
        if (route >= 0) {
            int available = snapshot.waiting[route] - outstanding[route];
            int grant = clamp_int(available, 1, cfg.quantum);
            send_green(cmd_pipes[route][1], grant, green_id++);
            outstanding[route] += grant;

            sem_wait(&shared->stats_lock);
            shared->current_green = route;
            sem_post(&shared->stats_lock);

            int opposite = route_opposite(route);
            if (opposite >= 0 && opposite < cfg.active_routes && snapshot.priority_waiting[route] == 0) {
                int opp_available = snapshot.waiting[opposite] - outstanding[opposite];
                if (opp_available > 0) {
                    int opp_grant = clamp_int(opp_available, 1, cfg.quantum);
                    send_green(cmd_pipes[opposite][1], opp_grant, green_id++);
                    outstanding[opposite] += opp_grant;
                }
            }
        } else {
            usleep(1000);
        }

        if (!cfg.quiet && now_us() >= next_state_write) {
            snapshot_shared(shared, &snapshot);
            write_live_state(&cfg, &snapshot, start, 1);
            next_state_write = now_us() + 50000LL;
        }
    }

    for (int i = 0; i < cfg.active_routes; i++) {
        send_shutdown(cmd_pipes[i][1]);
    }
    for (int i = 0; i < cfg.active_routes; i++) {
        waitpid(children[i], NULL, 0);
        close(cmd_pipes[i][1]);
        close(report_read_fds[i]);
    }
    drain_messages(msgid, 1);

    long long end = now_us();
    long long end_cpu = rusage_total_us();
    SharedState final_state;
    snapshot_shared(shared, &final_state);
    if (!cfg.quiet) {
        write_live_state(&cfg, &final_state, start, 0);
        if (!cfg.no_gui && gui_pid > 0) {
            int gui_status = 0;
            pid_t waited = waitpid(gui_pid, &gui_status, WNOHANG);
            if (waited == 0) {
                printf("Simulation complete. The GUI window will stay open until you close it.\n");
            }
        }
    }

    msgctl(msgid, IPC_RMID, NULL);
    destroy_shared_state(shared);
    munmap(shared, sizeof(SharedState));

    long long total_wait = 0;
    for (int i = 0; i < MAX_ROUTES; i++) {
        total_wait += final_state.wait_us[i];
    }

    SimulationResult result;
    result.strategy = cfg.strategy;
    result.elapsed_s = (end - start) / 1000000.0;
    result.passed = final_state.total_passed;
    result.avg_wait_ms = final_state.total_passed > 0 ? (double)total_wait / final_state.total_passed / 1000.0 : 0.0;
    result.throughput = result.elapsed_s > 0.0 ? final_state.total_passed / result.elapsed_s : 0.0;
    result.cpu_percent = result.elapsed_s > 0.0 ? ((end_cpu - start_cpu) / 1000000.0) / result.elapsed_s * 100.0 : 0.0;
    result.collisions_avoided = final_state.collisions_avoided;
    result.deadlock_retries = final_state.deadlock_retries;
    return result;
}

void print_result(const Config *cfg, const SimulationResult *result) {
    printf("\n===== Resultat SmartCross =====\n");
    printf("Strategie              : %s\n", strategy_name(result->strategy));
    printf("Processus              : %d (%d routes + controleur)\n", cfg->active_routes + 1, cfg->active_routes);
    printf("Threads par route       : %d\n", cfg->thread_count);
    printf("Voitures traitees       : %d\n", result->passed);
    printf("Temps total             : %.3f s\n", result->elapsed_s);
    printf("Attente moyenne         : %.2f ms\n", result->avg_wait_ms);
    printf("Debit                   : %.2f voitures/s\n", result->throughput);
    printf("CPU approx.             : %.1f %%\n", result->cpu_percent);
    printf("Collisions evitees      : %d\n", result->collisions_avoided);
    printf("Tentatives anti-deadlock: %d\n", result->deadlock_retries);
}
