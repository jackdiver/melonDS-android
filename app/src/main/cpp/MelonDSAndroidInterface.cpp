#include "JniEnvHandler.h"
#include "UriFileHandler.h"
#include "MelonDS.h"
#include "OpenGLContext.h"
#include "netplay/DeterminismHarness.h"

#include <string>

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