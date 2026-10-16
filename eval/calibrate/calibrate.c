// Microbenchmarks for the cost model's machine parameters (Phase 2.3).
// calibrate.py builds this, runs each benchmark (the startup ones in fresh
// processes) and writes machine/<name>.json; see README.md.
//
//   calibrate <bench> [args]      prints one number on stdout
//
// Host:   stream1 / streamN   triad bandwidth, 1 thread / all, GB/s
//         ops1 / opsN         ops/s as p05tool counts them, 1 thread / all, G/s
//         first / first_if0   this process's first `parallel` (if(0)), us
//         region              an empty `parallel for` once the runtime is up, us
// Device (built with -DP05_TARGET):
//         tfirst              this process's first `target`, us
//         tlaunch             an empty kernel once the device is up, us
//         tcopy <bytes> to|from   an empty `target data` mapping that many
//                             bytes (allocate, copy one way, free), us
//         tstream / tops      triad bandwidth / ops on the device, G.../s
//
// Bytes and ops are counted the way the cost model counts them
// (Profitability.cpp): the triad touches 24 B per element (no write-allocate
// traffic); the ops kernel is 64 multiply-adds written out, 128 ops with no
// loop counter among them (the model counts a k++ too, but it costs next to
// nothing, so a counted loop would inflate the rate), which
// `p05tool eval/calibrate/calibrate.c` reports as its loop's ops/iteration.
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FMA(x) x = x * 0.999 + 0.001;
#define FMA8(x) FMA(x) FMA(x) FMA(x) FMA(x) FMA(x) FMA(x) FMA(x) FMA(x)
#define FMA64(x) FMA8(x) FMA8(x) FMA8(x) FMA8(x) FMA8(x) FMA8(x) FMA8(x) FMA8(x)
#define OPS_PER_ELEM 128

static double now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static double *alloc(size_t n, double v) {
  double *p = malloc(n * sizeof(double));
  if (!p) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  for (size_t i = 0; i < n; i++)
    p[i] = v + (double)(i % 7);
  return p;
}

// The kernels the cost model would see: p05tool counts 128 ops per
// iteration of `ops_kernel`'s loop.
void triad(double *a, const double *b, const double *c, long n) {
  for (long i = 0; i < n; i++)
    a[i] = b[i] + 0.5 * c[i];
}

void ops_kernel(double *out, const double *in, long n) {
  for (long i = 0; i < n; i++) {
    double x = in[i];
    FMA64(x)
    out[i] = x;
  }
}

static void triad_par(double *a, const double *b, const double *c, long n) {
#pragma omp parallel for schedule(static)
  for (long i = 0; i < n; i++)
    a[i] = b[i] + 0.5 * c[i];
}

static void ops_par(double *out, const double *in, long n) {
#pragma omp parallel for schedule(static)
  for (long i = 0; i < n; i++) {
    double x = in[i];
    FMA64(x)
    out[i] = x;
  }
}

// The arguments of one benchmarked call.
struct args {
  double *a, *b, *c;
  long n;
};

// Best of `reps` runs of f, in us.
static double best(int reps, void (*f)(struct args *), struct args *x) {
  double b = 1e300;
  for (int r = 0; r < reps; r++) {
    double t0 = now_us();
    f(x);
    double t = now_us() - t0;
    if (t < b)
      b = t;
  }
  return b;
}

static void run_triad(struct args *x) { triad(x->a, x->b, x->c, x->n); }
static void run_triad_par(struct args *x) { triad_par(x->a, x->b, x->c, x->n); }
static void run_ops(struct args *x) { ops_kernel(x->a, x->b, x->n); }
static void run_ops_par(struct args *x) { ops_par(x->a, x->b, x->n); }

static volatile double sink;

static double host_stream(int threads) {
  const long n = 1L << 25;
  struct args x = {alloc(n, 0), alloc(n, 1), alloc(n, 2), n};
  double us;
  if (threads == 1)
    us = best(10, run_triad, &x);
  else {
    run_triad_par(&x); // thread start
    us = best(10, run_triad_par, &x);
  }
  sink = x.a[n / 3];
  return 24.0 * n / us / 1e3; // GB/s
}

static double host_ops(int threads) {
  const long n = 1L << 20;
  struct args x = {alloc(n, 0), alloc(n, 1), NULL, n};
  double us;
  if (threads == 1)
    us = best(5, run_ops, &x);
  else {
    run_ops_par(&x);
    us = best(5, run_ops_par, &x);
  }
  sink = x.a[n / 3];
  return (double)OPS_PER_ELEM * n / us / 1e3; // G ops/s
}

// A run-time false, so the compiler can't drop the construct.
static volatile int zero = 0;

static double first_region(int if0) {
  int x = 0;
  const int on = zero;
  double t0 = now_us();
  if (if0) {
#pragma omp parallel if (on)
    x++;
  } else {
#pragma omp parallel
    {
#pragma omp atomic
      x++;
    }
  }
  double t = now_us() - t0;
  sink = x;
  return t;
}

