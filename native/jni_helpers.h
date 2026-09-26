// 知言的 JNI 小工具（从 satori-qq 的 satori.cpp 移出，里程碑 2 的 Java 层共用）。
#pragma once

#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <android/log.h>

#define WX_TAG "SatoriWx"
#define WLOGI(...) __android_log_print(ANDROID_LOG_INFO, WX_TAG, __VA_ARGS__)
#define WLOGE(...) __android_log_print(ANDROID_LOG_ERROR, WX_TAG, __VA_ARGS__)

/** 在当前线程拿 JNIEnv；native 线程要自己 attach。 */
inline JNIEnv *WxGetEnv(JavaVM *vm, bool *attached) {
    *attached = false;
    if (vm == nullptr) return nullptr;
    JNIEnv *env = nullptr;
    jint r = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (r == JNI_OK) return env;
    if (r == JNI_EDETACHED && vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        *attached = true;
        return env;
    }
    return nullptr;
}

inline void WxReleaseEnv(JavaVM *vm, bool attached) {
    if (attached && vm != nullptr) vm->DetachCurrentThread();
}

/** 关掉 hidden API 限制：Java 侧要反射宿主内部类时用。 */
inline void WxExemptHiddenApis(JNIEnv *env) {
    jclass vm_runtime = env->FindClass("dalvik/system/VMRuntime");
    if (vm_runtime == nullptr) { env->ExceptionClear(); return; }
    jmethodID get_runtime = env->GetStaticMethodID(vm_runtime, "getRuntime",
                                                   "()Ldalvik/system/VMRuntime;");
    jmethodID set_exemptions = env->GetMethodID(vm_runtime, "setHiddenApiExemptions",
                                                "([Ljava/lang/String;)V");
    if (get_runtime == nullptr || set_exemptions == nullptr) {
        env->ExceptionClear();
        return;
    }
    jobject runtime = env->CallStaticObjectMethod(vm_runtime, get_runtime);
    jclass string_cls = env->FindClass("java/lang/String");
    jstring all = env->NewStringUTF("L");  // 前缀 "L" 覆盖所有类
    jobjectArray arr = env->NewObjectArray(1, string_cls, all);
    env->CallVoidMethod(runtime, set_exemptions, arr);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        WLOGI("setHiddenApiExemptions rejected; continuing without it");
    }
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(all);
    env->DeleteLocalRef(runtime);
}
