#define N 100000
double X[N];
void bstep(void) {
  for (int i = 0; i < N; i++)
    X[i] = X[i] * 2.0;
}
