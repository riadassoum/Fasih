#include <jni.h>
#include "tashkeel/diacritizer.hpp"
#include <string>

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_tashkeel_TashkeelEngine_nativeCreate(JNIEnv* env, jclass, jstring modelPath) {
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

JNIEXPORT void JNICALL
Java_com_tashkeel_TashkeelEngine_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    delete reinterpret_cast<tashkeel::Diacritizer*>(handle);
}

JNIEXPORT jstring JNICALL
Java_com_tashkeel_TashkeelEngine_nativeDiacritize(JNIEnv* env, jclass, jlong handle, jstring text) {
    if (!handle || !text) return text;
    auto* engine = reinterpret_cast<tashkeel::Diacritizer*>(handle);
    const char* utf8 = env->GetStringUTFChars(text, nullptr);
    jsize len = env->GetStringUTFLength(text);
    std::string out = engine->diacritize(utf8, static_cast<size_t>(len));
    env->ReleaseStringUTFChars(text, utf8);
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jint JNICALL
Java_com_tashkeel_TashkeelEngine_nativeVocabSize(JNIEnv*, jclass, jlong handle) {
    if (!handle) return 0;
    return static_cast<jint>(reinterpret_cast<tashkeel::Diacritizer*>(handle)->vocab_size());
}

JNIEXPORT jboolean JNICALL
Java_com_tashkeel_TashkeelEngine_nativeSetWeight(JNIEnv*, jclass, jlong handle, jint id, jint value) {
    if (!handle || id < 0) return JNI_FALSE;
    return reinterpret_cast<tashkeel::Diacritizer*>(handle)->set_weight(
               static_cast<uint32_t>(id), static_cast<int32_t>(value))
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_tashkeel_TashkeelEngine_nativeSetRestoreSpelling(JNIEnv*, jclass, jlong handle, jboolean enabled) {
    if (!handle) return;
    reinterpret_cast<tashkeel::Diacritizer*>(handle)->set_restore_spelling(enabled == JNI_TRUE);
}

}
