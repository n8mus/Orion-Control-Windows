/* SPDX-License-Identifier: GPL-3.0-or-later
 * The two Opus internals RADE speech needs, behind an opaque C face so
 * the console's C++ never includes Opus's dnn/ headers:
 *   FARGAN  - the vocoder: 36 features -> 160 samples @ 16 kHz (10 ms)
 *   LPCNet  - feature extraction for transmit: 160 samples -> 36 features
 * Same calls as freedv-backend's RADEReceiveStep / RADETransmitStep. */
#ifndef TTC_RADE_SHIM_H
#define TTC_RADE_SHIM_H
#ifdef __cplusplus
extern "C" {
#endif

enum { TTC_RADE_FRAME = 160, TTC_RADE_NFEAT = 36 };

void* ttc_fargan_new(void);                    /* init + zero warm-up */
void  ttc_fargan_free(void* st);
void  ttc_fargan_synth(void* st, float pcm[TTC_RADE_FRAME], const float feat[TTC_RADE_NFEAT]);

void* ttc_lpcnet_enc_new(void);
void  ttc_lpcnet_enc_free(void* st);
void  ttc_lpcnet_features(void* st, const short pcm[TTC_RADE_FRAME], float feat[TTC_RADE_NFEAT]);

#ifdef __cplusplus
}
#endif
#endif
