#ifndef SMARTCROSS_H
#define SMARTCROSS_H

#include <semaphore.h>
#include <stddef.h>
#include <sys/types.h>

#define MAX_ROUTES 4
#define MAX_THREADS 16
#define MAX_QUANTUM 50

typedef enum {
    ROUTE_NORTH = 0,
    ROUTE_SOUTH = 1,
    ROUTE_EAST = 2,
    ROUTE_WEST = 3
} Route;

typedef enum {
    MODE_NORMAL = 0,
    MODE_PRIORITY = 1,
    MODE_BENCHMARK = 2
} Mode;

typedef enum {
    STRATEGY_FCFS = 0,
    STRATEGY_RR = 1,
    STRATEGY_PRIORITY = 2,
    STRATEGY_SJF = 3,
    STRATEGY_DYNAMIC = 4,
    STRATEGY_COUNT = 5
} Strategy;

typedef enum {
    TURN_RIGHT = 0,
    TURN_STRAIGHT = 1,
    TURN_LEFT = 2
} Turn;

typedef enum {
    MSG_ARRIVAL = 1,
    MSG_PASSED = 2,
    MSG_PRIORITY = 3
} MessageEvent;

typedef enum {
    REPORT_REQUEST = 1,
    REPORT_DONE = 2,
    REPORT_PRIORITY = 3,
    REPORT_PASSED = 4
} ReportType;

typedef struct {
    long mtype;
    int route;
    int event;
    int car_id;
    int waiting;
    int urgent;
    long long wait_us;
    char text[96];
} TrafficMessage;

typedef struct {
    int type;
    int route;
    int value;
} RouteReport;

typedef struct {
    int grant;
    int shutdown;
    int green_id;
} GreenCommand;

typedef struct {
    int id;
    int route;
    int turn;
    int urgent;
    long long arrival_us;
} Car;

typedef struct {
    /* Shared memory block visible to the controller process and all route processes.
       The stats_lock semaphore protects every mutable field in this structure. */
    sem_t stats_lock;
    /* The four semaphore-protected conflict zones model the dining-philosophers
       adaptation: a car may need 1, 2, or 3 zones depending on its turn. */
    sem_t zones[4];
    int active_routes;
    int current_green;
    int total_cars;
    int total_generated;
    int total_passed;
    int collisions_avoided;
    int deadlock_retries;
    int ambulance_seen;
    int waiting[MAX_ROUTES];
    int priority_waiting[MAX_ROUTES];
    int generated[MAX_ROUTES];
    int passed[MAX_ROUTES];
    int done_generating[MAX_ROUTES];
    long long wait_us[MAX_ROUTES];
    long long first_arrival_us[MAX_ROUTES];
} SharedState;

typedef struct {
    /* User-facing configuration. process_count includes the central controller,
       so p=5 means 1 controller + 4 route processes. */
    Mode mode;
    Strategy strategy;
    int total_cars;
    int process_count;
    int thread_count;
    int quantum;
    int quiet;
    int no_plot;
    /* Used by the Python game launcher. It starts the C engine without letting
       the C engine open another GUI recursively. */
    int no_gui;
    int active_routes;
    int emergency_route;
    unsigned int seed;
    int base_sleep_us;
    int crossing_sleep_us;
} Config;

typedef struct {
    Strategy strategy;
    double elapsed_s;
    double avg_wait_ms;
    double throughput;
    double cpu_percent;
    int passed;
    int collisions_avoided;
    int deadlock_retries;
} SimulationResult;

extern const char *ROUTE_NAMES[MAX_ROUTES];
extern const char *STRATEGY_NAMES[STRATEGY_COUNT];

long long now_us(void);
long long rusage_total_us(void);
int clamp_int(int value, int min_value, int max_value);
void die(const char *message);
const char *strategy_name(Strategy strategy);
int parse_strategy(const char *value, Strategy *strategy);
int route_opposite(int route);
int write_full(int fd, const void *buffer, size_t size);
int read_full(int fd, void *buffer, size_t size);

void route_process_main(Config cfg, int route, int target_cars, SharedState *shared, int msgid, int cmd_fd, int report_fd);
SimulationResult run_simulation(Config cfg);
void print_result(const Config *cfg, const SimulationResult *result);
void run_benchmark(Config cfg);

#endif
