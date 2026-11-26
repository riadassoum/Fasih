#include <jni.h>
#include "tashkeel/diacritizer.hpp"
#include <string>

#ifndef BARQ_JNI_EXPORT
#define BARQ_JNI_EXPORT extern "C" __attribute__((visibility("default"))) JNIEXPORT
#endif

extern "C" {

BARQ_JNI_EXPORT jlong JNICALL
Java_com_planetblindtech_barq_barq_nativeCreate(JNIEnv* env, jclass, jstring modelPath) {
    if (!modelPath) return 0;
    const char* path = env->GetStringUTFChars(modelPath, nullptr);
    if (!path) return 0;
    auto* engine = new tashkeel::Diacritizer();
    bool ok = engine->load(path);
    env->ReleaseStringUTFChars(modelPath, path);
    if (!ok) {
        delete engine;
        return 0;
    }
    return reinterpret_cast<jlong>(engine);
}

BARQ_JNI_EXPORT void JNICALL
Java_com_planetblindtech_barq_barq_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    delete reinterpret_cast<tashkeel::Diacritizer*>(handle);
}

BARQ_JNI_EXPORT jstring JNICALL
Java_com_planetblindtech_barq_barq_nativeDiacritize(JNIEnv* env, jclass, jlong handle, jstring text) {
    if (!handle || !text) return text;
    auto* engine = reinterpret_cast<tashkeel::Diacritizer*>(handle);
    const char* utf8 = env->GetStringUTFChars(text, nullptr);
    if (!utf8) return nullptr;
    jsize len = env->GetStringUTFLength(text);
    std::string out = engine->diacritize(utf8, static_cast<size_t>(len));
    env->ReleaseStringUTFChars(text, utf8);
    return env->NewStringUTF(out.c_str());
}

BARQ_JNI_EXPORT jobjectArray JNICALL
Java_com_planetblindtech_barq_barq_nativeDiacritizeBatch(JNIEnv* env, jclass, jlong handle, jobjectArray texts) {
    if (!handle || !texts) return nullptr;
    jclass str_cls = env->FindClass("java/lang/String");
    if (!str_cls) return nullptr;
    jsize n = env->GetArrayLength(texts);
    jobjectArray out_arr = env->NewObjectArray(n, str_cls, nullptr);
    if (!out_arr) {
        env->DeleteLocalRef(str_cls);
        return nullptr;
    }
    auto* engine = reinterpret_cast<tashkeel::Diacritizer*>(handle);
    for (jsize i = 0; i < n; ++i) {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(texts, i));
        if (!item) {
            continue;
        }
        jsize ulen = env->GetStringUTFLength(item);
        if (ulen == 0) {
            env->SetObjectArrayElement(out_arr, i, item);
            env->DeleteLocalRef(item);
            continue;
        }
        const char* utf8 = env->GetStringUTFChars(item, nullptr);
        if (!utf8) {
            env->DeleteLocalRef(item);
            env->DeleteLocalRef(str_cls);
            return nullptr;
        }
        std::string out = engine->diacritize(utf8, static_cast<size_t>(ulen));
        env->ReleaseStringUTFChars(item, utf8);
        jstring res = env->NewStringUTF(out.c_str());
        if (!res) {
            env->DeleteLocalRef(item);
            env->DeleteLocalRef(str_cls);
            return nullptr;
        }
        env->SetObjectArrayElement(out_arr, i, res);
        env->DeleteLocalRef(res);
        env->DeleteLocalRef(item);
    }
    env->DeleteLocalRef(str_cls);
    return out_arr;
}

BARQ_JNI_EXPORT jint JNICALL
Java_com_planetblindtech_barq_barq_nativeVocabSize(JNIEnv*, jclass, jlong handle) {
    if (!handle) return 0;
    return static_cast<jint>(reinterpret_cast<tashkeel::Diacritizer*>(handle)->vocab_size());
}

BARQ_JNI_EXPORT jboolean JNICALL
Java_com_planetblindtech_barq_barq_nativeHasNeuralIrab(JNIEnv*, jclass, jlong handle) {
    if (!handle) return JNI_FALSE;
    return reinterpret_cast<tashkeel::Diacritizer*>(handle)->has_v9_neural() ? JNI_TRUE : JNI_FALSE;
}

BARQ_JNI_EXPORT jint JNICALL
Java_com_planetblindtech_barq_barq_nativeGetWeight(JNIEnv*, jclass, jlong handle, jint id) {
    if (!handle || id < 0) return 0;
    return static_cast<jint>(
        reinterpret_cast<tashkeel::Diacritizer*>(handle)->weight(static_cast<uint32_t>(id)));
}

BARQ_JNI_EXPORT jboolean JNICALL
Java_com_planetblindtech_barq_barq_nativeSetWeight(JNIEnv*, jclass, jlong handle, jint id, jint value) {
    if (!handle || id < 0) return JNI_FALSE;
    return reinterpret_cast<tashkeel::Diacritizer*>(handle)->set_weight(
               static_cast<uint32_t>(id), static_cast<int32_t>(value))
               ? JNI_TRUE
               : JNI_FALSE;
}

BARQ_JNI_EXPORT void JNICALL
Java_com_planetblindtech_barq_barq_nativeSetRestoreSpelling(JNIEnv*, jclass, jlong handle, jboolean enabled) {
    if (!handle) return;
    reinterpret_cast<tashkeel::Diacritizer*>(handle)->set_restore_spelling(enabled == JNI_TRUE);
}

}
