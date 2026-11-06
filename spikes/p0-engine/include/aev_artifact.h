#ifndef AEV_ARTIFACT_H
#define AEV_ARTIFACT_H

/* P0-PE-DLL-01 contract v3.  These values replace the earlier ABI 1
 * experiment values; artifacts carrying the old values are rejected. */
#define AEV_GUEST_ABI_VERSION 2ULL
#define AEV_GUEST_STATE_SIZE (37ULL * 8ULL)

#define AEV_ARTIFACT_ABI_VERSION 2u
#define AEV_TRANSLATOR_ABI_VERSION 2u
#define AEV_ARTIFACT_INFO_ABI_VERSION 2u

/* In-process trace sink ABI (GuestState.reserved0). */
#define AEV_TRACE_SINK_ABI_VERSION 1u
/* On-disk trace headers.  COFF schema 3 and PE schema 4 both carry the new
 * guest/callback/artifact/translator versions; old trace_abi=1 is rejected. */
#define AEV_TRACE_FILE_ABI_COFF 3u
#define AEV_TRACE_FILE_ABI_PE 4u

#define AEV_TRACE_SINK_SOURCE_AOT 0u
#define AEV_TRACE_SINK_SOURCE_JIT_COMPILE 1u
#define AEV_TRACE_SINK_SOURCE_JIT_EXEC 2u
#define AEV_AOT_TARGET_TRIPLE "arm64-apple-macosx14.0.0"

#define AEV_MODULE_KIND_COFF "coff"
#define AEV_MODULE_KIND_PE64 "pe32+amd64-dll"

#endif /* AEV_ARTIFACT_H */
