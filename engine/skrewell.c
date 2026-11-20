/* Skrewell engine: see skrewell.h and ENGINE_NOTES.md. Mirrors engine.js (the browser test bench). */
#include "skrewell.h"
#ifdef PLAT_HOST
#include <string.h>
#else
void *memset(void *d, int c, unsigned n);     /* felucca-bsp chip/libc.c */
void *memcpy(void *d, const void *s, unsigned n);
#endif
#include "fastmath.h"
#define expf fm_expf
#define tanf fm_tanf
#define powf fm_powf
#define log2f fm_log2f
#define sqrtf fm_sqrtf
static inline __attribute__((always_inline)) float skr_floorf(float x) { int32_t i = (int32_t)x; float f = (float)i; return f > x ? f - 1.0f : f; }
static inline __attribute__((always_inline)) float skr_fabsf(float x) { union { float f; uint32_t u; } b; b.f = x; b.u &= 0x7FFFFFFFu; return b.f; }
#define floorf skr_floorf
#define fabsf skr_fabsf

/* The per-sample work runs from RAM (fm1-base's .app_ram), out of the way of the flash cache's misses: the UI's
 * drawing shares that cache, and on the FM-1 its misses made the audio late. Helpers it uses are always inlined. */
#ifdef PLAT_HOST
#define SKR_RAM
#define SKR_FAR(fn) (&(fn))
#else
#define SKR_RAM __attribute__((section(".app_ram"), noinline))
static inline __attribute__((always_inline)) void *skr_far_ptr(void *fn) { void *volatile held = fn; return held; }
#define SKR_FAR(fn) ((__typeof__(&(fn)))skr_far_ptr((void *)&(fn)))
#endif
#define AI __attribute__((always_inline))

#define NV SKR_NV
#define NL SKR_NL
#define MAXDEL_MS 3000.0f

static inline AI float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline AI float lerpf(float a, float b, float t) { return a + (b - a) * t; }
/* fast 2^x, |rel err| < 2e-4 */
static inline AI float fexp2(float x) {
    if (x < -126.f) return 0.f;
    if (x > 126.f) x = 126.f;
    float fi = floorf(x), f = x - fi;
    float p = 1.0f + f * (0.6951786f + f * (0.2261575f + f * 0.0780043f));
    union { float f; int32_t i; } u; u.f = p; u.i += (int32_t)fi << 23; return u.f;
}
static inline float pitch_hz(float p) { return 440.f * fexp2((p - 69.f) * (1.f / 12.f)); }
static inline float hz_pitch(float f) { return 69.f + 12.f * log2f((f > 1e-9f ? f : 1e-9f) / 440.f); }
static inline AI float sel3(float pos, float a, float b, float c) {
    pos = clampf(pos, 0, 2);
    return pos < 1 ? lerpf(a, b, pos) : lerpf(b, c, pos - 1);
}
static inline AI float shaper(float shp, float x) {
    x = clampf(x, 0, 1);
    float x2 = x * x, s = sqrtf(sqrtf(x));
    return sel3(2 * shp, x2 * x2, x, s);
}
static inline float one_pole(float fc, float sr) {
    return 1.f - expf(-6.2831853f * clampf(fc, 0, sr * 0.45f) / sr);
}

static void lp_coef(float *c, float fc, float sr);
static void dl_reshift(skr_engine *e, int li, unsigned ns);
static void verb_control(skr_engine *e);

void skr_default_patch(skr_patch *p) {
    memset(p, 0, sizeof *p);
    for (int f = 0; f < SKR_NFN; f++) for (int v = 0; v < NV; v++) p->bars[f][v] = 0.5f;
    p->osc = p->filter = p->delay = p->flow = 0.5039f; p->out = 1.0079f;
    p->oscMin = p->cutMin = p->lpMin = p->hpMin = p->delLong = -26;
    p->oscMax = p->cutMax = p->lpMax = p->hpMax = p->delShort = 73;
    for (int m = 0; m < 3; m++) {
        skr_flow *f = &p->flow_ab[m];
        f->fm = (skr_pair){4.75f, 12.25f}; f->am = (skr_pair){1.25f, 3.75f};
        f->res = (skr_pair){0.25f, 0.75f}; f->smt = (skr_pair){15.25f, 5.75f};
        f->nrm = (skr_pair){0.75f, 0.26f};
    }
}

