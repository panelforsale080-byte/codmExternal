#include "overlay_jni.hpp"
#include "clip_android.hpp"
#include "ui_core.hpp"
#include <jni.h>

static JavaVM *g_vm = nullptr;
static jclass g_cls = nullptr;
static jmethodID g_menuTouch = nullptr;
static jmethodID g_toast = nullptr;

void jni_set_menu_touch(bool on) {
    if (!g_vm || !g_cls || !g_menuTouch) return;
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    env->CallStaticVoidMethod(g_cls, g_menuTouch, on ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void jni_toast(const char *msg) {
    if (!g_vm || !g_cls || !g_toast || !msg) return;
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    jstring j = env->NewStringUTF(msg);
    env->CallStaticVoidMethod(g_cls, g_toast, j);
    env->DeleteLocalRef(j);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeInit(JNIEnv *env, jclass clazz) {
    env->GetJavaVM(&g_vm);
    g_cls = (jclass)env->NewGlobalRef(clazz);
    g_menuTouch = env->GetStaticMethodID(clazz, "applyMenuTouch", "(Z)V");
    g_toast = env->GetStaticMethodID(clazz, "showToast", "(Ljava/lang/String;)V");
}

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeSetContext(JNIEnv *env, jclass, jobject ctx) {
    android_clip_set_context(g_vm, ctx);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeSetDisplaySize(JNIEnv *, jclass, jint w, jint h) {
    ui_set_display_size((int)w, (int)h);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeGlSurfaceCreated(JNIEnv *, jclass) {
    ui_gl_surface_created();
}

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeGlSurfaceChanged(JNIEnv *, jclass, jint w, jint h) {
    ui_gl_surface_changed((int)w, (int)h);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeGlSurfaceDestroyed(JNIEnv *, jclass) {
    ui_gl_surface_destroyed();
}

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeGlDrawFrame(JNIEnv *, jclass) { ui_gl_draw_frame(); }

extern "C" JNIEXPORT jint JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeGlFrameDelayMs(JNIEnv *, jclass) {
    return ui_gl_frame_delay_ms();
}

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeTouch(JNIEnv *, jclass, jint action, jfloat x, jfloat y) {
    ui_touch((int)action, x, y);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeOpenMenu(JNIEnv *, jclass) { ui_open_menu(); }

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeToggleMenu(JNIEnv *, jclass) { ui_toggle_menu(); }

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeMinimizeMenu(JNIEnv *, jclass) { ui_minimize_menu(); }

extern "C" JNIEXPORT jboolean JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeIsMenuInteractive(JNIEnv *, jclass) {
    return ui_menu_interactive() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeNeedsContinuousGl(JNIEnv *, jclass) {
    return ui_needs_continuous_gl() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeStartEsp(JNIEnv *, jclass) { ui_esp_start(); }

extern "C" JNIEXPORT void JNICALL
Java_com_xorel_codm_external_NativeBridge_nativeStopEsp(JNIEnv *, jclass) { ui_esp_stop(); }
