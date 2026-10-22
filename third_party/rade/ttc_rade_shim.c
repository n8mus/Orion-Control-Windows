/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ttc_rade_shim.h"

#include <stdlib.h>
#include <string.h>

#include "cpu_support.h"
#include "fargan.h"
#include "lpcnet.h"

void* ttc_fargan_new(void) {
    FARGANState* st = (FARGANState*)calloc(1, sizeof(FARGANState));
    if (!st) return NULL;
    /* freedv's start-up: init, then continue from 320 samples of silence
       with 5 frames of zero features so the first real frame is clean. */
    float zeros[320];
    float feat[5 * NB_TOTAL_FEATURES];
    memset(zeros, 0, sizeof zeros);
    memset(feat, 0, sizeof feat);
    fargan_init(st);
    fargan_cont(st, zeros, feat);
    return st;
}

void ttc_fargan_free(void* st) { free(st); }

void ttc_fargan_synth(void* st, float pcm[TTC_RADE_FRAME], const float feat[TTC_RADE_NFEAT]) {
    fargan_synthesize((FARGANState*)st, pcm, feat);
}

void* ttc_lpcnet_enc_new(void) { return lpcnet_encoder_create(); }
void  ttc_lpcnet_enc_free(void* st) { lpcnet_encoder_destroy((LPCNetEncState*)st); }

void ttc_lpcnet_features(void* st, const short pcm[TTC_RADE_FRAME], float feat[TTC_RADE_NFEAT]) {
    /* opus_select_arch(): Opus's runtime SIMD pick (AVX-only CPUs get the
       SSE4.1 kernels, never AVX2) — freedv-backend's choice over arch 0. */
    lpcnet_compute_single_frame_features((LPCNetEncState*)st, pcm, feat, opus_select_arch());
}