void skr_init(skr_engine *e, float sr) {
    memset(e, 0, sizeof *e);
    e->sr = sr;
    skr_default_patch(&e->p);
    e->scX = e->scY = e->scXs = e->scYs = e->scS[0] = e->scS[1] = 0.5f;
    e->glue = (skr_glue){-18.0f, 4.0f, 10.0f, 200.0f, 4.0f, 1.0f};
    e->gl_gain = 1.f;
    e->verb = (skr_verb){0.6f, 0.45f, 0.5f, 0.2f};
    /* every oscillator starts at a different phase (Reaktor's L/R never start identical) */
    uint32_t sd = 22222;
    for (int i = 0; i < NL; i++) {
        sd = (uint32_t)(((uint64_t)sd * 16807u) % 2147483647u);
        e->phase[i] = (float)sd / 2147483647.f;
        e->envLP[i] = 1.f;
    }
    for (int i = 0; i < NL; i++) { e->Fs[i] = 100.f; e->Ds[i] = 10.f; }
    for (int sh = 1; sh < 8; sh++) lp_coef(e->dlp[sh], 0.4f * sr / (float)(1u << sh), sr);
}

#define SKR_GLIDE_MS 15.f                       /* parameter smoothing: a knob step becomes a short glide, not a click */
void skr_load(skr_engine *e, const skr_patch *p) { e->p = *p; }
void skr_morph(skr_engine *e, float ms) {
    if (ms <= SKR_GLIDE_MS || !e->ps_ok) { e->morph_len = 0.f; return; }
    e->m0 = e->ps;
    e->morph_t = 0.f;
    e->morph_len = ms * 0.001f * e->sr;
}

static void patch_glide(skr_engine *e, unsigned n) {
    const unsigned nf = (unsigned)((sizeof(skr_patch) - __builtin_offsetof(skr_patch, bars)) / sizeof(float));
    float *a = &e->ps.bars[0][0];
    const float *b = &e->p.bars[0][0];
    if (!e->ps_ok) {                             /* first block: no glide */
        e->ps = e->p;
        e->ps_ok = 1;
        return;
    }
    if (e->morph_len > 0.f) {                    /* a morph: an S-curve from where it started */
        const float *z = &e->m0.bars[0][0];
        float k = (e->morph_t += (float)n) / e->morph_len;
        if (k >= 1.f) { k = 1.f; e->morph_len = 0.f; }
        k = k * k * (3.f - 2.f * k);
        e->ps.mode = e->p.mode;
        for (unsigned i = 0; i < nf; i++)
            a[i] = z[i] + (b[i] - z[i]) * k;
        return;
    }
    if (e->ps.mode != e->p.mode) {               /* a new mode: no glide */
        e->ps = e->p;
        return;
    }
    float cb = 1.f - fm_exp2f(-(float)n / (e->sr * SKR_GLIDE_MS * 0.001f) * 1.4426950f);
    for (unsigned i = 0; i < nf; i++) {
        float d = b[i] - a[i];
        a[i] = (d > -1e-6f && d < 1e-6f) ? b[i] : a[i] + cb * d;
    }
}

