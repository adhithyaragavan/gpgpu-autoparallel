// tests/crosstu.test: host code between the time loop's GPU loops calls
// bstep (crosstu_hoist_other.c), whose loop that file's run may offload.
#define N 100000
extern double X[N];
void bstep(void);
void run(int steps) {
  for (int t = 0; t < steps; t++) {
    for (int i = 0; i < N; i++)
      X[i] = X[i] * 0.5 + 1.0;
    bstep();
  }
}
