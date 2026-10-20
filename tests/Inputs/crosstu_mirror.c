// tests/crosstu.test: the mirror case, the block-scope extern is in the
// imported function (crosstu_mirror_def.c).
double G[100000];
double g(int i);
void loop(int n) {
  for (int i = 0; i < n; i++)
    G[i] = g(i) + 1.0;
}
