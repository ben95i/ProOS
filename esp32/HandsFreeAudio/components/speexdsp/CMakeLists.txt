## SpeexDSP 1.2.1 (Xiph.Org, BSD licence, see COPYING): the echo canceller (mdf.c)
## and preprocessor (preprocess.c) only, built fixed point with the KISS FFT.
## Origin, checksums and local additions are listed in README.md.
idf_component_register(
    SRCS "libspeexdsp/mdf.c"
         "libspeexdsp/preprocess.c"
         "libspeexdsp/filterbank.c"
         "libspeexdsp/fftwrap.c"
         "libspeexdsp/kiss_fft.c"
         "libspeexdsp/kiss_fftr.c"
         "speexdsp_alloc.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "libspeexdsp"
)

# FIXED_POINT: 16-bit arrays, about half the RAM of the float build (AEC-PLAN.md).
# USE_KISS_FFT: the FFT backend that supports fixed point.
# OS_SUPPORT_CUSTOM: allocations go through speexdsp_alloc.c.
# EXPORT: SpeexDSP's symbol-visibility macro, unused in a static library.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FIXED_POINT
    USE_KISS_FFT
    OS_SUPPORT_CUSTOM
    EXPORT=
)

# Third-party code: keep its warnings out of the project's build output.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
