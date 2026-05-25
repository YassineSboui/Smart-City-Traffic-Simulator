#define _GNU_SOURCE

#include "smartcross.h"

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static void run_plot_exec(void) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork plot");
        return;
    }
    if (pid == 0) {
        execlp("python3", "python3", "scripts/plot_results.py", "results.csv", (char *)NULL);
        execlp("python", "python", "scripts/plot_results.py", "results.csv", (char *)NULL);
        perror("exec plot_results.py");
        _exit(1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("Graphique/rapport genere via exec().\n");
    } else {
        printf("Generation du graphique impossible, mais results.csv est disponible.\n");
    }
}

void run_benchmark(Config cfg) {
    /* Benchmark mode runs all strategies with the same configuration and writes
       comparable metrics to results.csv. */
    FILE *csv = fopen("results.csv", "w");
    if (!csv) {
        die("fopen results.csv");
    }

    fprintf(csv, "strategy,processes,threads,cars,elapsed_s,avg_wait_ms,throughput_cars_s,cpu_percent,collisions_avoided,deadlock_retries,speedup\n");

    double baseline = 0.0;
    for (int i = 0; i < STRATEGY_COUNT; i++) {
        cfg.strategy = (Strategy)i;
        cfg.mode = MODE_BENCHMARK;
        cfg.quiet = 1;
        cfg.base_sleep_us = 0;
        cfg.crossing_sleep_us = 700;

        SimulationResult result = run_simulation(cfg);
        if (i == 0) {
            baseline = result.elapsed_s;
        }
        double speedup = result.elapsed_s > 0.0 ? baseline / result.elapsed_s : 0.0;

        fprintf(csv, "%s,%d,%d,%d,%.6f,%.3f,%.3f,%.1f,%d,%d,%.3f\n",
                strategy_name(result.strategy), cfg.process_count, cfg.thread_count, cfg.total_cars,
                result.elapsed_s, result.avg_wait_ms, result.throughput, result.cpu_percent,
                result.collisions_avoided, result.deadlock_retries, speedup);
        printf("%-8s temps=%.3fs attente=%.2fms debit=%.1f/s speedup=%.2f\n",
               strategy_name(result.strategy), result.elapsed_s, result.avg_wait_ms, result.throughput, speedup);
    }

    fclose(csv);
    printf("results.csv genere.\n");
    if (!cfg.no_plot) {
        run_plot_exec();
    }
}
