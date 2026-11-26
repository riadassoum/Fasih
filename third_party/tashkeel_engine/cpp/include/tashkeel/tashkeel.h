#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  if defined(BARQ_EXPORTS) || defined(TASHKEEL_EXPORTS)
#    define BARQ_C_API __declspec(dllexport)
#  else
#    define BARQ_C_API __declspec(dllimport)
#  endif
#else
#  define BARQ_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

BARQ_C_API void* barq_create(const char* model_path);
BARQ_C_API void barq_destroy(void* handle);
BARQ_C_API char* barq_diacritize(void* handle, const char* utf8_text, size_t len, size_t* out_len);
BARQ_C_API int barq_diacritize_into(void* handle, const char* utf8_text, size_t len,
                                    char* out_buf, size_t out_cap, size_t* out_len);
BARQ_C_API void barq_free_str(char* str);
BARQ_C_API uint32_t barq_vocab_size(void* handle);
BARQ_C_API int32_t barq_get_weight(void* handle, uint32_t id);
BARQ_C_API int barq_set_weight(void* handle, uint32_t id, int32_t value);
BARQ_C_API void barq_set_restore_spelling(void* handle, int enabled);

BARQ_C_API void* tashkeel_create(const char* model_path);
BARQ_C_API void tashkeel_destroy(void* handle);
BARQ_C_API char* tashkeel_diacritize(void* handle, const char* utf8_text, size_t len, size_t* out_len);
BARQ_C_API int tashkeel_diacritize_into(void* handle, const char* utf8_text, size_t len,
                                        char* out_buf, size_t out_cap, size_t* out_len);
BARQ_C_API void tashkeel_free_str(char* str);
BARQ_C_API uint32_t tashkeel_vocab_size(void* handle);
BARQ_C_API int32_t tashkeel_get_weight(void* handle, uint32_t id);
BARQ_C_API int tashkeel_set_weight(void* handle, uint32_t id, int32_t value);
BARQ_C_API void tashkeel_set_restore_spelling(void* handle, int enabled);

#ifdef __cplusplus
}
#endif
