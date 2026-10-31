#ifndef ANDROID_API_LEVEL_H
#define ANDROID_API_LEVEL_H

/*
 * When this vendored header is put on the include path ahead of the NDK
 * sysroot (CMake Android builds), defer to the NDK's real <android/api-level.h>
 * so that the __ANDROID_API_* macros used by the platform headers
 * (__ANDROID_API_U__ and friends) stay defined. Platform (Soong) builds do not
 * use this path.
 */
#if defined(__ANDROID__)
#include_next <android/api-level.h>
#else
/*
 * Magic version number for a current development build, which has
 * not yet turned into an official release.
 */
#define __ANDROID_API__ 10000
#endif

#endif /* ANDROID_API_LEVEL_H */