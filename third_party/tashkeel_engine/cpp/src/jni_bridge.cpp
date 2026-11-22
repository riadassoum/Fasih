#include <jni.h>
#include "tashkeel/diacritizer.hpp"
#include <string>

#define BARQ_JNI_EXPORT JNIEXPORT __attribute__((visibility("default")))

namespace {

jlong jni_create_impl(JNIEnv* env, jstring modelPath) {
    if (!modelPath) return 0;
    const char* path = env->GetStringUTFChars(modelPath, nullptr);
    auto* engine = new tashkeel::Diacritizer();
    bool ok = engine->load(path);
    env->ReleaseStringUTFChars(modelPath, path);
    if (!ok) {
        delete engine;
        return 0;
    }
    return reinterpret_cast<jlong>(engine);
}

void jni_destroy_impl(jlong handle) {
    delete reinterpret_cast<tashkeel::Diacritizer*>(handle);
}

jstring jni_diacritize_impl(JNIEnv* env, jlong handle, jstring text) {
    if (!handle || !text) return text;
    auto* engine = reinterpret_cast<tashkeel::Diacritizer*>(handle);
    const char* utf8 = env->GetStringUTFChars(text, nullptr);
    jsize len = env->GetStringUTFLength(text);
    std::string out = engine->diacritize(utf8, static_cast<size_t>(len));
    env->ReleaseStringUTFChars(text, utf8);
    return env->NewStringUTF(out.c_str());
}

jint jni_vocab_size_impl(jlong handle) {
    if (!handle) return 0;
    return static_cast<jint>(reinterpret_cast<tashkeel::Diacritizer*>(handle)->vocab_size());
}

jboolean jni_set_weight_impl(jlong handle, jint id, jint value) {
    if (!handle || id < 0) return JNI_FALSE;
    return reinterpret_cast<tashkeel::Diacritizer*>(handle)->set_weight(
               static_cast<uint32_t>(id), static_cast<int32_t>(value))
               ? JNI_TRUE
               : JNI_FALSE;
}

void jni_set_restore_spelling_impl(jlong handle, jboolean enabled) {
    if (!handle) return;
    reinterpret_cast<tashkeel::Diacritizer*>(handle)->set_restore_spelling(enabled == JNI_TRUE);
}

} // namespace

extern "C" {

/* Primary Barq Android JNI bindings (com.barq.BarqEngine) */
BARQ_JNI_EXPORT jlong JNICALL
Java_com_barq_BarqEngine_nativeCreate(JNIEnv* env, jclass, jstring modelPath) {
    return jni_create_impl(env, modelPath);
}

BARQ_JNI_EXPORT void JNICALL
Java_com_barq_BarqEngine_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    jni_destroy_impl(handle);
}

BARQ_JNI_EXPORT jstring JNICALL
Java_com_barq_BarqEngine_nativeDiacritize(JNIEnv* env, jclass, jlong handle, jstring text) {
    return jni_diacritize_impl(env, handle, text);
}

BARQ_JNI_EXPORT jint JNICALL
Java_com_barq_BarqEngine_nativeVocabSize(JNIEnv*, jclass, jlong handle) {
    return jni_vocab_size_impl(handle);
}

BARQ_JNI_EXPORT jboolean JNICALL
Java_com_barq_BarqEngine_nativeSetWeight(JNIEnv*, jclass, jlong handle, jint id, jint value) {
    return jni_set_weight_impl(handle, id, value);
}

BARQ_JNI_EXPORT void JNICALL
Java_com_barq_BarqEngine_nativeSetRestoreSpelling(JNIEnv*, jclass, jlong handle, jboolean enabled) {
    jni_set_restore_spelling_impl(handle, enabled);
}

/* Legacy ABI-compatible Android JNI bindings (com.tashkeel.TashkeelEngine) */
BARQ_JNI_EXPORT jlong JNICALL
Java_com_tashkeel_TashkeelEngine_nativeCreate(JNIEnv* env, jclass, jstring modelPath) {
    return jni_create_impl(env, modelPath);
}

BARQ_JNI_EXPORT void JNICALL
Java_com_tashkeel_TashkeelEngine_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    jni_destroy_impl(handle);
}

BARQ_JNI_EXPORT jstring JNICALL
Java_com_tashkeel_TashkeelEngine_nativeDiacritize(JNIEnv* env, jclass, jlong handle, jstring text) {
    return jni_diacritize_impl(env, handle, text);
}

BARQ_JNI_EXPORT jint JNICALL
Java_com_tashkeel_TashkeelEngine_nativeVocabSize(JNIEnv*, jclass, jlong handle) {
    return jni_vocab_size_impl(handle);
}

BARQ_JNI_EXPORT jboolean JNICALL
Java_com_tashkeel_TashkeelEngine_nativeSetWeight(JNIEnv*, jclass, jlong handle, jint id, jint value) {
    return jni_set_weight_impl(handle, id, value);
}

BARQ_JNI_EXPORT void JNICALL
Java_com_tashkeel_TashkeelEngine_nativeSetRestoreSpelling(JNIEnv*, jclass, jlong handle, jboolean enabled) {
    jni_set_restore_spelling_impl(handle, enabled);
}

}
