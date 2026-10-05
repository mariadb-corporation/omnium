// Created by Roel Van de Paar, MariaDB
// xoshiro256++: the one source of randomness. Per-worker streams come from jump().
#include "common.h"

#include <sys/random.h>
#include <time.h>

static inline uint64_t splitmix64(uint64_t& x) {
  uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}
static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

void Xoshiro256pp::seed(uint64_t z) {
  s[0] = splitmix64(z); s[1] = splitmix64(z);
  s[2] = splitmix64(z); s[3] = splitmix64(z);
  if ((s[0] | s[1] | s[2] | s[3]) == 0) s[0] = 0x9E3779B97F4A7C15ULL;
}
void Xoshiro256pp::seed_full() {
  uint64_t z = 0;
  if (getrandom(s, sizeof(s), 0) != (ssize_t)sizeof(s)) z = (uint64_t)time(nullptr);
  z ^= (uint64_t)getpid() << 32;
  z ^= (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
  z ^= (uint64_t)(uintptr_t)&z;
  for (auto& w : s) w ^= splitmix64(z);
  if ((s[0] | s[1] | s[2] | s[3]) == 0) s[0] = 0x9E3779B97F4A7C15ULL;
}
uint64_t Xoshiro256pp::next() {
  uint64_t r = rotl(s[0] + s[3], 23) + s[0];
  uint64_t t = s[1] << 17;
  s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
  s[3] = rotl(s[3], 45);
  return r;
}
// the reference jump polynomial: 2^128 draws forward, so streams never overlap
void Xoshiro256pp::jump() {
  static const uint64_t J[] = {0x180ec6d33cfd0aba, 0xd5a61266f0c9392c, 0xa9582618e03fc9aa, 0x39abdc4529b1661c};
  uint64_t t[4] = {0, 0, 0, 0};
  for (uint64_t j : J)
    for (int b = 0; b < 64; b++) {
      if (j & (1ULL << b)) for (int k = 0; k < 4; k++) t[k] ^= s[k];
      next();
    }
  for (int k = 0; k < 4; k++) s[k] = t[k];
}
uint64_t Xoshiro256pp::below(uint64_t n) {
  if (n == 0) return 0;
  // unbiased: reject the top partial range
  uint64_t lim = UINT64_MAX - (UINT64_MAX % n);
  uint64_t r;
  do r = next(); while (r >= lim);
  return r % n;
}
long Xoshiro256pp::range(long lo, long hi) {
  if (hi <= lo) return lo;
  return lo + (long)below((uint64_t)(hi - lo + 1));
}
double Xoshiro256pp::unit() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
bool Xoshiro256pp::chance_pct(int pct) { return pct > 0 && (pct >= 100 || (int)below(100) < pct); }
string Xoshiro256pp::digits(int n) {
  string r;
  for (int i = 0; i < n; i++) r += (char)('0' + (i == 0 ? 1 + below(9) : below(10)));
  return r;
}

static Xoshiro256pp g_rng;
static uint64_t g_seed_used = 0;
static bool g_seeded = false;
static std::mutex g_rng_mtx;

static void ensure_seeded() {
  if (g_seeded) return;
  g_rng.seed_full();
  g_seed_used = g_rng.s[0] ^ g_rng.s[3];
  g_seeded = true;
}
Xoshiro256pp rng() {
  std::lock_guard<std::mutex> lk(g_rng_mtx);
  ensure_seeded();
  Xoshiro256pp x;
  x.seed(g_rng.next());
  return x;
}
void rng_seed_process(uint64_t seed) {
  std::lock_guard<std::mutex> lk(g_rng_mtx);
  g_rng.seed(seed);
  g_seed_used = seed;
  g_seeded = true;
}
Xoshiro256pp rng_stream(unsigned k) {
  std::lock_guard<std::mutex> lk(g_rng_mtx);
  ensure_seeded();
  Xoshiro256pp x;
  x.seed(g_seed_used);
  for (unsigned i = 0; i <= k; i++) x.jump();
  return x;
}
uint64_t rng_seed_used() { std::lock_guard<std::mutex> lk(g_rng_mtx); ensure_seeded(); return g_seed_used; }