static void control(skr_engine *e, unsigned n) {
    int reshifted = 0;
    patch_glide(e, n);
    const float sr = e->sr; const skr_patch *P = &e->ps; const int m = P->mode;
    const float inv_n = 1.f / (float)(n ? n : 1u);
    const skr_flow *F = &P->flow_ab[m];
    float fl = clampf(P->flow, 0, 1);
    float fm = lerpf(F->fm.a, F->fm.b, fl);
    e->log2fm = log2f(fm > 1e-3f ? fm : 1e-3f);
    e->amD = lerpf(F->am.a, F->am.b, fl);
    float res = clampf(lerpf(F->res.a, F->res.b, fl), 0, 0.98f);
    e->smt = lerpf(F->smt.a, F->smt.b, fl);
    e->nrm = lerpf(F->nrm.a, F->nrm.b, fl);
    e->k = 2.f * (1.f - res);
    for (int v = 0; v < NV; v++) {
        e->cF[v] = pitch_hz(lerpf(P->oscMin, P->oscMax, shaper(P->osc, P->bars[SKR_F_OSC_FREQ][v])));
        e->cFV[v] = P->bars[SKR_F_FREQ_MOD][v];
        e->cAp[v] = e->cA[v];
        e->cA[v] = P->bars[SKR_F_OSC_AMP][v];
        e->cAd[v] = (e->cA[v] - e->cAp[v]) * inv_n;
        e->cAV[v] = P->bars[SKR_F_AMP_MOD][v];
        float a = shaper(P->filter, P->bars[SKR_F_FILTER1][v]), b = shaper(P->filter, P->bars[SKR_F_FILTER2][v]);
        float fcA = 1000.f, fcB = 1000.f;
        if (m == SKR_XUNG_TEKH) { fcA = pitch_hz(lerpf(P->cutMin, P->cutMax, a)); e->cTyp[v] = 2.f * b; }
        else if (m == SKR_IMPLANT_QUAD) {
            float q = b + 0.5f * a * (1.f - b);
            fcB = pitch_hz(lerpf(P->lpMin, P->lpMax, q));
            fcA = pitch_hz(lerpf(P->hpMin, P->hpMax, a * q));
        }
        float dp = lerpf(P->delShort, P->delLong, shaper(P->delay, P->bars[SKR_F_DELAY][v]));
        e->cDEL[v] = clampf(1000.f / pitch_hz(dp), 0, MAXDEL_MS);
        e->cFBp[v] = e->cFB[v];
        e->cFB[v] = P->bars[SKR_F_FEEDBACK][v];
        e->cFBd[v] = (e->cFB[v] - e->cFBp[v]) * inv_n;
        for (int s = 0; s < 2; s++) {
            float fc = s == 0 ? fcA : fcB;
            float g = tanf(3.14159265f * clampf(fc, 1, sr * 0.45f) / sr);
            float a1 = 1.f / (1.f + g * (g + e->k));
            e->svf[v][s][0] = a1; e->svf[v][s][1] = g * a1; e->svf[v][s][2] = g * g * a1;
        }
        float smt = e->smt, DEL = e->cDEL[v] > 1e-3f ? e->cDEL[v] : 1e-3f;
        e->cFco[v] = one_pole(e->Fs[v] / smt, sr);
        e->cDco[v] = one_pole(pitch_hz(clampf(hz_pitch(500.f * smt / DEL), -80, 0)), sr);
        e->cEnvCo[v] = one_pole(1000.f / (smt * DEL), sr);
        float relMs = smt * DEL > 0.05f ? smt * DEL : 0.05f;  /* Peak Detector: time constant smt*DEL ms */
        e->cRel[v] = expf(-1.f / (relMs * sr * 0.001f));
        /* storage rate for this voice's two lines: from the larger of target and current delay */
        float need = (e->cDEL[v] > e->Ds[v] ? e->cDEL[v] : e->Ds[v]) * sr * 0.001f + 2.f * 128.f;
        for (int s = 0; s < 2; s++) {
            int li = s * NV + v; unsigned sh = e->dshift[li];
            while (sh < 7 && need / (float)(1u << sh) > (float)(SKR_DLEN - 8)) sh++;
            while (sh > 0 && need / (float)(1u << (sh - 1)) < 0.7f * (float)(SKR_DLEN - 8)) sh--;
            /* a line changes its storage rate by resampling its whole buffer: one line a block at most, so a knob
             * that moves every delay at once never makes a block run late (the rest follow, 2.9 ms apart) */
            if (sh != e->dshift[li] && !reshifted) {
                dl_reshift(e, li, sh);
                reshifted = 1;
            }
            e->dinv[li] = 1.f / (float)(1u << e->dshift[li]);
        }
    }
    float db = sel3(P->out, -36, 0, 18);
    db = floorf(db * 2.f + 0.5f) * 0.5f - 18.f;
    e->gainT = e->mute ? 0.f : powf(10.f, db / 20.f);
    e->cvCo = one_pole(1000.f / (6.2831853f * 20.f), sr);
    {   /* crossvoice (Fr.V selector, Audio Smoother 20 ms): position smoothed once a block, blend weights kept */
        float cb = 1.f - fm_exp2f((float)n * fm_log2f(1.f - e->cvCo));
        for (int li = 0; li < NL; li++) {
            int v = li % NV;
            e->posF[li] += cb * (8.f * e->cFV[v] - e->posF[li]);
            e->posA[li] += cb * (8.f * e->cAV[v] - e->posA[li]);
            for (int k = 0; k < 2; k++) {
                float q = clampf(k ? e->posA[li] : e->posF[li], 0, 7);
                int i0 = (int)q; int i1 = i0 < 7 ? i0 + 1 : 7;
                e->cvIx[li][2 * k] = (uint8_t)i0; e->cvIx[li][2 * k + 1] = (uint8_t)i1;
                e->cvFr[li][k] = q - (float)i0;
            }
        }
    }
    e->gCo = one_pole(1000.f / (6.2831853f * 30.f), sr);
    e->scCo = one_pole(pitch_hz(-40), sr);
    verb_control(e);
    const skr_glue *g = &e->glue;
    e->gl_att = 1.f - expf(-1.f / (g->attack_ms * 0.001f * sr));
    e->gl_rel = 1.f - expf(-1.f / (g->release_ms * 0.001f * sr));
    e->gl_thr = g->thr_db / 6.0206f;
    e->gl_knee = 6.f / 6.0206f;
    e->gl_slope = 1.f - 1.f / (g->ratio > 1.f ? g->ratio : 1.f);
    e->gl_mk = g->makeup_db / 6.0206f;
}

