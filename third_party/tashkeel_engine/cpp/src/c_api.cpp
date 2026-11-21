#include "tashkeel/diacritizer.hpp"
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#define TASHKEEL_API __declspec(dllexport)
#else
#define TASHKEEL_API __attribute__((visibility("default")))
#endif

extern "C" {

TASHKEEL_API void* tashkeel_create(const char* model_path) {
    if (!model_path) return nullptr;
    auto* engine = new tashkeel::Diacritizer();
    if (!engine->load(model_path)) {
        delete engine;
        return nullptr;
    }
    return static_cast<void*>(engine);
}

TASHKEEL_API void tashkeel_destroy(void* handle) {
    delete static_cast<tashkeel::Diacritizer*>(handle);
}

TASHKEEL_API char* tashkeel_diacritize(void* handle, const char* utf8_text, size_t len, size_t* out_len) {
    if (!handle || !utf8_text) return nullptr;
    auto* engine = static_cast<tashkeel::Diacritizer*>(handle);
    std::string res = engine->diacritize(utf8_text, len);
    char* buf = static_cast<char*>(std::malloc(res.size() + 1));
    if (!buf) return nullptr;
    std::memcpy(buf, res.data(), res.size());
    buf[res.size()] = '\0';
    if (out_len) *out_len = res.size();
    return buf;
}

TASHKEEL_API void tashkeel_free_str(char* str) {
    std::free(str);
}

TASHKEEL_API uint32_t tashkeel_vocab_size(void* handle) {
    if (!handle) return 0;
    return static_cast<tashkeel::Diacritizer*>(handle)->vocab_size();
}

TASHKEEL_API int32_t tashkeel_get_weight(void* handle, uint32_t id) {
    if (!handle) return 0;
    return static_cast<tashkeel::Diacritizer*>(handle)->weight(id);
}

TASHKEEL_API int tashkeel_set_weight(void* handle, uint32_t id, int32_t value) {
    if (!handle) return 0;
    return static_cast<tashkeel::Diacritizer*>(handle)->set_weight(id, value) ? 1 : 0;
}

TASHKEEL_API void tashkeel_set_restore_spelling(void* handle, int enabled) {
    if (!handle) return;
    static_cast<tashkeel::Diacritizer*>(handle)->set_restore_spelling(enabled != 0);
}

}
