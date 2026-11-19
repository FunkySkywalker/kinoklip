# RNNoise (vendored, unchanged)

[xiph/rnnoise](https://github.com/xiph/rnnoise) tag v0.1.1, commit 6cbfd53eb348a8d394e0757b4025c6ded34eb2b6. Recurrent-neural-network
noise suppression for speech at 48 kHz (Jean-Marc Valin, Mozilla / Xiph.Org). Licence: BSD-3-Clause, see COPYING
(GPL-compatible). The trained weights are `src/rnn_data.c`, part of the same source release under the same licence.
Only the files needed to build the library are copied (no training code, no scripts); nothing is edited.
Built by CMake as the static library `kk_rnnoise`; used through `src/render/NoiseReduction.*`.
