/* Resynthesize with RHVoice's own HTS vocoder (to validate feature conventions).
   usage: vocode mgc.f32 lf0.f32 bap.f32 bpf.txt out.f64 [alpha beta]
   mgc: T x 31 float32, lf0: T float32 (-1e10 = unvoiced), bap: T x 7 float32 (dB) */
#include <stdio.h>
#include <stdlib.h>
#include "HTS_hidden.h"

void HTS_Audio_write(HTS_Audio* a, short d) { (void)a; (void)d; }
static float* readf(const char* p, long* n) {
  FILE* f = fopen(p, "rb"); fseek(f, 0, SEEK_END); *n = ftell(f) / 4; rewind(f);
  float* b = malloc(*n * 4); fread(b, 4, *n, f); fclose(f); return b;
}

int main(int argc, char** argv) {
  long nm, nl, nb; float* mgc = readf(argv[1], &nm); float* lf0 = readf(argv[2], &nl); float* bap = readf(argv[3], &nb);
  double alpha = argc > 6 ? atof(argv[6]) : 0.466, beta = argc > 7 ? atof(argv[7]) : 0.0;
  const int M = 30, NB = 7, FP = 120, SR = 24000;
  BPF bpf; FILE* fb = fopen(argv[4], "r"); fscanf(fb, "%u %u", &bpf.number, &bpf.length);
  bpf.coefs = malloc(sizeof(double*) * bpf.number);
  for (unsigned i = 0; i < bpf.number; i++) { bpf.coefs[i] = malloc(sizeof(double) * bpf.length);
    for (unsigned j = 0; j < bpf.length; j++) fscanf(fb, "%lf", &bpf.coefs[i][j]); }
  fclose(fb);
  HTS_Vocoder v; HTS_Vocoder_initialize(&v, M, 0, TRUE, SR, FP);
  FILE* fo = fopen(argv[5], "wb"); double raw[FP]; double spec[M + 1], b[NB];
  for (long t = 0; t < nl; t++) {
    for (int i = 0; i <= M; i++) spec[i] = mgc[t * (M + 1) + i];
    for (int i = 0; i < NB; i++) { double x = bap[t * NB + i]; if (x > 0) x = 0; x = pow(10.0, x / 10.0); b[i] = x > 1 ? 1 : x; }
    double l = lf0[t] < -1e9 ? LZERO : lf0[t];
    HTS_Vocoder_synthesize(&v, M, l, spec, b, &bpf, alpha, beta, 1.0, raw, NULL);
    fwrite(raw, sizeof(double), FP, fo);
  }
  fclose(fo); HTS_Vocoder_clear(&v); return 0;
}
