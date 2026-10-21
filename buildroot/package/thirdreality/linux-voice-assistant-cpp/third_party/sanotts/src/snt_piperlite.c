/* snt_piperlite.c -- portable fp32 reference of the piperlite decoder.
 * Plain C99, no malloc: all working memory comes from the caller's arena.
 * Semantics mirror tools/train_roota_piper_decoder_student.py exactly
 * (variant "piperlite", leaky_relu): see snt_piperlite.h for the graph.
 *
 * Weight slot order is fixed by tools/export_piperlite_golden.py; meta.bin
 * carries dims plus a per-slot (offset,size) table, and init() verifies each
 * slot size against the shape implied by the dims, so a stale or reordered
 * blob fails loudly instead of decoding garbage.
 */
#include "snt_piperlite.h"

#include <math.h>
#include <string.h>

#define PL_HOP SNT_PIPERLITE_HOP

/* ---- meta.bin parsing ------------------------------------------------- */

static int32_t rd_i32(const unsigned char *p) {
    /* little-endian, alignment-safe */
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static float rd_f32(const unsigned char *p) {
    union { uint32_t u; float f; } v;
    v.u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return v.f;
}

/* Expected float count of weight slot `idx` given the model dims;
 * returns -1 for an out-of-range slot. Order must match the exporter. */
static long slot_floats(const snt_piperlite_model *m, int idx) {
    static const int bk[3] = {3, 5, 7}; /* residual-bank branch kernels */
    if (idx < 0) return -1;
    if (idx == 0) return (long)m->c0 * m->in_ch * 7;
    if (idx == 1) return m->c0;
    /* three upsample stages, each: up_w, up_b, then 3 branches x 4 tensors */
    {
        static const int up_k[3] = {16, 16, 8};
        int in_c[3], out_c[3];
        int s, base = 2;
        in_c[0] = m->c0; out_c[0] = m->c1;
        in_c[1] = m->c1; out_c[1] = m->c2;
        in_c[2] = m->c2; out_c[2] = m->c3;
        for (s = 0; s < 3; s++) {
            if (idx == base) return (long)in_c[s] * out_c[s] * up_k[s];
            if (idx == base + 1) return out_c[s];
            if (idx < base + 14) {
                int r = idx - (base + 2);      /* 0..11 within the bank */
                int branch = r / 4, part = r % 4;
                int ch = out_c[s];
                if (part == 0 || part == 2) return (long)ch * ch * bk[branch];
                return ch;
            }
            base += 14;
        }
        /* base == 44 here */
        if (idx == base) return (long)m->c3 * 7;      /* post.weight [1,c3,7] */
        if (idx == base + 1) return 1;                 /* post.bias */
        base += 2;
        if (m->pf_channels > 0) {
            int l;
            if (idx == base) return (long)m->pf_channels * m->pf_kernel;
            if (idx == base + 1) return m->pf_channels;
            base += 2;
            for (l = 0; l < m->pf_layers; l++) {
                if (idx == base) return 1;             /* unit scale */
                if (idx == base + 1 || idx == base + 3)
                    return (long)m->pf_channels * m->pf_channels * 3;
                if (idx == base + 2 || idx == base + 4) return m->pf_channels;
                base += 5;
            }
            if (idx == base) return (long)m->pf_channels * m->pf_kernel;
            if (idx == base + 1) return 1;
        }
    }
    return -1;
}

int snt_piperlite_init(snt_piperlite_model *m,
                       const void *meta, size_t meta_bytes,
                       const float *weights, size_t weight_floats) {
    const unsigned char *p = (const unsigned char *)meta;
    int i, expected_tensors;
    if (!m || !p || !weights) return -1;
    if (meta_bytes < 12 * 4) return -2;
    memset(m, 0, sizeof *m);
    if (rd_i32(p) != (int32_t)SNT_PIPERLITE_MAGIC) return -3;
    if (rd_i32(p + 4) != 1) return -4; /* version */
    m->in_ch = rd_i32(p + 8);
    m->c0 = rd_i32(p + 12);
    m->c1 = rd_i32(p + 16);
    m->c2 = rd_i32(p + 20);
    m->c3 = rd_i32(p + 24);
    m->pf_channels = rd_i32(p + 28);
    m->pf_layers = rd_i32(p + 32);
    m->pf_kernel = rd_i32(p + 36);
    m->pf_scale = rd_f32(p + 40);
    m->n_tensors = rd_i32(p + 44);
    if (m->in_ch <= 0 || m->c0 <= 0 || m->c1 <= 0 || m->c2 <= 0 || m->c3 <= 0)
        return -5;
    if (m->pf_channels < 0 || m->pf_layers < 0 ||
        (m->pf_channels == 0) != (m->pf_layers == 0))
        return -5;
    if (m->pf_channels > 0 && (m->pf_kernel <= 0 || m->pf_kernel % 2 == 0))
        return -5;
    expected_tensors =
        46 + (m->pf_channels > 0 ? 4 + 5 * m->pf_layers : 0);
    if (m->n_tensors != expected_tensors ||
        m->n_tensors > SNT_PIPERLITE_MAX_TENSORS)
        return -6;
    if (meta_bytes < (size_t)(12 + 2 * m->n_tensors) * 4) return -2;
    for (i = 0; i < m->n_tensors; i++) {
        long off = rd_i32(p + 48 + 8 * i);
        long size = rd_i32(p + 52 + 8 * i);
        long want = slot_floats(m, i);
        if (off < 0 || size <= 0 || want < 0) return -7;
        if (size != want) return -8;
        if ((size_t)off + (size_t)size > weight_floats) return -9;
        m->w[i] = weights + off;
    }
    return 0;
}

/* ---- int8 container (LVA tools/make_voice_files.py) -------------------- */

#define PL_Q8_MAGIC 0x4438514Cu /* 'LQ8D' */

static uint32_t rd_u32(const unsigned char *p) { return (uint32_t)rd_i32(p); }

int snt_piperlite_init_q8(snt_piperlite_model *m, const void *blob,
                          size_t blob_bytes) {
    const unsigned char *p = (const unsigned char *)blob;
    uint32_t meta_bytes, n, i;
    size_t table;
    int rc;
    if (!m || !p || blob_bytes < 16) return -1;
    if (rd_u32(p) != PL_Q8_MAGIC || rd_u32(p + 4) != 1) return -3;
    meta_bytes = rd_u32(p + 8);
    n = rd_u32(p + 12);
    if (n == 0 || n > SNT_PIPERLITE_MAX_TENSORS) return -6;
    table = ((16u + meta_bytes + 15u) & ~(size_t)15u);
    if (table + 16u * n > blob_bytes) return -2;
    /* Validate dims and slot sizes with the fp32 parser; the weight pointer
     * is a placeholder, every slot is rebound below. */
    {
        size_t max_floats = 0;
        for (i = 0; i < n; i++) {
            size_t end = (size_t)rd_i32(p + 16 + 48 + 8 * i) +
                         (size_t)rd_i32(p + 16 + 52 + 8 * i);
            if (end > max_floats) max_floats = end;
        }
        rc = snt_piperlite_init(m, p + 16, meta_bytes, (const float *)p,
                                max_floats);
        if (rc != 0) return rc;
    }
    if ((int)n != m->n_tensors) return -6;
    for (i = 0; i < n; i++) {
        const unsigned char *rec = p + table + 16u * i;
        uint32_t kind = rd_u32(rec), off = rd_u32(rec + 4);
        uint32_t soff = rd_u32(rec + 8), noc = rd_u32(rec + 12);
        size_t size = (size_t)rd_i32(p + 16 + 52 + 8 * i);
        if ((off & 3u) != 0) return -9;
        if (kind == 0) {
            if (off + size * 4u > blob_bytes) return -9;
            m->w[i] = (const float *)(const void *)(p + off);
            m->q8[i] = NULL;
            m->q8_scale[i] = NULL;
        } else if (kind == 1) {
            if (off + size > blob_bytes || (soff & 3u) != 0 ||
                soff + (size_t)noc * 4u > blob_bytes || noc == 0)
                return -9;
            m->w[i] = NULL;
            m->q8[i] = (const int8_t *)(const void *)(p + off);
            m->q8_scale[i] = (const float *)(const void *)(p + soff);
        } else {
            return -7;
        }
    }
    return 0;
}

/* ---- weights ---------------------------------------------------------- */

/* Element e of conv weight slot `ws` (a pointer from m->w[]). For int8 slots
 * the model keeps a parallel q8/scale entry; the scale index is the output
 * channel `oc`. */
typedef struct {
    const float *f;       /* fp32 weights, or NULL */
    const int8_t *q;      /* int8 weights, or NULL */
    const float *scale;   /* per-output-channel scales for q */
} pl_wref;

static pl_wref pl_slot(const snt_piperlite_model *m, int slot) {
    pl_wref r;
    r.f = m->w[slot];
    r.q = m->q8[slot];
    r.scale = m->q8_scale[slot];
    return r;
}

static float pl_wget(pl_wref w, long e, int oc) {
    return w.q ? (float)w.q[e] * w.scale[oc] : w.f[e];
}

/* ---- kernels ----------------------------------------------------------- */

static void pl_leaky_copy(float *dst, const float *src, long n, float slope) {
    long i;
    for (i = 0; i < n; i++) {
        float v = src[i];
        dst[i] = v > 0.0f ? v : slope * v;
    }
}

/* Conv1d / ConvTranspose1d kernels.
 *
 * Both run through one register-blocked micro-kernel: 4 output channels x
 * 16 time steps, weights repacked per call to [oc_block][ic][k][4] so one
 * vector load feeds 16 lane-FMAs. Inputs are zero-padded into scratch, so
 * the inner loop has no bounds checks. ConvTranspose1d uses the polyphase
 * form (K == 2*stride): each output phase is a K=2 conv over the input. */
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <stdlib.h>
#define PL_TB 16
#define PL_OB 4

/* acc[4][16] (+)= sum_ic sum_k wp[ic][k][0..3] * xp[ic][t0 + k*dil + 0..15] */
static void pl_tile(float *acc, const float *xp, long xs, const float *wp,
                    int in_ch, int K, int dil, int t0) {
    int ic, k;
#if defined(__aarch64__)
    float32x4_t a00 = vld1q_f32(acc + 0),  a01 = vld1q_f32(acc + 4);
    float32x4_t a02 = vld1q_f32(acc + 8),  a03 = vld1q_f32(acc + 12);
    float32x4_t a10 = vld1q_f32(acc + 16), a11 = vld1q_f32(acc + 20);
    float32x4_t a12 = vld1q_f32(acc + 24), a13 = vld1q_f32(acc + 28);
    float32x4_t a20 = vld1q_f32(acc + 32), a21 = vld1q_f32(acc + 36);
    float32x4_t a22 = vld1q_f32(acc + 40), a23 = vld1q_f32(acc + 44);
    float32x4_t a30 = vld1q_f32(acc + 48), a31 = vld1q_f32(acc + 52);
    float32x4_t a32 = vld1q_f32(acc + 56), a33 = vld1q_f32(acc + 60);
    for (ic = 0; ic < in_ch; ic++) {
        const float *xr = xp + (long)ic * xs + t0;
        for (k = 0; k < K; k++) {
            const float *xk = xr + k * dil;
            float32x4_t wv = vld1q_f32(wp);
            float32x4_t x0 = vld1q_f32(xk), x1 = vld1q_f32(xk + 4);
            float32x4_t x2 = vld1q_f32(xk + 8), x3 = vld1q_f32(xk + 12);
            wp += 4;
            a00 = vfmaq_laneq_f32(a00, x0, wv, 0);
            a01 = vfmaq_laneq_f32(a01, x1, wv, 0);
            a02 = vfmaq_laneq_f32(a02, x2, wv, 0);
            a03 = vfmaq_laneq_f32(a03, x3, wv, 0);
            a10 = vfmaq_laneq_f32(a10, x0, wv, 1);
            a11 = vfmaq_laneq_f32(a11, x1, wv, 1);
            a12 = vfmaq_laneq_f32(a12, x2, wv, 1);
            a13 = vfmaq_laneq_f32(a13, x3, wv, 1);
            a20 = vfmaq_laneq_f32(a20, x0, wv, 2);
            a21 = vfmaq_laneq_f32(a21, x1, wv, 2);
            a22 = vfmaq_laneq_f32(a22, x2, wv, 2);
            a23 = vfmaq_laneq_f32(a23, x3, wv, 2);
            a30 = vfmaq_laneq_f32(a30, x0, wv, 3);
            a31 = vfmaq_laneq_f32(a31, x1, wv, 3);
            a32 = vfmaq_laneq_f32(a32, x2, wv, 3);
            a33 = vfmaq_laneq_f32(a33, x3, wv, 3);
        }
    }
    vst1q_f32(acc + 0, a00);  vst1q_f32(acc + 4, a01);
    vst1q_f32(acc + 8, a02);  vst1q_f32(acc + 12, a03);
    vst1q_f32(acc + 16, a10); vst1q_f32(acc + 20, a11);
    vst1q_f32(acc + 24, a12); vst1q_f32(acc + 28, a13);
    vst1q_f32(acc + 32, a20); vst1q_f32(acc + 36, a21);
    vst1q_f32(acc + 40, a22); vst1q_f32(acc + 44, a23);
    vst1q_f32(acc + 48, a30); vst1q_f32(acc + 52, a31);
    vst1q_f32(acc + 56, a32); vst1q_f32(acc + 60, a33);
#else
    int o, t;
    for (ic = 0; ic < in_ch; ic++) {
        const float *xr = xp + (long)ic * xs + t0;
        for (k = 0; k < K; k++) {
            const float *xk = xr + k * dil;
            for (o = 0; o < PL_OB; o++)
                for (t = 0; t < PL_TB; t++)
                    acc[o * PL_TB + t] += wp[o] * xk[t];
            wp += 4;
        }
    }
#endif
}

/* Parallel-for hook (see snt_piperlite_set_parallel). Jobs are
 * independent: each writes a disjoint output tile. */
static snt_piperlite_parallel_fn g_par_fn;
static void *g_par_user;

void snt_piperlite_set_parallel(snt_piperlite_parallel_fn fn, void *user) {
    g_par_fn = fn;
    g_par_user = user;
}

static void pl_run(int n, void (*job)(void *, int), void *ctx) {
    int j;
    if (g_par_fn) {
        g_par_fn(n, job, ctx, g_par_user);
        return;
    }
    for (j = 0; j < n; j++) job(ctx, j);
}

struct pl_conv_ctx {
    float *out; const float *xp; long xs; const float *wp; const float *b;
    int in_ch, out_ch, T, K, dil, accumulate, ntb; long wblk;
};

static void pl_conv_job(void *ctx, int job) {
    const struct pl_conv_ctx *c = (const struct pl_conv_ctx *)ctx;
    int jb = job / c->ntb, t0 = (job % c->ntb) * PL_TB;
    int oc0 = jb * PL_OB;
    int noc = c->out_ch - oc0 < PL_OB ? c->out_ch - oc0 : PL_OB;
    int n = c->T - t0 < PL_TB ? c->T - t0 : PL_TB;
    float acc[PL_OB * PL_TB];
    int oo, t;
    for (oo = 0; oo < PL_OB; oo++)
        for (t = 0; t < PL_TB; t++)
            acc[oo * PL_TB + t] = oo < noc ? c->b[oc0 + oo] : 0.0f;
    pl_tile(acc, c->xp, c->xs, c->wp + jb * c->wblk, c->in_ch, c->K, c->dil,
            t0);
    for (oo = 0; oo < noc; oo++) {
        float *orow = c->out + (long)(oc0 + oo) * c->T + t0;
        const float *ar = acc + oo * PL_TB;
        if (c->accumulate)
            for (t = 0; t < n; t++) orow[t] += ar[t];
        else
            for (t = 0; t < n; t++) orow[t] = ar[t];
    }
}

struct pl_convtr_ctx {
    float *out; const float *xp; long xs; const float *wp; const float *b;
    int in_ch, out_ch, L, M, stride, pad, nob, ntb; long wblk;
};

static void pl_convtr_job(void *ctx, int job) {
    const struct pl_convtr_ctx *c = (const struct pl_convtr_ctx *)ctx;
    int ph = job / (c->nob * c->ntb), rem = job % (c->nob * c->ntb);
    int jb = rem / c->ntb, m0 = (rem % c->ntb) * PL_TB;
    int oc0 = jb * PL_OB;
    int noc = c->out_ch - oc0 < PL_OB ? c->out_ch - oc0 : PL_OB;
    float acc[PL_OB * PL_TB];
    int oo, t;
    for (oo = 0; oo < PL_OB; oo++)
        for (t = 0; t < PL_TB; t++)
            acc[oo * PL_TB + t] = oo < noc ? c->b[oc0 + oo] : 0.0f;
    pl_tile(acc, c->xp, c->xs, c->wp + ((long)ph * c->nob + jb) * c->wblk,
            c->in_ch, 2, 1, m0);
    for (oo = 0; oo < noc; oo++) {
        float *orow = c->out + (long)(oc0 + oo) * c->L;
        const float *ar = acc + oo * PL_TB;
        for (t = 0; t < PL_TB; t++) {
            long j = (long)(m0 + t) * c->stride + ph - c->pad;
            if (m0 + t < c->M && j >= 0 && j < c->L) orow[j] = ar[t];
        }
    }
}

/* Copy x [in_ch][T] into a zero-padded scratch [in_ch][xs] at column `lead`. */
static float *pl_pad_input(const float *x, int in_ch, int T, int lead,
                           long xs) {
    float *xp = (float *)malloc((size_t)in_ch * xs * sizeof(float));
    int ic;
    if (!xp) return NULL;
    for (ic = 0; ic < in_ch; ic++) {
        float *r = xp + (long)ic * xs;
        memset(r, 0, (size_t)lead * sizeof(float));
        memcpy(r + lead, x + (long)ic * T, (size_t)T * sizeof(float));
        memset(r + lead + T, 0, (size_t)(xs - lead - T) * sizeof(float));
    }
    return xp;
}

/* Conv1d, PyTorch "same" semantics: pad = dil*(K/2) zeros each side.
 * w layout [out_ch, in_ch, K]; x,out channel-major [ch][T].
 * accumulate!=0: out += bias + conv; else out = bias + conv. */
static void pl_conv1d(float *out, const float *x, pl_wref w,
                      const float *b, int in_ch, int out_ch, int T,
                      int K, int dil, int accumulate) {
    int pad = dil * (K / 2);
    int tpad = (T + PL_TB - 1) / PL_TB * PL_TB;
    long xs = tpad + 2L * pad;
    int nob = (out_ch + PL_OB - 1) / PL_OB, ntb = tpad / PL_TB;
    long wblk = (long)in_ch * K * PL_OB;
    float *xp = pl_pad_input(x, in_ch, T, pad, xs);
    float *wp = (float *)calloc((size_t)nob * wblk, sizeof(float));
    int ob, ic, k, o;
    if (!xp || !wp) { free(xp); free(wp); return; }
    for (ob = 0; ob < nob; ob++)
        for (ic = 0; ic < in_ch; ic++)
            for (k = 0; k < K; k++)
                for (o = 0; o < PL_OB; o++) {
                    int oc = ob * PL_OB + o;
                    if (oc < out_ch)
                        wp[ob * wblk + ((long)ic * K + k) * PL_OB + o] =
                            pl_wget(w, ((long)oc * in_ch + ic) * K + k, oc);
                }
    {
        struct pl_conv_ctx c;
        c.out = out; c.xp = xp; c.xs = xs; c.wp = wp; c.b = b;
        c.in_ch = in_ch; c.out_ch = out_ch; c.T = T; c.K = K; c.dil = dil;
        c.accumulate = accumulate; c.ntb = ntb; c.wblk = wblk;
        pl_run(nob * ntb, pl_conv_job, &c);
    }
    free(xp);
    free(wp);
}

/* ConvTranspose1d, w layout [in_ch, out_ch, K]; out length
 * (T-1)*stride - 2*pad + K. Requires K == 2*stride (true for every
 * piperlite stage). Output j = m*stride + r - pad gets
 * w[.,.,r]*x[m] + w[.,.,r+stride]*x[m-1]. */
static void pl_convtr1d(float *out, const float *x, pl_wref w,
                        const float *b, int in_ch, int out_ch, int T,
                        int K, int stride, int pad) {
    int L = (T - 1) * stride - 2 * pad + K;
    int M = T + 1;                       /* phase outputs m = 0..T */
    int mpad = (M + PL_TB - 1) / PL_TB * PL_TB;
    long xs = mpad + 1;                  /* one leading zero for x[m-1] */
    int nob = (out_ch + PL_OB - 1) / PL_OB, ntb = mpad / PL_TB;
    long wblk = (long)in_ch * 2 * PL_OB;
    float *xp = pl_pad_input(x, in_ch, T, 1, xs);
    float *wp = (float *)calloc((size_t)stride * nob * wblk, sizeof(float));
    int r, ob, ic, o;
    if (!xp || !wp || K != 2 * stride) { free(xp); free(wp); return; }
    /* wp[r][ob][ic][tap][o]: tap 0 -> x[m-1] (w[r+s]), tap 1 -> x[m] (w[r]) */
    for (r = 0; r < stride; r++)
        for (ob = 0; ob < nob; ob++)
            for (ic = 0; ic < in_ch; ic++)
                for (o = 0; o < PL_OB; o++) {
                    int oc = ob * PL_OB + o;
                    float *d = wp + ((long)r * nob + ob) * wblk +
                               (long)ic * 2 * PL_OB + o;
                    if (oc >= out_ch) continue;
                    d[0]     = pl_wget(w, ((long)ic * out_ch + oc) * K + r + stride,
                                       oc);
                    d[PL_OB] = pl_wget(w, ((long)ic * out_ch + oc) * K + r, oc);
                }
    {
        struct pl_convtr_ctx c;
        c.out = out; c.xp = xp; c.xs = xs; c.wp = wp; c.b = b;
        c.in_ch = in_ch; c.out_ch = out_ch; c.L = L; c.M = M;
        c.stride = stride; c.pad = pad; c.nob = nob; c.ntb = ntb; c.wblk = wblk;
        pl_run(stride * nob * ntb, pl_convtr_job, &c);
    }
    free(xp);
    free(wp);
}

/* PiperResidualBank: 3 branches (kernel, dil1, dil2) = (3,1,2), (5,2,6),
 * (7,3,12); each branch y1 = conv1(lrelu(x)) + x, y2 = conv2(lrelu(y1)) + y1;
 * bank out = (y2_0 + y2_1 + y2_2) / 3. `ws` points at the 12 bank slots.
 * Buffers sum/t/u are ch*T floats each; out may alias sum. */
static void pl_res_bank(float *out, const float *x,
                        const snt_piperlite_model *m, int slot0,
                        int ch, int T, float *sum, float *t, float *u) {
    static const int bk[3] = {3, 5, 7};
    static const int bd1[3] = {1, 2, 3};
    static const int bd2[3] = {2, 6, 12};
    long n = (long)ch * T;
    long i;
    int br;
    memset(sum, 0, (size_t)n * sizeof(float));
    for (br = 0; br < 3; br++) {
        pl_wref w1 = pl_slot(m, slot0 + br * 4 + 0);
        pl_wref w2 = pl_slot(m, slot0 + br * 4 + 2);
        const float *b1 = m->w[slot0 + br * 4 + 1];
        const float *b2 = m->w[slot0 + br * 4 + 3];
        pl_leaky_copy(t, x, n, 0.1f);
        pl_conv1d(u, t, w1, b1, ch, ch, T, bk[br], bd1[br], 0);
        for (i = 0; i < n; i++) u[i] += x[i];      /* y1 */
        pl_leaky_copy(t, u, n, 0.1f);
        pl_conv1d(sum, t, w2, b2, ch, ch, T, bk[br], bd2[br], 1); /* += conv */
        for (i = 0; i < n; i++) sum[i] += u[i];    /* += y1 */
    }
    for (i = 0; i < n; i++) out[i] = sum[i] * (1.0f / 3.0f);
}

/* ---- arena sizing ------------------------------------------------------ */

static long pl_lane_floats(const snt_piperlite_model *m, int frames) {
    long s = (long)m->c0 * frames;
    long v = (long)m->c1 * 8 * frames;
    if (v > s) s = v;
    v = (long)m->c2 * 64 * frames;
    if (v > s) s = v;
    v = (long)m->c3 * PL_HOP * frames;
    if (v > s) s = v;
    v = (long)(m->pf_channels > 0 ? m->pf_channels : 1) * PL_HOP * frames;
    if (v > s) s = v;
    return s;
}

size_t snt_piperlite_arena_floats(const snt_piperlite_model *m, int frames) {
    if (!m || frames <= 0) return 0;
    return (size_t)(4 * pl_lane_floats(m, frames));
}

/* ---- forward ------------------------------------------------------------ */

static void pl_tap(const snt_piperlite_model *m, const char *name,
                   const float *data, int ch, long len) {
    if (m->stage_cb) m->stage_cb(name, data, ch, (int)len, m->stage_user);
}

int snt_piperlite_synthesize(const snt_piperlite_model *m,
                             const float *z, int frames,
                             float *audio_out,
                             float *arena, size_t arena_floats) {
    long lane, n_samples;
    float *ln[4];
    float *x;
    int cur, s, i;
    long j;
    /* per-stage geometry: up kernel/stride/pad, weight slot bases */
    static const int up_k[3] = {16, 16, 8};
    static const int up_s[3] = {8, 8, 4};
    static const int up_p[3] = {4, 4, 2};
    static const char *up_name[3] = {"up0", "up1", "up2"};
    static const char *mix_name[3] = {"stage0_mix", "stage1_mix", "stage2_mix"};

    if (!m || !z || frames <= 0 || !audio_out || !arena) return -1;
    if (arena_floats < snt_piperlite_arena_floats(m, frames)) return -2;
    lane = pl_lane_floats(m, frames);
    for (i = 0; i < 4; i++) ln[i] = arena + (long)i * lane;
    n_samples = (long)frames * PL_HOP;

    /* pre: Conv1d(in_ch, c0, 7, pad 3) */
    pl_conv1d(ln[0], z, pl_slot(m, 0), m->w[1], m->in_ch, m->c0, frames, 7,
              1, 0);
    pl_tap(m, "pre", ln[0], m->c0, frames);
    x = ln[0];
    cur = 0;

    {
        int T = frames;
        int in_c[3], out_c[3];
        in_c[0] = m->c0; out_c[0] = m->c1;
        in_c[1] = m->c1; out_c[1] = m->c2;
        in_c[2] = m->c2; out_c[2] = m->c3;
        for (s = 0; s < 3; s++) {
            int base = 2 + s * 14;
            int nxt = (cur + 1) % 4;
            int b_sum = (cur + 2) % 4, b_t = (cur + 3) % 4, b_u = cur;
            int L = (T - 1) * up_s[s] - 2 * up_p[s] + up_k[s];
            /* act (slope 0.1) then upsample */
            pl_leaky_copy(x, x, (long)in_c[s] * T, 0.1f);
            pl_convtr1d(ln[nxt], x, pl_slot(m, base), m->w[base + 1],
                        in_c[s], out_c[s], T, up_k[s], up_s[s], up_p[s]);
            pl_tap(m, up_name[s], ln[nxt], out_c[s], L);
            /* residual bank; result lands in the sum lane */
            pl_res_bank(ln[b_sum], ln[nxt], m, base + 2, out_c[s], L,
                        ln[b_sum], ln[b_t], ln[b_u]);
            pl_tap(m, mix_name[s], ln[b_sum], out_c[s], L);
            x = ln[b_sum];
            cur = b_sum;
            T = L;
        }
        /* post: lrelu(0.01) -> Conv1d(c3, 1, 7, pad 3) -> tanh */
        pl_leaky_copy(x, x, (long)m->c3 * T, 0.01f);
        pl_conv1d(audio_out, x, pl_slot(m, 44), m->w[45], m->c3, 1, T, 7, 1,
                  0);
        pl_tap(m, "pre_tanh", audio_out, 1, T);
        for (j = 0; j < n_samples; j++) audio_out[j] = tanhf(audio_out[j]);
        pl_tap(m, "audio_pre_filter", audio_out, 1, T);
    }

    /* optional WaveformPostFilter:
     * r = in_conv(audio); r = ResidualUnit^layers(r); r2 = out_conv(r);
     * audio = tanh(audio + pf_scale * r2). ResidualUnit(dil=1+l):
     * r += scale * conv2(lrelu(conv1(lrelu(r)))). */
    if (m->pf_channels > 0) {
        int pf = m->pf_channels, K = m->pf_kernel;
        long n = (long)pf * n_samples;
        float *r = ln[0], *t = ln[1], *u = ln[2], *v = ln[3];
        int base = 46, l;
        pl_conv1d(r, audio_out, pl_slot(m, base), m->w[base + 1], 1, pf,
                  (int)n_samples, K, 1, 0);
        base += 2;
        for (l = 0; l < m->pf_layers; l++) {
            float scale = m->w[base][0];
            pl_leaky_copy(t, r, n, 0.1f);
            pl_conv1d(u, t, pl_slot(m, base + 1), m->w[base + 2], pf, pf,
                      (int)n_samples, 3, 1 + l, 0);
            pl_leaky_copy(t, u, n, 0.1f);
            pl_conv1d(v, t, pl_slot(m, base + 3), m->w[base + 4], pf, pf,
                      (int)n_samples, 3, 1, 0);
            for (j = 0; j < n; j++) r[j] += scale * v[j];
            base += 5;
        }
        pl_conv1d(t, r, pl_slot(m, base), m->w[base + 1], pf, 1,
                  (int)n_samples, K, 1, 0);
        for (j = 0; j < n_samples; j++)
            audio_out[j] = tanhf(audio_out[j] + m->pf_scale * t[j]);
    }
    pl_tap(m, "audio", audio_out, 1, n_samples);
    return 0;
}
