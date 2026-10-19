#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::fprintf(stderr, "expected exact fixture DLL path\n"); return 2; }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetEnvironmentVariableW(L"DEVBENCH_LOADER_FIXTURE_FAIL_PREPARE", nullptr);
    SetEnvironmentVariableW(L"DEVBENCH_LOADER_FIXTURE_PREPARE_FAILED", nullptr);
    for (int fail = 0; fail != 2; ++fail) {
        // The DLL's production Prepare runs under the REAL Windows loader lock.
        // Any accidental transport construction/join there hits the outer deadline.
        const auto dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) { std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 3; }
        const auto complete = reinterpret_cast<int (*)(int)>(GetProcAddress(dll, "CompleteStartup"));
        if (!complete) { FreeLibrary(dll); return 4; }
        const int result = complete(fail);
        if (result != 0) {
            std::fprintf(stderr, "startup mode=%d failed: %d\n", fail, result);
            // Failed cleanup must not unload code still used by an owned worker.
            return 5;
        }
        if (!FreeLibrary(dll)) return 6;
        std::printf("[PASS] real DLL loader preparation / safe-phase %s / owned drain\n",
                    fail ? "rollback" : "pristine startup");
    }
    // Exceptional preparation must also return through the real loader without
    // joining a worker. This is process-local fixture configuration, never global.
    SetEnvironmentVariableW(L"DEVBENCH_LOADER_FIXTURE_FAIL_PREPARE", L"1");
    const auto rejected = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    wchar_t failureProof[2]{};
    const bool exactFailure = GetEnvironmentVariableW(L"DEVBENCH_LOADER_FIXTURE_PREPARE_FAILED", failureProof, 2) == 1 && failureProof[0] == L'1';
    SetEnvironmentVariableW(L"DEVBENCH_LOADER_FIXTURE_FAIL_PREPARE", nullptr);
    SetEnvironmentVariableW(L"DEVBENCH_LOADER_FIXTURE_PREPARE_FAILED", nullptr);
    if (rejected) { FreeLibrary(rejected); return 7; }
    if (!exactFailure) return 8; // A missing DLL/unrelated exception cannot satisfy this case.
    std::printf("[PASS] real DLL loader preparation failure / thread-free cleanup\n");
    std::printf("3 Windows loader cases, 0 failed\n");
    return 0;
}
