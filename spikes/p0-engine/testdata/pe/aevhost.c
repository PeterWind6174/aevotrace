/* P0-PE-DLL-01 fixture helper: no CRT, Microsoft x64 C ABI.
 * AevUpdate is the single declared HostImport used by aevguest.dll. */
static unsigned int g_helper_calls = 0;

__declspec(dllexport) int AevUpdate(int a, int b) {
    g_helper_calls++;
    return (int)((unsigned int)a * 3u + (unsigned int)b + 1u);
}

__declspec(dllexport) unsigned int AevHelperCalls(void) {
    return g_helper_calls;
}
