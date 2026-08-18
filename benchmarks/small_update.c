// Demo benchmark 3: safe but NOT GPU-profitable. Same shape as example.c and
// saxpy.c (loop body calls a pure helper), but the trip count is tiny (8) —
// kernel-launch and host-device transfer overhead would dwarf the actual work.
// This is the case the Week 2 profitability heuristic must learn to decline,
// so the demo can show "safe" and "profitable" are different questions.

double clamp_unit(double v) {
  if (v < 0.0)
    return 0.0;
  if (v > 1.0)
    return 1.0;
  return v;
}

void normalize(double *values) {
  for (int i = 0; i < 8; i++) {
    values[i] = clamp_unit(values[i]);
  }
}

int main() {
  double values[8] = {-0.5, 0.2, 1.5, 0.7, -1.0, 0.9, 0.0, 2.0};
  normalize(values);
  return 0;
}