/* Delay lines. Short delays are stored at full rate. Long ones are stored at sr/2^shift
 * (band-limited on the way in and out, so the reduced rate adds no stair-step images) to fit
 * the FM-1's RAM. When a line changes rate its contents are resampled, not cleared. */
/* 16-bit float for delay storage: sign, 5-bit exponent covering 2^-29..2^2, 10-bit mantissa.
 * Same memory as int16, but the error follows the signal (about -66 dB below it) instead of
 * sitting at a fixed floor that the norm stage would amplify on quiet patches. */
static inline AI int16_t h_enc(float x) {       /* float -> 16 bits: the exponent rebased, the mantissa rounded */
    union { float f; uint32_t u; } v; v.f = x;
    int32_t a = (int32_t)(v.u & 0x7FFFFFFFu) - (int32_t)(98u << 23) + 0x1000;   /* 98 = 127 - 29 */
    if (a < 0) a = 0;                                   /* below 2^-29: zero */
    if (a > (int32_t)(0x7FFFu << 13)) a = (int32_t)(0x7FFFu << 13);   /* above ~8: the largest */
    return (int16_t)(((uint32_t)a >> 13) | ((v.u >> 16) & 0x8000u));
}
static inline AI float h_dec(int16_t h) {          /* 16 bits -> float (0 comes back as 2^-29: inaudible) */
    union { float f; uint32_t u; } v;
    uint32_t u = (uint16_t)h;
    v.u = (((u & 0x7FFFu) << 13) + (98u << 23)) | ((u & 0x8000u) << 16);
    return v.f;
}
#ifdef SKR_FLOAT_DELAY
#define D_ENC(x) (x)
#define D_DEC(x) (x)
#else
#define D_ENC(x) h_enc(x)
#define D_DEC(x) h_dec(x)
#endif

