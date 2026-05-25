#include "smartcross.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void usage(const char *program) {
    printf("Usage: %s [options]\n", program);
    printf("Options:\n");
    printf("  -mode normal|priority|benchmark\n");
    printf("  -strategy fcfs|rr|priority|sjf|dynamic\n");
    printf("  -cars N          nombre total de voitures\n");
    printf("  -p N             nombre de processus total, 2..5 (controleur inclus)\n");
    printf("  -t N             threads par processus route, 1..16\n");
    printf("  -quantum N       voitures autorisees par feu vert\n");
    printf("  -speed N         vitesse demo: 1 tres lent, 5 moyen, 10 rapide\n");
    printf("  -seed N          graine pseudo-aleatoire\n");
    printf("  -quiet           sortie compacte\n");
    printf("  -no-gui          n'ouvre pas la fenetre graphique\n");
    printf("  -no-plot         ne lance pas le script python en benchmark\n");
    printf("  -h, -help        affiche cette aide\n");
}

static Config default_config(void) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = MODE_NORMAL;
    cfg.strategy = STRATEGY_RR;
    cfg.total_cars = 80;
    cfg.process_count = 5;
    cfg.thread_count = 2;
    cfg.quantum = 4;
    cfg.seed = (unsigned int)time(NULL);
    cfg.base_sleep_us = 12000;
    cfg.crossing_sleep_us = 12000;
    return cfg;
}

static int parse_args(int argc, char **argv, Config *cfg) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "normal") == 0) {
                cfg->mode = MODE_NORMAL;
            } else if (strcmp(mode, "priority") == 0) {
                cfg->mode = MODE_PRIORITY;
            } else if (strcmp(mode, "benchmark") == 0) {
                cfg->mode = MODE_BENCHMARK;
            } else {
                fprintf(stderr, "Mode inconnu: %s\n", mode);
                return -1;
            }
        } else if (strcmp(argv[i], "-strategy") == 0 && i + 1 < argc) {
            if (!parse_strategy(argv[++i], &cfg->strategy)) {
                fprintf(stderr, "Strategie inconnue: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-cars") == 0 && i + 1 < argc) {
            cfg->total_cars = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            cfg->process_count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            cfg->thread_count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-quantum") == 0 && i + 1 < argc) {
            cfg->quantum = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-speed") == 0 && i + 1 < argc) {
            int speed = clamp_int(atoi(argv[++i]), 1, 10);
            /* Speed is intentionally non-linear for demos. With multiple route
               processes and worker threads, a linear scale still finishes too
               quickly for the eye to follow. */
            static const int arrival_delays_us[10] = {
                900000, 650000, 450000, 320000, 230000,
                165000, 120000, 85000, 60000, 40000
            };
            static const int crossing_delays_us[10] = {
                520000, 400000, 300000, 230000, 175000,
                130000, 95000, 70000, 52000, 38000
            };
            cfg->base_sleep_us = arrival_delays_us[speed - 1];
            cfg->crossing_sleep_us = crossing_delays_us[speed - 1];
        } else if (strcmp(argv[i], "-seed") == 0 && i + 1 < argc) {
            cfg->seed = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-quiet") == 0) {
            cfg->quiet = 1;
        } else if (strcmp(argv[i], "-no-gui") == 0) {
            cfg->no_gui = 1;
        } else if (strcmp(argv[i], "-no-plot") == 0) {
            cfg->no_plot = 1;
        } else {
            fprintf(stderr, "Option inconnue ou incomplete: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    Config cfg = default_config();
    int parse_status = parse_args(argc, argv, &cfg);
    if (parse_status <= 0) {
        return parse_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    cfg.process_count = clamp_int(cfg.process_count, 2, 5);
    cfg.thread_count = clamp_int(cfg.thread_count, 1, MAX_THREADS);
    cfg.quantum = clamp_int(cfg.quantum, 1, MAX_QUANTUM);
    cfg.total_cars = clamp_int(cfg.total_cars, 1, 100000);

    if (cfg.mode == MODE_BENCHMARK) {
        run_benchmark(cfg);
        return EXIT_SUCCESS;
    }

    /* Normal double-click usage opens the Python game launcher first.
       The launcher later starts this same binary with -no-gui to run only
       the C simulation engine in the background. */
    if (!cfg.quiet && !cfg.no_gui) {
        execlp("python3", "python3", "scripts/smartcross_gui.py", (char *)NULL);
        execlp("python", "python", "scripts/smartcross_gui.py", (char *)NULL);
        perror("exec smartcross_gui.py");
        return EXIT_FAILURE;
    }

    if (!cfg.quiet && !cfg.no_gui) {
        cfg.base_sleep_us *= 3;
        cfg.crossing_sleep_us *= 3;
    }

    if (cfg.mode == MODE_PRIORITY) {
        cfg.strategy = STRATEGY_PRIORITY;
    }

    SimulationResult result = run_simulation(cfg);
    cfg.active_routes = cfg.process_count - 1;
    print_result(&cfg, &result);
    return EXIT_SUCCESS;
}
