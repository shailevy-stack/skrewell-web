/* Skrewell engine for the M-VAVE FM-1.
 * A reimplementation of Lazyfish's Skrewell (Reaktor 5), rebuilt from the decoded ensemble
 * (see ENGINE_NOTES.md). Same algorithm as the browser test bench (engine.js), in float C,
 * with fixed memory and no allocation. */
#ifndef SKREWELL_H
#define SKREWELL_H
#include <stdint.h>

#define SKR_NV 8                 /* voices = channels */
#define SKR_NL (SKR_NV * 2)      /* levers: L and R per voice */
#define SKR_MAXBLK 256           /* frames per block, at most */
#ifndef SKR_DLEN
#define SKR_DLEN 8192            /* int16 samples per delay line; longer delays use decimated storage */
#endif

#ifdef SKR_FLOAT_DELAY
typedef float skr_dsample;
#else
typedef int16_t skr_dsample;
#endif

enum { SKR_IMPLANT_QUAD = 0, SKR_XUNG_TEKH = 1, SKR_SUBTOTAL = 2 };
enum { SKR_F_OSC_FREQ, SKR_F_FREQ_MOD, SKR_F_OSC_AMP, SKR_F_AMP_MOD,
       SKR_F_FILTER1, SKR_F_FILTER2, SKR_F_DELAY, SKR_F_FEEDBACK, SKR_NFN };

typedef struct { float a, b; } skr_pair;      /* flow pair: value at FLOW 0 and FLOW 1 */
typedef struct { skr_pair fm, am, res, smt, nrm; } skr_flow;

typedef struct {                               /* one snapshot */
    uint8_t mode;
    float bars[SKR_NFN][SKR_NV];               /* 0..1 */
    float osc, filter, delay, flow;            /* panel knobs 0..1 */
    float out;                                 /* 0..2 (1 = 0 dB before the -18 dB offset) */
    float oscMin, oscMax, cutMin, cutMax, lpMin, lpMax, hpMin, hpMax, delShort, delLong; /* Reaktor pitch */
    skr_flow flow_ab[3];                       /* per mode */
} skr_patch;

typedef struct {                               /* glue compressor (end of chain) */
    float thr_db;    /* threshold, dB (0 .. -36) */
    float ratio;     /* 2 .. 10 */
    float attack_ms, release_ms, makeup_db, mix;
} skr_glue;

typedef struct {                               /* reverb (global FX): 4-line FDN with input diffusion */
    float size;      /* 0..1: room size (delay lengths 35..100 %) */
    float decay;     /* 0..1: RT60 0.3 .. 12 s */
    float damp;      /* 0..1: high damping, bright .. dark */
    float mix;       /* 0..1: the send (0 = off); the dry signal always passes whole */
} skr_verb;
#define SKR_RV_N 4
#define SKR_RV_LEN 3912                       /* longest line at full size + interpolation headroom */
#define SKR_AP_N 4
#define SKR_AP_LEN 400

typedef struct {
    /* hot per-sample state first: small offsets from the engine's base (the big buffers are at the end) */
    float phase[SKR_NL], env[SKR_NL], envLP[SKR_NL];
    float ysh[2][SKR_NL];                      /* lever outputs by sample parity */
    float ic[SKR_NL][4], posF[SKR_NL], posA[SKR_NL];
    uint8_t cvIx[SKR_NL][4]; float cvFr[SKR_NL][2], dinv[SKR_NL];
    uint32_t dpos[SKR_NL];
    uint8_t dshift[SKR_NL]; uint8_t dcount[SKR_NL]; float dacc[SKR_NL];
    float Fs[SKR_NL], Ds[SKR_NL];              /* pitch and delay glides, per lever (each side its own) */
    float scS[2];
    uint32_t scount;                           /* samples rendered (the parity) */
    volatile uint32_t cnt[2];                  /* samples each side has done this block (two cores) */
    void (*fence)(void);                       /* memory barrier between the cores (0: one core) */
    volatile uint8_t broken;                   /* the other core stopped answering */
    /* control-rate values */
    float cF[SKR_NV], cA[SKR_NV], cFV[SKR_NV], cAV[SKR_NV], cDEL[SKR_NV], cFB[SKR_NV];
    float cAp[SKR_NV], cAd[SKR_NV], cFBp[SKR_NV], cFBd[SKR_NV];   /* amp and feedback ramped across the block */
    float cTyp[SKR_NV], svf[SKR_NV][2][3];
    float cFco[SKR_NV], cDco[SKR_NV], cEnvCo[SKR_NV], cRel[SKR_NV];
    float log2fm, amD, k, nrm, smt, cvCo, gCo, scCo, gainT, gainS, scXs, scYs;
    float dz[SKR_NL][3][2], dlp[8][5];       /* band-limiting filters for reduced-rate lines */
    uint16_t rv_pos, ap_pos[SKR_AP_N];
    float rv_lp[SKR_RV_N], rv_g[SKR_RV_N], rv_len[SKR_RV_N], rv_lenT[SKR_RV_N], rv_dco, rv_dry, rv_wet, rv_lenCo;
    float gl_env, gl_gain, gl_att, gl_rel, gl_thr, gl_knee, gl_slope, gl_mk;   /* glue state */
    float sr;
    skr_patch p;                               /* the patch as set */
    skr_patch ps;                              /* ... as the engine plays it: every value glides to p (~15 ms) */
    uint8_t ps_ok;
    skr_glue glue;
    skr_verb verb;
    uint8_t mute;
    float scX, scY;                            /* SCOPE MIX pad 0..1 */
    float sideOut[2][SKR_MAXBLK];
    /* the big buffers */
    float ap_buf[SKR_AP_N][SKR_AP_LEN];
    skr_dsample rv_buf[SKR_RV_N][SKR_RV_LEN];
    skr_dsample scratch[SKR_DLEN];             /* delay-line resampling temp */
    skr_dsample dbuf[SKR_NL][SKR_DLEN];
} skr_engine;

void skr_init(skr_engine *e, float sample_rate);
void skr_default_patch(skr_patch *p);
void skr_load(skr_engine *e, const skr_patch *p);   /* keeps the running state, like Reaktor */
/* Render n stereo frames, float -1..1, interleaved or split. scope (may be NULL) gets
 * n*2*SKR_NV floats: for every frame, each voice's x,y scope point. */
void skr_render(skr_engine *e, float *outL, float *outR, unsigned n, float *scope);
/* the same on two cores: skr_block_begin, then skr_side(e, 0, ...) on one core and skr_side(e, 1, ...) on the other
 * at once (they meet every sample through e->fence and e->cnt), then skr_block_end */
void skr_block_begin(skr_engine *e, unsigned n);
void skr_side(skr_engine *e, int s, unsigned n, float *scope);
void skr_block_end(skr_engine *e, float *outL, float *outR, unsigned n);
void skr_sides_one(skr_engine *e, unsigned n, float *scope);   /* after skr_block_begin, when the other core can't */
#endif