#define dl_scratch (e->scratch)
static inline AI float bq(float *z, const float *c, float x) {     /* TDF-II biquad: c = b0 b1 b2 a1 a2 */
    float y = c[0] * x + z[0];
    z[0] = c[1] * x - c[3] * y + z[1];
    z[1] = c[2] * x - c[4] * y;
    return y;
}
static void lp_coef(float *c, float fc, float sr) {             /* Butterworth low-pass */
    float w = tanf(3.14159265f * fc / sr), q = 0.70710678f, n = 1.f / (1.f + w / q + w * w);
    c[0] = w * w * n; c[1] = 2 * c[0]; c[2] = c[0]; c[3] = 2.f * (w * w - 1.f) * n; c[4] = (1.f - w / q + w * w) * n;
}
static void hp_coef(float *c, float fc, float sr) {             /* Butterworth high-pass */
    float w = tanf(3.14159265f * fc / sr), q = 0.70710678f, n = 1.f / (1.f + w / q + w * w);
    c[0] = n; c[1] = -2.f * n; c[2] = n; c[3] = 2.f * (w * w - 1.f) * n; c[4] = (1.f - w / q + w * w) * n;
}
static void dl_reshift(skr_engine *e, int li, unsigned ns) {
    /* the line's samples moved as they are (no decode, no averaging: cheap enough for any block), every g-th one
     * kept or each repeated g times; only the audio already in the line at the switch is touched */
    unsigned os = e->dshift[li]; if (ns == os) return;
    skr_dsample *b = e->dbuf[li]; unsigned p = e->dpos[li];
    for (unsigned i = 0; i < SKR_DLEN; i++) { unsigned j = p + i; if (j >= SKR_DLEN) j -= SKR_DLEN; dl_scratch[i] = b[j]; } /* oldest..newest */
    if (ns > os) {                       /* fewer samples per second: the newest part, every g-th sample */
        unsigned g = 1u << (ns - os), keep = SKR_DLEN / g;
        for (unsigned i = 0; i < SKR_DLEN - keep; i++) b[i] = 0;
        for (unsigned i = 0; i < keep; i++) b[SKR_DLEN - keep + i] = dl_scratch[SKR_DLEN - keep * g + i * g + g - 1];
    } else {                             /* more samples per second: the newest part, each sample g times */
        unsigned g = 1u << (os - ns), src = SKR_DLEN / g;
        for (unsigned i = 0; i < SKR_DLEN; i++) b[i] = dl_scratch[SKR_DLEN - src + i / g];
    }
    e->dpos[li] = 0; e->dshift[li] = (uint8_t)ns; e->dcount[li] = 0; e->dacc[li] = 0;
}
static inline AI void dl_write(skr_engine *e, int li, float x) {
    unsigned shift = e->dshift[li];
    if (shift) { x = bq(e->dz[li][0], e->dlp[shift], x); }
    else { e->dbuf[li][e->dpos[li]] = D_ENC(x); if (++e->dpos[li] >= SKR_DLEN) e->dpos[li] = 0; return; }
    e->dacc[li] += x;
    if (++e->dcount[li] >= (1u << shift)) {
        e->dbuf[li][e->dpos[li]] = D_ENC(e->dacc[li] / (float)(1u << shift));
        if (++e->dpos[li] >= SKR_DLEN) e->dpos[li] = 0;
        e->dcount[li] = 0; e->dacc[li] = 0;
    }
}
static inline AI float dl_read(skr_engine *e, int li, float dsamp) {
    unsigned shift = e->dshift[li];
    float d = (dsamp + (float)e->dcount[li]) * e->dinv[li];
    if (d < 1.f) d = 1.f;
    float rp = (float)e->dpos[li] - d;
    while (rp < 0) rp += SKR_DLEN;
    unsigned r0 = (unsigned)rp; float rf = rp - (float)r0;
    if (r0 >= SKR_DLEN) r0 -= SKR_DLEN;
    unsigned r1 = r0 + 1 < SKR_DLEN ? r0 + 1 : 0;
    float a = D_DEC(e->dbuf[li][r0]), b = D_DEC(e->dbuf[li][r1]);
    float y = a + (b - a) * rf;
    if (shift) { y = bq(e->dz[li][1], e->dlp[shift], y); y = bq(e->dz[li][2], e->dlp[shift], y); }
    return y;
}

/* last-stage safety: linear below 0.8, smooth knee to +-1 */
static inline AI float softclip(float x) {
    float a = fabsf(x);
    if (a <= 0.8f) return x;
    float t = (a - 0.8f) / 0.2f, y = 0.8f + 0.2f * t / (1.f + t);  /* -> 1.0 as a -> inf */
    return x < 0 ? -y : y;
}

/* glue compressor: stereo-linked, feed-forward, peak-ish detector, soft knee */
static inline AI float flog2(float x) {           /* fast log2, |err| < 0.005 */
    union { float f; uint32_t u; } v; v.f = x;
    float e = (float)(int)((v.u >> 23) & 255) - 128.f;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;       /* mantissa in [1,2) */
    float m = v.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}
