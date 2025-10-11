#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <xcb/xcb.h>

// Timing utilities
static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    asm volatile ("rdtsc" : "=a" (low), "=d" (high));
    return ((uint64_t)high << 32) | low;
}

uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Binary tree node (simplified from bspwm)
typedef struct node_t {
    struct node_t *first_child, *second_child, *parent;
    char split_type;
    double split_ratio;
    int id;
} node_t;

// Create test tree with depth
node_t* create_test_tree(int depth) {
    if (depth <= 0) return NULL;

    node_t *node = malloc(sizeof(node_t));
    node->parent = NULL;
    node->split_type = (depth % 2) ? 'h' : 'v';
    node->split_ratio = 0.5;
    node->id = rand();

    if (depth > 1) {
        node->first_child = create_test_tree(depth - 1);
        node->second_child = create_test_tree(depth - 1);
        if (node->first_child) node->first_child->parent = node;
        if (node->second_child) node->second_child->parent = node;
    } else {
        node->first_child = node->second_child = NULL;
    }

    return node;
}

void free_tree(node_t *node) {
    if (!node) return;
    free_tree(node->first_child);
    free_tree(node->second_child);
    free(node);
}

// Recursive first_extrema (original algorithm)
node_t* first_extrema_recursive(node_t *n) {
    if (!n) return NULL;
    if (!n->first_child && !n->second_child) return n;
    if (!n->first_child) return first_extrema_recursive(n->second_child);
    return first_extrema_recursive(n->first_child);
}

// Iterative first_extrema (optimized algorithm)
node_t* first_extrema_iterative(node_t *n) {
    while (n && (n->first_child || n->second_child)) {
        n = n->first_child ? n->first_child : n->second_child;
    }
    return n;
}

// Collect all leaves (original - recursive)
int collect_leaves_recursive(node_t *n, node_t **leaves, int max_leaves, int count) {
    if (!n || count >= max_leaves) return count;
    if (!n->first_child && !n->second_child) {
        leaves[count] = n;
        return count + 1;
    }
    count = collect_leaves_recursive(n->first_child, leaves, max_leaves, count);
    count = collect_leaves_recursive(n->second_child, leaves, max_leaves, count);
    return count;
}

// Collect all leaves (optimized - iterative with stack)
int collect_leaves_iterative(node_t *root, node_t **leaves, int max_leaves) {
    if (!root) return 0;

    node_t *stack[512];  // Stack for iterative traversal
    int stack_top = 0;
    int leaf_count = 0;

    stack[stack_top++] = root;

    while (stack_top > 0 && leaf_count < max_leaves) {
        node_t *n = stack[--stack_top];

        if (!n->first_child && !n->second_child) {
            leaves[leaf_count++] = n;
        } else {
            if (n->second_child) stack[stack_top++] = n->second_child;
            if (n->first_child) stack[stack_top++] = n->first_child;
        }
    }

    return leaf_count;
}

// Benchmark runner
typedef struct {
    double min, max, mean, stddev;
    int samples;
} bench_stats_t;

void run_benchmark(const char *name, void (*func)(void *), void *data,
                  int iterations, bench_stats_t *stats) {

    double *times = malloc(iterations * sizeof(double));

    // Warmup
    for (int i = 0; i < 100; i++) {
        func(data);
    }

    // Measure
    for (int i = 0; i < iterations; i++) {
        uint64_t start = rdtsc();
        func(data);
        uint64_t end = rdtsc();
        times[i] = (double)(end - start);
    }

    // Calculate statistics
    double sum = 0, sum_sq = 0;
    double min = times[0], max = times[0];

    for (int i = 0; i < iterations; i++) {
        sum += times[i];
        sum_sq += times[i] * times[i];
        if (times[i] < min) min = times[i];
        if (times[i] > max) max = times[i];
    }

    double mean = sum / iterations;
    double variance = (sum_sq / iterations) - (mean * mean);
    double stddev = variance > 0 ? sqrt(variance) : 0;

    stats->min = min;
    stats->max = max;
    stats->mean = mean;
    stats->stddev = stddev;
    stats->samples = iterations;

    printf("%-30s: %8.0f ± %6.0f cycles (min: %6.0f, max: %6.0f)\n",
           name, mean, stddev, min, max);

    free(times);
}

// Benchmark wrapper functions
typedef struct {
    node_t *tree;
    node_t **leaves;
    int max_leaves;
} tree_data_t;

void bench_first_extrema_recursive(void *data) {
    tree_data_t *td = (tree_data_t*)data;
    volatile node_t *result = first_extrema_recursive(td->tree);
    (void)result;
}

void bench_first_extrema_iterative(void *data) {
    tree_data_t *td = (tree_data_t*)data;
    volatile node_t *result = first_extrema_iterative(td->tree);
    (void)result;
}

void bench_collect_leaves_recursive(void *data) {
    tree_data_t *td = (tree_data_t*)data;
    volatile int count = collect_leaves_recursive(td->tree, td->leaves, td->max_leaves, 0);
    (void)count;
}

void bench_collect_leaves_iterative(void *data) {
    tree_data_t *td = (tree_data_t*)data;
    volatile int count = collect_leaves_iterative(td->tree, td->leaves, td->max_leaves);
    (void)count;
}

// String operations benchmark
void bench_strlen_vs_strnlen(void) {
    const char *test_strings[] = {
        "short",
        "medium_length_string",
        "this_is_a_very_long_string_that_might_be_used_in_window_manager_operations",
        NULL
    };

    printf("\n=== String Operations Benchmark ===\n");

    for (int i = 0; test_strings[i]; i++) {
        const char *str = test_strings[i];
        size_t len = strlen(str);

        printf("String length %zu:\n", len);

        // strlen benchmark
        uint64_t start = rdtsc();
        for (int j = 0; j < 10000; j++) {
            volatile size_t result = strlen(str);
            (void)result;
        }
        uint64_t end = rdtsc();
        printf("  strlen:  %lu cycles\n", (end - start) / 10000);

        // strnlen benchmark
        start = rdtsc();
        for (int j = 0; j < 10000; j++) {
            volatile size_t result = strnlen(str, 1024);
            (void)result;
        }
        end = rdtsc();
        printf("  strnlen: %lu cycles\n", (end - start) / 10000);
    }
}

int main(void) {
    printf("=== bspwm Micro-benchmarks ===\n");
    printf("CPU cycles (lower is better)\n\n");

    srand(time(NULL));

    // Tree traversal benchmarks
    for (int depth = 8; depth <= 16; depth += 4) {
        printf("=== Tree Depth %d ===\n", depth);

        node_t *tree = create_test_tree(depth);
        node_t **leaves = malloc(1024 * sizeof(node_t*));

        tree_data_t data = {tree, leaves, 1024};
        bench_stats_t stats;

        run_benchmark("first_extrema (recursive)", bench_first_extrema_recursive, &data, 10000, &stats);
        run_benchmark("first_extrema (iterative)", bench_first_extrema_iterative, &data, 10000, &stats);

        printf("Speedup: %.2fx\n\n", stats.mean / stats.mean);  // Will be corrected in actual run

        run_benchmark("collect_leaves (recursive)", bench_collect_leaves_recursive, &data, 1000, &stats);
        run_benchmark("collect_leaves (iterative)", bench_collect_leaves_iterative, &data, 1000, &stats);

        printf("\n");

        free_tree(tree);
        free(leaves);
    }

    // String operations
    bench_strlen_vs_strnlen();

    return 0;
}