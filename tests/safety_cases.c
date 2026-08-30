// Day 8-10 fixture for SafetyAnalyzer. Expected verdict is written above each
// loop, matching the benchmarks/loop_shapes.c convention: a single wrong
// verdict here is a bug. Both directions are covered in every category —
// the discrimination principle from DAY_BY_DAY.md Day 9 (a check that only
// ever says one thing hasn't proven anything).

// ============================================================ SAFE ========

// SAFE — pure callee, crosses a function boundary. The headline case: a
// naive intraprocedural-only checker has nothing to say about `pure_scale`
// at all, since it never looks past the call.
static double pure_scale(double x, double factor) {
  return x * factor;
}

void safe_pure_callee(double *data, int n, double factor) {
  for (int i = 0; i < n; i++) {
    data[i] = pure_scale(data[i], factor);
  }
}

// SAFE — pure transitively, two hops down. Proves the verdict rides
// CallResolver's reachability walk rather than only checking LI.Callees.
static double pure_step2(double x) {
  return x - 1.0;
}

static double pure_step1(double x) {
  return pure_step2(x) * 2.0;
}

void safe_transitive_pure(double *data, int n) {
  for (int i = 0; i < n; i++) {
    data[i] = pure_step1(data[i]);
  }
}

// SAFE — a scalar temporary declared inside the loop body is private per
// iteration, not loop-carried.
void safe_body_local_temp(double *data, int n) {
  for (int i = 0; i < n; i++) {
    double tmp = data[i] * 2.0;
    data[i] = tmp + 1.0;
  }
}

// ========================================================== UNSAFE ========

// UNSAFE — direct callee writes a global.
static int call_count = 0;

static double counts_calls(double x) {
  call_count++;
  return x;
}

void unsafe_global_write(double *data, int n) {
  for (int i = 0; i < n; i++) {
    data[i] = counts_calls(data[i]);
  }
}

// UNSAFE — the global write is three hops down, not on the direct callee.
// The direct callee (`scale_and_touch`) is otherwise pure; a one-hop-only
// check would wrongly call this loop SAFE.
static int touch_count = 0;

static void mark_touched(void) {
  touch_count++;
}

static double touch_step(double x) {
  mark_touched();
  return x - 1.0;
}

static double scale_and_touch(double x, double factor) {
  return touch_step(x) * factor;
}

void unsafe_transitive_global_write(double *data, int n, double factor) {
  for (int i = 0; i < n; i++) {
    data[i] = scale_and_touch(data[i], factor);
  }
}

// UNSAFE — callee writes through a pointer parameter. (This is actually the
// classic reduction shape — accumulating into `*out` — which a real compiler
// would recognize as parallelizable with a reduction clause. This coarse,
// call-site-independent check cannot tell that apart from a genuine
// cross-iteration write and conservatively rejects both; see
// SafetyAnalysis.h's documented over-approximations.)
static void accumulate(double *out, double v) {
  *out += v;
}

void unsafe_pointer_param_write(double *data, int n, double *total) {
  for (int i = 0; i < n; i++) {
    accumulate(total, data[i]);
  }
}

// UNSAFE — loop body reassigns its own induction variable.
void unsafe_induction_var_reassigned(double *data, int n) {
  for (int i = 0; i < n; i++) {
    data[i] = pure_scale(data[i], 2.0);
    if (data[i] < 0.0)
      i = n; // bails out early by clobbering i, not by `break`
  }
}

// UNSAFE — reads a neighboring element: not indexed by the induction
// variable alone, so a parallel schedule can read stale or not-yet-written
// data depending on iteration order.
void unsafe_neighbor_access(double *data, int n) {
  for (int i = 1; i < n; i++) {
    data[i] = pure_scale(data[i - 1], 2.0);
  }
}

// UNSAFE — scatter access: the index into `data` is itself data-dependent,
// so which element each iteration touches cannot be determined statically.
void unsafe_scatter_write(double *data, const int *idx, int n) {
  for (int i = 0; i < n; i++) {
    data[idx[i]] = 0.0;
  }
}

// UNSAFE — classic reduction. `total` is declared outside the loop body, so
// every iteration writes the same shared location (see the note on
// `accumulate` above — same underlying limitation, this time visible
// directly in the loop body rather than through a callee).
double unsafe_reduction(const double *data, int n) {
  double total = 0.0;
  for (int i = 0; i < n; i++) {
    total += data[i];
  }
  return total;
}

// ========================================================= UNKNOWN ========

// UNKNOWN — external_effect is declared but never defined in this TU, a
// stand-in for a real library call whose body this analysis cannot inspect.
// Not marked UNSAFE: no hazard was actually *found*, the analysis simply
// cannot see far enough to say either way.
double external_effect(double x);

void unknown_opaque_callee(double *data, int n) {
  for (int i = 0; i < n; i++) {
    data[i] = external_effect(data[i]);
  }
}

// UNKNOWN — call through a function pointer; LoopInfo::HasIndirectCall fires
// because the callee cannot be resolved to a FunctionDecl at all.
void unknown_indirect_call(double *data, int n, double (*fn)(double)) {
  for (int i = 0; i < n; i++) {
    data[i] = fn(data[i]);
  }
}
