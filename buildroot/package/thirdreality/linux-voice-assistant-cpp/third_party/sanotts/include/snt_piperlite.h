/* snt_piperlite.h -- portable fp32 reference of the piperlite decoder
 * (DecoderStudent variant "piperlite" from
 * tools/train_roota_piper_decoder_student.py).
 *
 * Scope (matches every shipped piperlite checkpoint as of 2026-07):
 *   pre Conv1d(in,c0,k7) -> lrelu(0.1) -> ConvT(c0,c1,16/8/4) -> PiperResBank
 *   -> lrelu(0.1) -> ConvT(c1,c2,16/8/4) -> PiperResBank -> lrelu(0.1)
 *   -> ConvT(c2,c3,8/4/2) -> PiperResBank -> lrelu(0.01) -> Conv1d(c3,1,k7)
 *   -> tanh -> optional WaveformPostFilter.
 * Not implemented (exporter rejects such checkpoints): stage affines,
 * projections, rank-factorized convs, partial branch sets, snake activation,
 * pre-tanh repair.
 *
 * Weights/meta come from tools/export_piperlite_golden.py: weights_f32.bin
 * (fixed tensor order, documented in the exporter) plus meta.bin (dims +
 * per-slot offset table), so this code hardcodes no shapes.
 */
#ifndef SNT_PIPERLITE_H
#define SNT_PIPERLITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNT_PIPERLITE_MAGIC 0x534E504CL /* 'SNPL' */
#define SNT_PIPERLITE_HOP 256           /* audio samples per latent frame */
#define SNT_PIPERLITE_MAX_TENSORS 64

typedef struct {
    /* dims (from meta.bin) */
    int in_ch, c0, c1, c2, c3;
    int pf_channels, pf_layers, pf_kernel;
    float pf_scale;
    int n_tensors;
    /* per-slot weight pointers, in exporter slot order. With an int8
     * container a conv slot has w[i] == NULL and q8[i]/q8_scale[i] set. */
    const float *w[SNT_PIPERLITE_MAX_TENSORS];
    const int8_t *q8[SNT_PIPERLITE_MAX_TENSORS];
    const float *q8_scale[SNT_PIPERLITE_MAX_TENSORS];
    /* optional bring-up tap: called after each stage with the fp32 tensor
     * ("pre", "up0", "stage0_mix", ..., "pre_tanh", "audio_pre_filter").
     * NULL in production. */
    void (*stage_cb)(const char *name, const float *data, int ch, int len,
                     void *user);
    void *stage_user;
} snt_piperlite_model;

/* Parse meta.bin + bind weight pointers. weights must stay alive for the
 * model's lifetime (flash ok). Returns 0 on success, negative on a malformed
 * or out-of-range meta blob. */
int snt_piperlite_init(snt_piperlite_model *m,
                       const void *meta, size_t meta_bytes,
                       const float *weights, size_t weight_floats);

/* Bind an LVA int8 decoder container (tools/make_voice_files.py): conv
 * weights int8 per output channel, the rest fp32. blob must stay alive and
 * be 16-byte aligned (mmap is). Same return codes as snt_piperlite_init. */
int snt_piperlite_init_q8(snt_piperlite_model *m, const void *blob,
                          size_t blob_bytes);

/* Working-memory floats needed by snt_piperlite_synthesize for `frames`
 * latent frames (caller-owned arena, no malloc inside). */
size_t snt_piperlite_arena_floats(const snt_piperlite_model *m, int frames);

/* Run the decoder: z is [in_ch, frames] (channel-major, contiguous rows),
 * audio_out receives frames*SNT_PIPERLITE_HOP samples. Returns 0 on success,
 * negative on bad args / undersized arena. */
int snt_piperlite_synthesize(const snt_piperlite_model *m,
                             const float *z, int frames,
                             float *audio_out,
                             float *arena, size_t arena_floats);

/* Optional parallel-for used by the conv kernels: fn must call
 * job(ctx, j) once for every j in [0, n) and return when all are done.
 * NULL (default) runs serially. Process-global; set before synthesizing. */
typedef void (*snt_piperlite_parallel_fn)(int n, void (*job)(void *ctx, int j),
                                          void *ctx, void *user);
void snt_piperlite_set_parallel(snt_piperlite_parallel_fn fn, void *user);

#ifdef __cplusplus
}
#endif
#endif
