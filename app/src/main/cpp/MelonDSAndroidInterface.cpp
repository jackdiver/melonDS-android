#include "JniEnvHandler.h"
#include "UriFileHandler.h"
#include "MelonDS.h"
#include "OpenGLContext.h"
#include "netplay/DeterminismHarness.h"

#include <string>
#include <dlfcn.h>

#ifndef M_BIONIC_ZERO_INIT
#define M_BIONIC_ZERO_INIT (-203)
#endif

// mallopt is not declared for minSdk 24; look it up at runtime (bionic has it since API 26)
static void enableZeroInitializedHeap()
{
    using MalloptFn = int (*)(int, int);
    auto fn = reinterpret_cast<MalloptFn>(dlsym(RTLD_DEFAULT, "mallopt"));
    if (fn)
        fn(M_BIONIC_ZERO_INIT, 1);
}

static std::string jstringToString(JNIEnv* env, jstring value)
{
    if (!value)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

JniEnvHandler* jniEnvHandler;

JavaVM* vm;
jobject androidUriFileHandler;
UriFileHandler* fileHandler;

extern "C"
{
JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_setup(JNIEnv* env, jobject thiz, jobject uriFileHandler)
{
    // Netplay determinism: zero-fill all heap allocations so that any emulator state that is never initialised
    // (and not covered by savestates) reads the same on every device. Android 11+; a no-op on older versions.
    enableZeroInitializedHeap();

    env->GetJavaVM(&vm);
    jniEnvHandler = new JniEnvHandler(vm);
    androidUriFileHandler = env->NewGlobalRef(uriFileHandler);
    fileHandler = new UriFileHandler(jniEnvHandler, androidUriFileHandler);

    auto* openGlContext = new OpenGLContext();
    openGlContext->InitContext(0);

    MelonDSAndroid::openGlContext = openGlContext;
    MelonDSAndroid::fileHandler = fileHandler;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_determinismRecord(JNIEnv* env, jobject thiz, jstring directory)
{
    MelonDSAndroid::determinismHarness.requestRecord(jstringToString(env, directory));
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_determinismReplay(JNIEnv* env, jobject thiz, jstring directory, jstring tag)
{
    MelonDSAndroid::determinismHarness.requestReplay(jstringToString(env, directory), jstringToString(env, tag));
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_determinismStop(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::determinismHarness.requestStop();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_mirrorStart(JNIEnv* env, jobject thiz, jint debugFlags)
{
    MelonDSAndroid::requestMirrorStart(debugFlags);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_mirrorStop(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::requestMirrorStop();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_mirrorRecord(JNIEnv* env, jobject thiz, jstring directory)
{
    MelonDSAndroid::requestMirrorRecord(jstringToString(env, directory));
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_mirrorReplay(JNIEnv* env, jobject thiz, jstring directory, jstring tag)
{
    MelonDSAndroid::requestMirrorReplay(jstringToString(env, directory), jstringToString(env, tag));
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_mirrorRecordStop(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::requestMirrorRecordStop();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_netHost(JNIEnv* env, jobject thiz, jint port, jint inputDelay)
{
    MelonDSAndroid::requestNetHost(port, inputDelay);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_netJoin(JNIEnv* env, jobject thiz, jstring address, jint port, jint inputDelay)
{
    MelonDSAndroid::requestNetJoin(jstringToString(env, address), port, inputDelay);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_netStop(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::requestNetStop();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_mirrorFocus(JNIEnv* env, jobject thiz, jint index)
{
    MelonDSAndroid::setMirrorFocus(index);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_determinismSelfTest(JNIEnv* env, jobject thiz, jint frames)
{
    MelonDSAndroid::determinismHarness.requestSelfTest(frames);
}

JNIEXPORT jlong JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_getEmulatorGlContext(JNIEnv* env, jobject thiz)
{
    return (jlong) MelonDSAndroid::openGlContext->GetContext();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_cleanup(JNIEnv* env, jobject thiz)
{
    env->DeleteGlobalRef(androidUriFileHandler);
    androidUriFileHandler = nullptr;
    vm = nullptr;

    MelonDSAndroid::openGlContext->DeInit();

    delete MelonDSAndroid::openGlContext;
    delete fileHandler;
    delete jniEnvHandler;

    MelonDSAndroid::openGlContext = nullptr;
}
}