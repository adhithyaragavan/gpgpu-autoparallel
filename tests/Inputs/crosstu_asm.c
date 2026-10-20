// tests/crosstu.test: an asm label gives G a second name, A (the same
// symbol on Mach-O, where C names get a leading underscore).
double G[100000];
extern double A[100000] __asm__("_G");
void loop(int n) {
  for (int i = 0; i < n; i++)
    A[i] = G[i + 1] + 1.0;
}
