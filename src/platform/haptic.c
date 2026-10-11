#include "platform/haptic.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <jni.h>

// OraclesActivity.haptic(): performHapticFeedback(KEYBOARD_TAP) on the game's view.
void haptic_tick(void) {
  static jclass cls;
  static jmethodID tick;
  JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (!env) return;
  if (!cls) {
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (!activity) return;
    jclass local = (*env)->GetObjectClass(env, activity);
    tick = (*env)->GetStaticMethodID(env, local, "haptic", "()V");
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); tick = NULL; }
    if (tick) cls = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    (*env)->DeleteLocalRef(env, activity);
    if (!cls) return;
  }
  (*env)->CallStaticVoidMethod(env, cls, tick);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}
#else
// Tests see the ticks a phone would give.
void haptic_tick(void) {
  static int log = -1;
  if (log < 0) log = getenv("ORACLES_TEST_HAPTICS") != NULL;
  if (log) fprintf(stderr, "haptic\n");
}
#endif