static double empty_region(void) {
  const int nt = omp_get_max_threads();
  int dummy[256] = {0};
  for (int w = 0; w < 100; w++) {
#pragma omp parallel for
    for (int i = 0; i < nt; i++)
      dummy[i % 256] += i;
  }
  const int reps = 2000;
  double t0 = now_us();
  for (int r = 0; r < reps; r++) {
#pragma omp parallel for
    for (int i = 0; i < nt; i++)
      dummy[i % 256] += i;
  }
  sink = dummy[0];
  return (now_us() - t0) / reps;
}

#ifdef P05_TARGET
static double target_first(void) {
  int on_host = 1;
  double t0 = now_us();
#pragma omp target map(from : on_host)
  { on_host = omp_is_initial_device(); }
  double t = now_us() - t0;
  if (on_host) {
    fprintf(stderr, "target region ran on the host\n");
    exit(1);
  }
  return t;
}

static double target_launch(void) {
  int x = 0;
  for (int w = 0; w < 20; w++) {
#pragma omp target teams distribute parallel for
    for (int i = 0; i < 1; i++)
      x += 0;
  }
  const int reps = 1000;
  double t0 = now_us();
  for (int r = 0; r < reps; r++) {
#pragma omp target teams distribute parallel for
    for (int i = 0; i < 1; i++)
      x += 0;
  }
  return (now_us() - t0) / reps;
}

static void map_to(struct args *x) {
  double *a = x->a;
  const long n = x->n;
#pragma omp target data map(to : a[0:n])
  {}
}

static void map_from(struct args *x) {
  double *a = x->a;
  const long n = x->n;
#pragma omp target data map(from : a[0:n])
  {}
}

static double target_copy(long bytes, int to) {
  struct args x = {alloc(bytes / (long)sizeof(double), 1), NULL, NULL,
                   bytes / (long)sizeof(double)};
  target_launch(); // device up
  const int reps = bytes >= (1L << 26) ? 5 : 20;
  double us = best(reps, to ? map_to : map_from, &x);
  sink = x.a[0];
  return us;
}

static void dev_triad(struct args *x) {
  double *a = x->a, *b = x->b, *c = x->c;
  const long n = x->n;
#pragma omp target teams distribute parallel for
  for (long i = 0; i < n; i++)
    a[i] = b[i] + 0.5 * c[i];
}

static void dev_ops(struct args *x) {
  double *out = x->a, *in = x->b;
  const long n = x->n;
#pragma omp target teams distribute parallel for
  for (long i = 0; i < n; i++) {
    double v = in[i];
    FMA64(v)
    out[i] = v;
  }
}

// Data stays on the device across the timed kernels.
static double target_stream(void) {
  const long n = 1L << 25;
  struct args x = {alloc(n, 0), alloc(n, 1), alloc(n, 2), n};
  double us;
#pragma omp target data map(to : x.b[0:n], x.c[0:n]) map(from : x.a[0:n])
  {
    dev_triad(&x);
    us = best(10, dev_triad, &x);
  }
  sink = x.a[n / 3];
  return 24.0 * n / us / 1e3;
}

static double target_ops(void) {
  const long n = 1L << 24;
  struct args x = {alloc(n, 0), alloc(n, 1), NULL, n};
  double us;
#pragma omp target data map(to : x.b[0:n]) map(from : x.a[0:n])
  {
    dev_ops(&x);
    us = best(5, dev_ops, &x);
  }
  sink = x.a[n / 3];
  return (double)OPS_PER_ELEM * n / us / 1e3;
}
#endif

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: calibrate <bench> [args]\n");
    return 2;
  }
  const char *b = argv[1];
  double v;
  if (!strcmp(b, "stream1"))
    v = host_stream(1);
  else if (!strcmp(b, "streamN"))
    v = host_stream(0);
  else if (!strcmp(b, "ops1"))
    v = host_ops(1);
  else if (!strcmp(b, "opsN"))
    v = host_ops(0);
  else if (!strcmp(b, "first"))
    v = first_region(0);
  else if (!strcmp(b, "first_if0"))
    v = first_region(1);
  else if (!strcmp(b, "region"))
    v = empty_region();
  else if (!strcmp(b, "threads"))
    v = omp_get_max_threads();
#ifdef P05_TARGET
  else if (!strcmp(b, "tfirst"))
    v = target_first();
  else if (!strcmp(b, "tlaunch"))
    v = target_launch();
  else if (!strcmp(b, "tcopy") && argc == 4)
    v = target_copy(atol(argv[2]), !strcmp(argv[3], "to"));
  else if (!strcmp(b, "tstream"))
    v = target_stream();
  else if (!strcmp(b, "tops"))
    v = target_ops();
#endif
  else {
    fprintf(stderr, "unknown benchmark %s\n", b);
    return 2;
  }
  printf("%.6f\n", v);
  return 0;
}
