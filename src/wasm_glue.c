/* SKREWELL web: the FM-1 engine (dsp/skrewell.c) compiled to WebAssembly, one core, 128-frame blocks. */
#include "skrewell.h"
#include <string.h>

void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

extern const unsigned skr_bank1_count;
extern const char *const skr_bank1_names[];
extern const skr_patch skr_bank1[];

#define EXPORT(name) __attribute__((export_name(#name)))
static skr_engine E;
static float outL[SKR_MAXBLK], outR[SKR_MAXBLK], scope[SKR_MAXBLK * 2 * SKR_NV];
static skr_patch tmp;

EXPORT(init) void w_init(float sr) { skr_init(&E, sr); E.p = skr_bank1[0]; }
EXPORT(render) void w_render(unsigned n) { if (n > SKR_MAXBLK) n = SKR_MAXBLK; skr_render(&E, outL, outR, n, scope); }
EXPORT(out_l) float *w_out_l(void) { return outL; }
EXPORT(out_r) float *w_out_r(void) { return outR; }
EXPORT(scope_ptr) float *w_scope(void) { return scope; }
/* the patch: JS reads and writes it as bytes (a preset or a scene) and by field */
EXPORT(patch_ptr) skr_patch *w_patch(void) { return &E.p; }
EXPORT(patch_size) unsigned w_patch_size(void) { return sizeof(skr_patch); }
EXPORT(tmp_ptr) skr_patch *w_tmp(void) { return &tmp; }
EXPORT(load_tmp) void w_load_tmp(void) { E.p = tmp; }
EXPORT(bank_count) unsigned w_bank_count(void) { return skr_bank1_count; }
EXPORT(bank_name) const char *w_bank_name(unsigned k) { return k < skr_bank1_count ? skr_bank1_names[k] : ""; }
EXPORT(bank_ptr) const skr_patch *w_bank(unsigned k) { return &skr_bank1[k < skr_bank1_count ? k : 0]; }
EXPORT(load_bank) void w_load_bank(unsigned k) { if (k < skr_bank1_count) E.p = skr_bank1[k]; }
EXPORT(set_bar) void w_set_bar(int f, int v, float x) { E.p.bars[f & 7][v & 7] = x; }
EXPORT(get_bar) float w_get_bar(int f, int v) { return E.p.bars[f & 7][v & 7]; }
EXPORT(set_knob) void w_set_knob(int k, float x) { float *p[4] = {&E.p.osc, &E.p.filter, &E.p.delay, &E.p.flow}; *p[k & 3] = x; }
EXPORT(get_knob) float w_get_knob(int k) { const float *p[4] = {&E.p.osc, &E.p.filter, &E.p.delay, &E.p.flow}; return *p[k & 3]; }
EXPORT(set_mode) void w_set_mode(int m) { E.p.mode = (uint8_t)(m < 0 ? 0 : m > 2 ? 2 : m); }
EXPORT(get_mode) int w_get_mode(void) { return E.p.mode; }
EXPORT(set_verb) void w_set_verb(float size, float decay, float damp, float mix, float lc_hz, float hc_hz, int insert) { E.verb = (skr_verb){size, decay, damp, mix, lc_hz, hc_hz, (uint8_t)(insert != 0)}; }
EXPORT(set_glue) void w_set_glue(float thr, float ratio, float rel, float mk, float mix) { E.glue = (skr_glue){thr, ratio, 10.f, rel, mk, mix}; }
EXPORT(ps_bar) float w_ps_bar(int f, int v) { return E.ps.bars[f & 7][v & 7]; }   /* what plays (tests) */
EXPORT(morph) void w_morph(float ms) { skr_morph(&E, ms); }
EXPORT(ps_ptr) skr_patch *w_ps(void) { return &E.ps; }                 /* what plays now (a morph on the way) */
EXPORT(morphing) int w_morphing(void) { return E.morph_len > 0.f; }
EXPORT(glue_gain) float w_glue_gain(void) { return E.gl_gain; }
EXPORT(set_scope_mix) void w_set_scope_mix(float x, float y) { E.scX = x; E.scY = y; }
