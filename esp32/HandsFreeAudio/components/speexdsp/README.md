# SpeexDSP 1.2.1 (vendored)

The echo canceller and preprocessor from Xiph.Org's SpeexDSP, built as an ESP-IDF component for HandsFreeAudio's echo cancellation (`../../AEC-PLAN.md`). BSD licence, `COPYING`.

## Origin

- Release tarball `https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz`
- SHA-256 `8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d`, matching Xiph's `SHA256SUMS.txt`
- Vendored 2026-10-19

Only the files the echo canceller, the preprocessor and the KISS FFT need are kept, **unchanged**: the resampler, jitter buffer, `smallft` FFT, platform-specific headers (ARM, Blackfin, TI), tests and build system are left out. To check a file against the release, compare its SHA-256 with the table below.

| File | SHA-256 |
| --- | --- |
| `libspeexdsp/arch.h` | `102f6a14a95f8ae0bcfc69270f3a9e3fbba08b63bba688ca524f71e3faa48c23` |
| `libspeexdsp/fftwrap.c` | `a59ec583e5660207da9cf3c1ddd6a7ff1ed91382eb40df2a4e77d5cddfe14c52` |
| `libspeexdsp/fftwrap.h` | `fd289dcc47b9ddb2b33039c1990c4644bb279c9413ede6e74fdb2742c3888ee5` |
| `libspeexdsp/filterbank.c` | `da21ffc079092566fc3a7297611cc86b9d08a8f5a91ae81e3427f0b14ad98463` |
| `libspeexdsp/filterbank.h` | `8b86cc1f52c52abe5685c751e30c91b2e24a5a60d30893027445886c30d0d249` |
| `libspeexdsp/fixed_generic.h` | `18e7c5e6bc4b137fc3aefc05d75d18cd4d50a2841fd8224cefed1e1340170b05` |
| `libspeexdsp/kiss_fft.c` | `61574b6ceff29423a12cda1928b2ae09c89d316ffd1881a207aa74c3dc97da3f` |
| `libspeexdsp/_kiss_fft_guts.h` | `a375fe2c2006fa18605a9e75431a537101caf4084b614d33240068bb1f475c86` |
| `libspeexdsp/kiss_fft.h` | `61c31ff4a2002186086c8b7efa14cc28af733741175056abad95d985c2ecf3ae` |
| `libspeexdsp/kiss_fftr.c` | `13b7ec32c17b927e0978195ce9eeb09e9791b236fcee94b807fd0086d14cb25e` |
| `libspeexdsp/kiss_fftr.h` | `2f6ef60fa5900508d8aa0d303a27cae6c1f17a6620a4e174d51f0ff8adc18bf2` |
| `libspeexdsp/math_approx.h` | `a024693b0d29dc4e904021a1b442908c1fa726b18f5d31b0935e38646f2701ff` |
| `libspeexdsp/mdf.c` | `a096297873dfd0a07509028d2cdd02d12edf8f1d03319fcecedda003d18d22a8` |
| `libspeexdsp/os_support.h` | `5ab9c979e8ac1f72c7441d24e39e7840828dd9356aac49a31eabeddb808bf1d3` |
| `libspeexdsp/preprocess.c` | `f2579e284dc4ec49e2252901966fa4a93e62245969617e740affe8fda5051a7a` |
| `libspeexdsp/pseudofloat.h` | `0bd544dbed0096e02da2fd800d673bafb19bd97f56e268b98402ad641726daae` |
| `include/speex/speexdsp_types.h` | `4f97187c057fd8493c787f2526752859cf57007d50fef8d2e03c9b79b2fed490` |
| `include/speex/speex_echo.h` | `ab18db97564a241c9e06d723351c2155bf04a23ac30e0c27a3d86dcc04de3426` |
| `include/speex/speex_preprocess.h` | `87066ed01639bbc8d3297bb58dbd55bfe1b9cd6e26fc68a1b7f87aa9b61fa9c3` |
| `COPYING` | `2654a4264b2bfe298dedc508748d140111840c315cc8eb646a3a68c13fa75b01` |

## Local additions

| File | Why |
| --- | --- |
| `CMakeLists.txt` | IDF component: sources, `FIXED_POINT`, `USE_KISS_FFT`, `OS_SUPPORT_CUSTOM`, `EXPORT=`, third-party warnings silenced |
| `include/speex/speexdsp_config_types.h` | Replaces the file SpeexDSP's `configure` generates: `spx_int16_t` … from `stdint.h` |
| `libspeexdsp/os_support_custom.h` | Routes `speex_alloc` / `speex_free` to `speexdsp_alloc.c` |
| `include/speexdsp_alloc.h`, `speexdsp_alloc.c` | Swappable allocator (default `calloc` / `free`) with live-allocation, byte and failure counters |
| `README.md` | This file |

No `config.h` is used (`HAVE_CONFIG_H` stays undefined); every setting is a compile definition in `CMakeLists.txt`, repeated in `../../sim/Makefile` for the PC build.

## Use

```c
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"

SpeexEchoState *echoState = speex_echo_state_init(120, 1920);           // 7.5 ms frames, 120 ms tail at 16 kHz
int sampleRate = 16000;
speex_echo_ctl(echoState, SPEEX_ECHO_SET_SAMPLING_RATE, &sampleRate);
SpeexPreprocessState *preprocessState = speex_preprocess_state_init(120, 16000);
speex_preprocess_ctl(preprocessState, SPEEX_PREPROCESS_SET_ECHO_STATE, echoState);

speex_echo_cancellation(echoState, micFrame, speakerFrame, outputFrame);  // per 120-sample frame
speex_preprocess_run(preprocessState, outputFrame);
```

Create, run and destroy states from one task: the allocator counters are not thread-safe.
