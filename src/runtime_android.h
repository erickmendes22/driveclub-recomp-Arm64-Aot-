/* Android's document providers are opened by Java, but all guest reads and
 * seeks operate on native descriptors. The callback owns no game-data copy. */
#ifndef DRIVECLUB_RUNTIME_ANDROID_H
#define DRIVECLUB_RUNTIME_ANDROID_H
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#ifdef __cplusplus
extern "C" {
#endif
int driveclub_android_files_active(void);
/* Signal-safe TLS flag: ART still owns managed faults inside provider calls. */
int driveclub_android_in_java(void);
int driveclub_android_is_game_path(const char *path);
/* Return a descriptor or -(host errno); caller owns a successful descriptor. */
int driveclub_android_open(const char *path);
/* Other operations return 0 or a positive host errno. */
int driveclub_android_stat(const char *path, struct stat *out);
int driveclub_android_list(const char *path, char ***entries, size_t *count);
void driveclub_android_free_list(char **entries, size_t count);
int driveclub_android_resolve(const char *path, char *out, size_t size);
void runtime_android_pad_update(uint32_t buttons, float lx, float ly, float rx,
                                float ry, float l2, float r2);
#ifdef __cplusplus
}
#endif
#endif
