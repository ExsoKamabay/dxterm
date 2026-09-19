// JNI bridge only. All terminal logic lives in engine/*. This file marshals
// between com.dracxterm.NativeTerminal (Kotlin) and xterm::Session (C++).
#include <jni.h>
#include <string>
#include <vector>
#include <cstdint>

#include "core/logger/Log.h"
#include "engine/session/Session.h"
#include "engine/terminal/Terminal.h"

using xterm::Session;
using xterm::Terminal;

namespace {

Session* asSession(jlong handle) { return reinterpret_cast<Session*>(handle); }

std::vector<std::string> toStringVector(JNIEnv* env, jobjectArray arr) {
    std::vector<std::string> out;
    if (!arr) return out;
    jsize n = env->GetArrayLength(arr);
    out.reserve(n);
    for (jsize i = 0; i < n; ++i) {
        auto js = reinterpret_cast<jstring>(env->GetObjectArrayElement(arr, i));
        if (!js) continue;
        const char* c = env->GetStringUTFChars(js, nullptr);
        out.emplace_back(c ? c : "");
        env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
    }
    return out;
}

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return r;
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_dracxterm_NativeTerminal_nativeCreate(
        JNIEnv* env, jclass, jint cols, jint rows,
        jobjectArray argvArr, jobjectArray envArr, jstring cwd) {
    auto argv = toStringVector(env, argvArr);
    auto envv = toStringVector(env, envArr);
    std::string cwdStr = jstr(env, cwd);
    if (argv.empty()) { LOGE("nativeCreate: empty argv"); return 0; }

    auto* s = new Session(cols > 0 ? cols : 80, rows > 0 ? rows : 24);
    if (!s->start(argv, envv, cwdStr)) {
        LOGE("nativeCreate: session start failed");
        delete s;
        return 0;
    }
    LOGI("session started (%dx%d) argv0=%s", cols, rows, argv[0].c_str());
    return reinterpret_cast<jlong>(s);
}

JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeWrite(
        JNIEnv* env, jclass, jlong handle, jbyteArray data) {
    Session* s = asSession(handle);
    if (!s || !data) return;
    jsize len = env->GetArrayLength(data);
    if (len <= 0) return;
    jbyte* p = env->GetByteArrayElements(data, nullptr);
    if (!p) return;                       // pinning failed (OOM): bail safely
    s->write(reinterpret_cast<uint8_t*>(p), static_cast<size_t>(len));
    env->ReleaseByteArrayElements(data, p, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeResize(
        JNIEnv*, jclass, jlong handle, jint cols, jint rows) {
    Session* s = asSession(handle);
    if (s) s->resize(cols, rows);
}

JNIEXPORT jlong JNICALL
Java_com_dracxterm_NativeTerminal_nativeGeneration(JNIEnv*, jclass, jlong handle) {
    Session* s = asSession(handle);
    return s ? static_cast<jlong>(s->generation()) : 0;
}

JNIEXPORT jboolean JNICALL
Java_com_dracxterm_NativeTerminal_nativeRunning(JNIEnv*, jclass, jlong handle) {
    Session* s = asSession(handle);
    return (s && s->running()) ? JNI_TRUE : JNI_FALSE;
}

// Returns cursor index (row*cols+col), -1 if scrolled off, or -2 if the arrays
// are too small (caller must realloc). meta is filled per Terminal::M_* indices.
JNIEXPORT jint JNICALL
Java_com_dracxterm_NativeTerminal_nativeSnapshot(
        JNIEnv* env, jclass, jlong handle,
        jintArray glyphs, jintArray fg, jintArray bg, jintArray attr, jintArray meta) {
    Session* s = asSession(handle);
    if (!s) return -1;
    if (!glyphs || !fg || !bg || !attr) return -1;   // caller must pass real buffers

    jsize cap = env->GetArrayLength(glyphs);
    auto* g = reinterpret_cast<uint32_t*>(env->GetIntArrayElements(glyphs, nullptr));
    auto* f = reinterpret_cast<uint32_t*>(env->GetIntArrayElements(fg, nullptr));
    auto* b = reinterpret_cast<uint32_t*>(env->GetIntArrayElements(bg, nullptr));
    auto* a = reinterpret_cast<uint32_t*>(env->GetIntArrayElements(attr, nullptr));
    if (!g || !f || !b || !a) {                       // pinning failed: release what we got
        if (g) env->ReleaseIntArrayElements(glyphs, reinterpret_cast<jint*>(g), JNI_ABORT);
        if (f) env->ReleaseIntArrayElements(fg, reinterpret_cast<jint*>(f), JNI_ABORT);
        if (b) env->ReleaseIntArrayElements(bg, reinterpret_cast<jint*>(b), JNI_ABORT);
        if (a) env->ReleaseIntArrayElements(attr, reinterpret_cast<jint*>(a), JNI_ABORT);
        return -1;
    }

    int m[Terminal::M_COUNT] = {0};
    int cursor = s->terminal().snapshot(g, f, b, a, static_cast<int>(cap), m);
    int need = m[Terminal::M_COLS] * m[Terminal::M_ROWS];
    if (static_cast<int>(cap) < need)
        LOGW("nativeSnapshot: buffer holds %d cells, grid needs %d; output truncated", (int)cap, need);

    env->ReleaseIntArrayElements(glyphs, reinterpret_cast<jint*>(g), 0);
    env->ReleaseIntArrayElements(fg, reinterpret_cast<jint*>(f), 0);
    env->ReleaseIntArrayElements(bg, reinterpret_cast<jint*>(b), 0);
    env->ReleaseIntArrayElements(attr, reinterpret_cast<jint*>(a), 0);

    if (meta) {
        jsize ml = env->GetArrayLength(meta);
        if (ml > Terminal::M_COUNT) ml = Terminal::M_COUNT;
        env->SetIntArrayRegion(meta, 0, ml, reinterpret_cast<jint*>(m));
    }
    return cursor;
}

JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeScroll(JNIEnv*, jclass, jlong h, jint delta) {
    Session* s = asSession(h); if (s) s->terminal().scrollView(delta);
}
JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeScrollToBottom(JNIEnv*, jclass, jlong h) {
    Session* s = asSession(h); if (s) s->terminal().scrollToBottom();
}

JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeSelectStart(JNIEnv*, jclass, jlong h, jint r, jint c) {
    Session* s = asSession(h); if (s) s->terminal().selectStart(r, c);
}
JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeSelectExtend(JNIEnv*, jclass, jlong h, jint r, jint c) {
    Session* s = asSession(h); if (s) s->terminal().selectExtend(r, c);
}
JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeSelectWord(JNIEnv*, jclass, jlong h, jint r, jint c) {
    Session* s = asSession(h); if (s) s->terminal().selectWord(r, c);
}
JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeClearSelection(JNIEnv*, jclass, jlong h) {
    Session* s = asSession(h); if (s) s->terminal().clearSelection();
}
JNIEXPORT jstring JNICALL
Java_com_dracxterm_NativeTerminal_nativeSelectionText(JNIEnv* env, jclass, jlong h) {
    Session* s = asSession(h);
    return env->NewStringUTF(s ? s->terminal().selectionText().c_str() : "");
}

JNIEXPORT jint JNICALL
Java_com_dracxterm_NativeTerminal_nativeSearch(
        JNIEnv* env, jclass, jlong h, jstring q, jint fromRow, jboolean forward) {
    Session* s = asSession(h);
    if (!s) return -1;
    return s->terminal().search(jstr(env, q), fromRow, forward == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeMouse(
        JNIEnv*, jclass, jlong h, jint button, jint col, jint row, jint type) {
    Session* s = asSession(h); if (s) s->terminal().mouseEvent(button, col, row, type);
}

JNIEXPORT jbyteArray JNICALL
Java_com_dracxterm_NativeTerminal_nativeWrapPaste(JNIEnv* env, jclass, jlong h, jstring text) {
    Session* s = asSession(h);
    std::string wrapped = s ? s->terminal().wrapPaste(jstr(env, text)) : jstr(env, text);
    jbyteArray out = env->NewByteArray((jsize)wrapped.size());
    env->SetByteArrayRegion(out, 0, (jsize)wrapped.size(),
                            reinterpret_cast<const jbyte*>(wrapped.data()));
    return out;
}

JNIEXPORT jstring JNICALL
Java_com_dracxterm_NativeTerminal_nativeTitle(JNIEnv* env, jclass, jlong h) {
    Session* s = asSession(h);
    return env->NewStringUTF(s ? s->terminal().takeTitle().c_str() : "");
}
JNIEXPORT jstring JNICALL
Java_com_dracxterm_NativeTerminal_nativeClipboard(JNIEnv* env, jclass, jlong h) {
    Session* s = asSession(h);
    return env->NewStringUTF(s ? s->terminal().takeClipboard().c_str() : "");
}

JNIEXPORT jstring JNICALL
Java_com_dracxterm_NativeTerminal_nativeAppControl(JNIEnv* env, jclass, jlong h) {
    Session* s = asSession(h);
    return env->NewStringUTF(s ? s->terminal().takeAppControl().c_str() : "");
}

JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeConfigure(
        JNIEnv*, jclass, jlong h, jint fg, jint bg, jint cursor, jint scrollback) {
    Session* s = asSession(h);
    if (!s) return;
    xterm::Config c;   // palette keeps engine defaults; colours/scrollback from the app theme
    c.defaultFg   = static_cast<uint32_t>(fg);
    c.defaultBg   = static_cast<uint32_t>(bg);
    c.cursorColor = static_cast<uint32_t>(cursor);
    if (scrollback > 0) c.scrollback = scrollback;
    s->terminal().configure(c);
}

JNIEXPORT jint JNICALL
Java_com_dracxterm_NativeTerminal_nativeCursorColor(JNIEnv*, jclass, jlong h) {
    Session* s = asSession(h);
    return s ? static_cast<jint>(s->terminal().cursorColor()) : 0;
}

JNIEXPORT jstring JNICALL
Java_com_dracxterm_NativeTerminal_nativeCellGrapheme(JNIEnv* env, jclass, jlong h, jint row, jint col) {
    Session* s = asSession(h);
    return env->NewStringUTF(s ? s->terminal().cellGrapheme(row, col).c_str() : "");
}

JNIEXPORT void JNICALL
Java_com_dracxterm_NativeTerminal_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    Session* s = asSession(handle);
    if (s) { s->stop(); delete s; }
}

} // extern "C"
