/* rawi_host: runs the Rawi diacritizer (ONNX) in its own process, so NVDA never
 * loads numpy or onnxruntime. Protocol on stdin/stdout (binary, little-endian):
 *   request : uint32 n, then n x int64 character ids   (n == 0 -> ping)
 *   reply   : uint32 n, then n x int32 diacritic classes (0xFFFFFFFF -> error)
 * usage: rawi_host <onnxruntime library> <model.onnx>
 * Exits when stdin closes (i.e. when NVDA exits or kills it). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "onnxruntime_c_api.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
typedef wchar_t pchar;
#define LOADLIB(p) (void *)LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)
#define GETSYM(h, s) (void *)GetProcAddress((HMODULE)h, s)
#else
#include <dlfcn.h>
typedef char pchar;
#define LOADLIB(p) dlopen(p, RTLD_NOW)
#define GETSYM(h, s) dlsym(h, s)
#endif

static const OrtApi *api;

static int rd(void *buf, size_t n) { return fread(buf, 1, n, stdin) == n; }
static void wr(const void *buf, size_t n) { fwrite(buf, 1, n, stdout); }
static void fail_reply(void) { uint32_t e = 0xFFFFFFFFu; wr(&e, 4); fflush(stdout); }
static int ok(OrtStatus *s) {
    if (!s) return 1;
    fprintf(stderr, "rawi_host: %s\n", api->GetErrorMessage(s));
    api->ReleaseStatus(s);
    return 0;
}

static int run(const pchar *lib, const pchar *model) {
    void *h = LOADLIB(lib);
    if (!h) { fprintf(stderr, "rawi_host: cannot load onnxruntime\n"); return 2; }
    typedef const OrtApiBase *(ORT_API_CALL *getbase_t)(void);
    getbase_t getbase = (getbase_t)GETSYM(h, "OrtGetApiBase");
    if (!getbase) return 2;
    api = getbase()->GetApi(ORT_API_VERSION);
    if (!api) api = getbase()->GetApi(20);          /* older runtime: still fine for this model */
    if (!api) return 2;

    OrtEnv *env; OrtSessionOptions *so; OrtSession *sess; OrtMemoryInfo *mem;
    OrtAllocator *alloc;
    if (!ok(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "rawi", &env))) return 3;
    if (!ok(api->CreateSessionOptions(&so))) return 3;
    api->SetIntraOpNumThreads(so, 1);
    api->SetInterOpNumThreads(so, 1);
    api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
    if (!ok(api->CreateSession(env, model, so, &sess))) return 3;
    if (!ok(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem))) return 3;
    if (!ok(api->GetAllocatorWithDefaultOptions(&alloc))) return 3;
    char *in_name, *out_name;
    if (!ok(api->SessionGetInputName(sess, 0, alloc, &in_name))) return 3;
    if (!ok(api->SessionGetOutputName(sess, 0, alloc, &out_name))) return 3;
    const char *in_names[1] = {in_name}, *out_names[1] = {out_name};

    int64_t *ids = NULL; int32_t *res = NULL; size_t cap = 0;
    for (;;) {
        uint32_t n;
        if (!rd(&n, 4)) break;                         /* stdin closed: exit */
        if (n > 100000) return 4;
        if (n > cap) {
            cap = n * 2;
            ids = realloc(ids, cap * sizeof *ids);
            res = realloc(res, cap * sizeof *res);
            if (!ids || !res) return 5;
        }
        if (n && !rd(ids, n * sizeof *ids)) break;
        if (n == 0) { wr(&n, 4); fflush(stdout); continue; }
        int64_t shape[2] = {1, (int64_t)n};
        OrtValue *in = NULL, *out = NULL;
        if (!ok(api->CreateTensorWithDataAsOrtValue(mem, ids, n * sizeof *ids, shape, 2,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in))) { fail_reply(); continue; }
        if (!ok(api->Run(sess, NULL, in_names, (const OrtValue *const *)&in, 1, out_names, 1, &out))) {
            api->ReleaseValue(in); fail_reply(); continue;
        }
        OrtTensorTypeAndShapeInfo *ti; ONNXTensorElementDataType ty;
        size_t dims; int64_t sh[4] = {0};
        api->GetTensorTypeAndShape(out, &ti);
        api->GetTensorElementType(ti, &ty);
        api->GetDimensionsCount(ti, &dims);
        if (dims > 4) dims = 4;
        api->GetDimensions(ti, sh, dims);
        api->ReleaseTensorTypeAndShapeInfo(ti);
        void *p; api->GetTensorMutableData(out, &p);
        int good = 1;
        if (ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 && dims <= 2) {
            for (uint32_t i = 0; i < n; i++) res[i] = (int32_t)((int64_t *)p)[i];
        } else if (ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32 && dims <= 2) {
            memcpy(res, p, n * sizeof *res);
        } else if (ty == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && dims >= 2) {
            int64_t k = sh[dims - 1];                  /* logits: argmax over the last axis */
            for (uint32_t i = 0; i < n; i++) {
                const float *row = (const float *)p + (int64_t)i * k; int32_t b = 0;
                for (int64_t j = 1; j < k; j++) if (row[j] > row[b]) b = (int32_t)j;
                res[i] = b;
            }
        } else good = 0;
        api->ReleaseValue(out); api->ReleaseValue(in);
        if (!good) { fail_reply(); continue; }
        wr(&n, 4); wr(res, n * sizeof *res); fflush(stdout);
    }
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    if (argc < 3) return 1;
    return run(argv[1], argv[2]);
}
#else
int main(int argc, char **argv) {
    if (argc < 3) return 1;
    return run(argv[1], argv[2]);
}
#endif