static inline AI void glue(skr_engine *e, float *l, float *r) {
    const skr_glue *g = &e->glue;
    if (g->mix <= 0.f) return;
    float lvl = fabsf(*l) > fabsf(*r) ? fabsf(*l) : fabsf(*r);
    e->gl_env += (lvl > e->gl_env ? e->gl_att : e->gl_rel) * (lvl - e->gl_env);
    /* everything in log2 units (1 = 6.02 dB) */
    float over = flog2(e->gl_env + 1e-9f) - e->gl_thr, knee = e->gl_knee, red;
    if (over <= -knee * 0.5f) red = 0;
    else if (over >= knee * 0.5f) red = over * e->gl_slope;
    else { float t = over + knee * 0.5f; red = e->gl_slope * t * t / (2.f * knee); }
    float gain = fexp2(e->gl_mk - red);
    float wet = g->mix, k = (1.f - wet) + gain * wet;
    *l *= k; *r *= k; e->gl_gain = gain;
}

/* ---- reverb: input diffusion (2 allpasses per side), then a 4-line feedback delay network
 * (Householder matrix, one-pole damping per line). Lines stored in the 16-bit float format. */
static const float RV_LEN[SKR_RV_N] = {3109.f, 3371.f, 3581.f, 3907.f};
static const uint16_t AP_LEN[SKR_AP_N] = {142, 379, 151, 397};
static void verb_control(skr_engine *e) {
    const skr_verb *v = &e->verb; float sr = e->sr;
    float rt = 0.3f * fm_exp2f(clampf(v->decay, 0, 1) * 5.3219f);          /* 0.3 * 40^decay s */
    float sc = 0.35f + 0.65f * clampf(v->size, 0, 1);
    for (int i = 0; i < SKR_RV_N; i++) {
        e->rv_lenT[i] = RV_LEN[i] * sc * (sr / 44100.f);
        if (e->rv_len[i] <= 0) e->rv_len[i] = e->rv_lenT[i];
        e->rv_g[i] = fm_exp2f(-3.f * 3.3219281f * e->rv_len[i] / (rt * sr));   /* 10^(-3 len / (rt sr)) */
    }
    float fc = 16000.f * fm_exp2f(-clampf(v->damp, 0, 1) * 3.64f);       /* 16 kHz .. 1.3 kHz */
    e->rv_dco = one_pole(fc, sr);
    float m = clampf(v->mix, 0, 1);              /* a send: the dry signal stays whole, SEND sets the reverb's share */
    if (v->insert) { e->rv_dry = 1.f - m; e->rv_wet = m; }   /* an insert: dry/wet */
    else { e->rv_dry = 1.f; e->rv_wet = 0.7f * m; }
    e->rv_hpon = v->lc_hz > 20.f;
    if (e->rv_hpon) hp_coef(e->rv_hpc, clampf(v->lc_hz, 20.f, 0.4f * sr), sr);
    e->rv_lpon = v->hc_hz > 0.f && v->hc_hz < 0.4f * sr;
    if (e->rv_lpon) lp_coef(e->rv_lpc, clampf(v->hc_hz, 20.f, 0.4f * sr), sr);
    e->rv_lenCo = 1.f - fm_expf(-1.f / (0.15f * sr));                      /* size glides over ~150 ms */
}
static inline AI float ap_proc(skr_engine *e, int k, float x) {
    float *b = e->ap_buf[k]; unsigned p = e->ap_pos[k];
    float d = b[p], y = d - 0.6f * x;
    b[p] = x + 0.6f * y;
    if (++p >= AP_LEN[k]) p = 0;
    e->ap_pos[k] = (uint16_t)p;
    return y;
}
static inline AI void verb(skr_engine *e, float *l, float *r) {
    if (e->verb.mix <= 0.f) return;
    float a = ap_proc(e, 1, ap_proc(e, 0, *l)), b = ap_proc(e, 3, ap_proc(e, 2, *r));
    float d[SKR_RV_N], s = 0; unsigned p = e->rv_pos;
    for (int i = 0; i < SKR_RV_N; i++) {
        e->rv_len[i] += e->rv_lenCo * (e->rv_lenT[i] - e->rv_len[i]);
        float rp = (float)p - e->rv_len[i]; if (rp < 0) rp += SKR_RV_LEN;
        unsigned r0 = (unsigned)rp; float f = rp - (float)r0; unsigned r1 = r0 + 1 < SKR_RV_LEN ? r0 + 1 : 0;
        float x0 = D_DEC(e->rv_buf[i][r0]), x1 = D_DEC(e->rv_buf[i][r1]);
        float x = x0 + (x1 - x0) * f;
        e->rv_lp[i] += e->rv_dco * (x - e->rv_lp[i]);
        d[i] = e->rv_lp[i] * e->rv_g[i]; s += d[i];
    }
    s *= 0.5f;
    float in[SKR_RV_N] = {a, b, a, -b};
    for (int i = 0; i < SKR_RV_N; i++) e->rv_buf[i][p] = D_ENC(clampf(d[i] - s + 0.5f * in[i], -3.9f, 3.9f));
    if (++p >= SKR_RV_LEN) p = 0;
    e->rv_pos = (uint16_t)p;
    float wl = d[0] - d[2], wr = d[1] - d[3];   /* uncorrelated sides; each keeps its own side of the image */
    if (e->rv_hpon) { wl = bq(e->rv_fz[0][0], e->rv_hpc, wl); wr = bq(e->rv_fz[1][0], e->rv_hpc, wr); }
    if (e->rv_lpon) { wl = bq(e->rv_fz[0][1], e->rv_lpc, wl); wr = bq(e->rv_fz[1][1], e->rv_lpc, wr); }
    *l = *l * e->rv_dry + wl * e->rv_wet; *r = *r * e->rv_dry + wr * e->rv_wet;
}

