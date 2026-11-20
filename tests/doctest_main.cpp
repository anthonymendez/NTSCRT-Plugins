#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdlib>

// Tests never touch the user's own shader cache (%LOCALAPPDATA%\NTSCRT\cache,
// ~/Library/Caches/NTSCRT): unless the environment says otherwise, compiled CRT
// shaders go to the build's test-shader-cache folder (core/stages/crt/shader_cache.cpp).
// Set before anything runs, also for the plug-in test_ofx loads (it reads the
// process environment when it loads).
int main(int argc, char **argv) {
#ifdef NTSCRT_TEST_SHADER_CACHE_DIR
    const char *e = std::getenv("NTSCRT_SHADER_CACHE");
    if (!e || !*e) {
#ifdef _WIN32
        _putenv_s("NTSCRT_SHADER_CACHE", NTSCRT_TEST_SHADER_CACHE_DIR);
#else
        setenv("NTSCRT_SHADER_CACHE", NTSCRT_TEST_SHADER_CACHE_DIR, 1);
#endif
    }
#endif
    return doctest::Context(argc, argv).run();
}
