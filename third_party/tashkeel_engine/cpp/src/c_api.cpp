#include "tashkeel/diacritizer.hpp"
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#define BARQ_API __declspec(dllexport)
#else
#define BARQ_API __attribute__((visibility("default")))
#endif

extern "C" {

BARQ_API void* barq_create(const char* model_path) {
    if (!model_path) return nullptr;
    auto* engine = new tashkeel::Diacritizer();
    if (!engine->load(model_path)) {
        delete engine;
        return nullptr;
    }
    return static_cast<void*>(engine);
}

BARQ_API void barq_destroy(void* handle) {
    delete static_cast<tashkeel::Diacritizer*>(handle);
}

BARQ_API char* barq_diacritize(void* handle, const char* utf8_text, size_t len, size_t* out_len) {
    if (!handle || !utf8_text) return nullptr;
    auto* engine = static_cast<tashkeel::Diacritizer*>(handle);
    thread_local std::string tls_out;
    tls_out = engine->diacritize(utf8_text, len);
    char* buf = static_cast<char*>(std::malloc(tls_out.size() + 1));
    if (!buf) return nullptr;
    if (!tls_out.empty()) std::memcpy(buf, tls_out.data(), tls_out.size());
    buf[tls_out.size()] = '\0';
    if (out_len) *out_len = tls_out.size();
    return buf;
}

BARQ_API int barq_diacritize_into(void* handle, const char* utf8_text, size_t len,
                                  char* out_buf, size_t out_cap, size_t* out_len) {
    if (!handle || (!utf8_text && len > 0)) {
        if (out_len) *out_len = 0;
        return 0;
    }
    auto* engine = static_cast<tashkeel::Diacritizer*>(handle);
    thread_local std::string tls_out;
    tls_out = engine->diacritize(utf8_text ? utf8_text : "", len);
    if (out_len) *out_len = tls_out.size();
    if (!out_buf || tls_out.size() + 1 > out_cap) {
        return 0;
    }
    if (!tls_out.empty()) {
        std::memcpy(out_buf, tls_out.data(), tls_out.size());
    }
    out_buf[tls_out.size()] = '\0';
    return 1;
}

BARQ_API void barq_free_str(char* str) {
    std::free(str);
}

BARQ_API uint32_t barq_vocab_size(void* handle) {
    if (!handle) return 0;
    return static_cast<tashkeel::Diacritizer*>(handle)->vocab_size();
}

BARQ_API int32_t barq_get_weight(void* handle, uint32_t id) {
    if (!handle) return 0;
    return static_cast<tashkeel::Diacritizer*>(handle)->weight(id);
}

BARQ_API int barq_set_weight(void* handle, uint32_t id, int32_t value) {
    if (!handle) return 0;
    return static_cast<tashkeel::Diacritizer*>(handle)->set_weight(id, value) ? 1 : 0;
}

BARQ_API void barq_set_restore_spelling(void* handle, int enabled) {
    if (!handle) return;
    static_cast<tashkeel::Diacritizer*>(handle)->set_restore_spelling(enabled != 0);
}

BARQ_API void* tashkeel_create(const char* model_path) { return barq_create(model_path); }
BARQ_API void tashkeel_destroy(void* handle) { barq_destroy(handle); }
BARQ_API char* tashkeel_diacritize(void* handle, const char* utf8_text, size_t len, size_t* out_len) {
    return barq_diacritize(handle, utf8_text, len, out_len);
}
BARQ_API int tashkeel_diacritize_into(void* handle, const char* utf8_text, size_t len,
                                       char* out_buf, size_t out_cap, size_t* out_len) {
    return barq_diacritize_into(handle, utf8_text, len, out_buf, out_cap, out_len);
}
BARQ_API void tashkeel_free_str(char* str) { barq_free_str(str); }
BARQ_API uint32_t tashkeel_vocab_size(void* handle) { return barq_vocab_size(handle); }
BARQ_API int32_t tashkeel_get_weight(void* handle, uint32_t id) { return barq_get_weight(handle, id); }
BARQ_API int tashkeel_set_weight(void* handle, uint32_t id, int32_t value) { return barq_set_weight(handle, id, value); }
BARQ_API void tashkeel_set_restore_spelling(void* handle, int enabled) { barq_set_restore_spelling(handle, enabled); }

}