/* ---- the render, by side. Each side (L levers, R levers) reads only the other side's PREVIOUS sample (and its
 * own), so within a sample the two sides are independent and can run on the two cores, meeting once a sample.
 * (Reaktor lets R see L's current sample; on the bench that ordering made no audible difference.)
 * Lever outputs are double-buffered by sample parity: ysh[t & 1] is written at sample t, ysh[(t + 1) & 1] read. */
static SKR_RAM void side_range(skr_engine *e, int s, unsigned i0, unsigned i1, float *scope, int sync) {
    const float sr = e->sr, k = e->k; const int m = e->p.mode;
    const int oth = (1 - s) * NV;
    float *out = e->sideOut[s];
    const float scT = s ? e->scY : e->scX;
    unsigned spin = 0;
    for (unsigned i = i0; i < i1; i++) {
        const unsigned t = e->scount + i;
        const float *src = e->ysh[(t + 1) & 1];
        float *dst = e->ysh[t & 1];
        float sum = 0;
        e->scS[s] += e->scCo * (scT - e->scS[s]);
        for (int v = 0; v < NV; v++) {
            const int li = s * NV + v;
            e->Fs[li] += e->cFco[v] * (e->cF[v] - e->Fs[li]);
            e->Ds[li] += e->cDco[v] * (e->cDEL[v] - e->Ds[li]);
            float dsamp = e->Ds[li] * sr * 0.001f;
            if (dsamp < 1) dsamp = 1;
            float FMs, AMs;
            if (m == SKR_SUBTOTAL) {
                float sm = 0;
                for (int u = 0; u < NV; u++) if (u != v) sm += src[oth + u];
                FMs = e->cFV[v] * sm; AMs = e->cAV[v] * sm;
            } else {
                const uint8_t *ix = e->cvIx[li];
                float a0 = src[oth + ix[0]], a1 = src[oth + ix[1]], b0 = src[oth + ix[2]], b1 = src[oth + ix[3]];
                FMs = a0 + (a1 - a0) * e->cvFr[li][0];
                AMs = b0 + (b1 - b0) * e->cvFr[li][1];
            }
            /* LEVER */
            float pos = clampf(0.5f * FMs + 0.5f, 0, 1);
            float freq = e->Fs[li] * fexp2(e->log2fm * (2.f * pos - 1.f));
            const float ri = (float)(i + 1u);
            float A = e->cAp[v] + e->cAd[v] * ri, amp = A + AMs * (e->amD * A);
            float ph = e->phase[li] + freq / sr; ph -= floorf(ph); e->phase[li] = ph;
            float osc;
            if (m == SKR_SUBTOTAL) { float u = 2.f * ph - 1.f; osc = amp * 4.f * u * (1.f - fabsf(u)); }
            else osc = ph < 0.5f ? amp : -amp;
            float FB = e->cFBp[v] + e->cFBd[v] * ri, ypr = src[li], x;
            float *ic = e->ic[li];
            if (m == SKR_XUNG_TEKH) {
                const float *c = e->svf[v][0];
                float v3 = osc - ic[1], v1 = c[0] * ic[0] + c[1] * v3, v2 = ic[1] + c[1] * ic[0] + c[2] * v3;
                ic[0] = 2 * v1 - ic[0]; ic[1] = 2 * v2 - ic[1];
                float hp = osc - k * v1 - v2;
                x = k * sel3(e->cTyp[v], v2, v1, hp) + FB * ypr;
            } else if (m == SKR_IMPLANT_QUAD) {
                float xi = osc + FB * ypr;
                const float *c = e->svf[v][0];
                float v3 = xi - ic[1], v1 = c[0] * ic[0] + c[1] * v3, v2 = ic[1] + c[1] * ic[0] + c[2] * v3;
                ic[0] = 2 * v1 - ic[0]; ic[1] = 2 * v2 - ic[1];
                float hp = k * (xi - k * v1 - v2);
                c = e->svf[v][1];
                v3 = hp - ic[3]; v1 = c[0] * ic[2] + c[1] * v3; v2 = ic[3] + c[1] * ic[2] + c[2] * v3;
                ic[2] = 2 * v1 - ic[2]; ic[3] = 2 * v2 - ic[3];
                x = k * v2;
            } else x = osc + FB * ypr;
            if (!(x > -1e6f && x < 1e6f)) x = 0;
            /* Single Delay */
            float d = dl_read(e, li, dsamp);   /* read before write: delay >= 1 sample */
            dl_write(e, li, x);
            /* norm */
            float ad = fabsf(d), en = e->env[li] * e->cRel[v];
            if (ad > en) en = ad;
            e->env[li] = en;
            float ec = clampf(en, e->nrm, 300.f);
            e->envLP[li] += e->cEnvCo[v] * (ec - e->envLP[li]);
            float o = d / (e->envLP[li] > 1e-4f ? e->envLP[li] : 1e-4f);
            dst[li] = o;
            sum += o;
            if (scope) scope[i * 2 * NV + 2 * v + s] = lerpf(m == SKR_SUBTOTAL ? osc : FMs, o, e->scS[s]);
        }
        out[i] = sum;
        if (sync) {                                   /* meet the other core: both done with sample t */
            e->cnt[s] = i + 1;
            if (e->fence) e->fence();
            while (e->cnt[1 - s] < i + 1)
                if (++spin > 40000000u) { e->broken = 1; return; }   /* (the other core is gone) */
        }
    }
}

