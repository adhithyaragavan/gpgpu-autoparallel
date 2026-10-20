// tests/crosstu.test: a block-scope extern here, the definition in
// crosstu_local_def.c. The import must not make G two objects.
double g(int i);
void loop(int n) {
  extern double G[100000];
  for (int i = 0; i < n; i++)
    G[i] = g(i) + 1.0;
}
