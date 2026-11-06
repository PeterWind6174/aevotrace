#ifndef AEV_ARTIFACT_H
#define AEV_ARTIFACT_H

/* Trace/AOT experiment v1 constants.  Callback ABI v2 remains unchanged. */
#define AEV_ARTIFACT_ABI_VERSION 1u
#define AEV_TRANSLATOR_ABI_VERSION 1u
#define AEV_ARTIFACT_INFO_ABI_VERSION 1u
#define AEV_TRACE_ABI_VERSION 1u
#define AEV_TRACE_SINK_SOURCE_AOT 0u
#define AEV_TRACE_SINK_SOURCE_JIT_COMPILE 1u
#define AEV_TRACE_SINK_SOURCE_JIT_EXEC 2u
#define AEV_AOT_TARGET_TRIPLE "arm64-apple-macosx14.0.0"

#endif /* AEV_ARTIFACT_H */