void skr_block_begin(skr_engine *e, unsigned n) {
    control(e, n);
    e->cnt[0] = e->cnt[1] = 0;
    if (e->fence) e->fence();
}

SKR_RAM void skr_side(skr_engine *e, int s, unsigned n, float *scope) { side_range(e, s, 0, n, scope, 1); }

SKR_RAM void skr_block_end(skr_engine *e, float *outL, float *outR, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        e->gainS += e->gCo * (e->gainT - e->gainS);
        float l = e->sideOut[0][i] * e->gainS, r = e->sideOut[1][i] * e->gainS;
        verb(e, &l, &r);
        glue(e, &l, &r);
        outL[i] = softclip(l); outR[i] = softclip(r);
    }
    e->scount += n;
}

SKR_RAM void skr_sides_one(skr_engine *e, unsigned n, float *scope) {   /* both sides on this core, sample by sample */
    for (unsigned i = 0; i < n; i++) {
        side_range(e, 0, i, i + 1, scope, 0);
        side_range(e, 1, i, i + 1, scope, 0);
    }
}

void skr_render(skr_engine *e, float *outL, float *outR, unsigned n, float *scope) {   /* one core */
    control(e, n);
    SKR_FAR(skr_sides_one)(e, n, scope);
    SKR_FAR(skr_block_end)(e, outL, outR, n);
}
