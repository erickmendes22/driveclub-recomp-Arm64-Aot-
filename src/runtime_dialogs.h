/* SPDX-License-Identifier: GPL-2.0-or-later
 * Guest ABI layouts used by the offline dialog provider. See
 * tools/README-service-contracts.md for reference headers and supported modes. */
#ifndef BB_RUNTIME_DIALOGS_H
#define BB_RUNTIME_DIALOGS_H
#include <stdint.h>

enum { DIALOG_NONE, DIALOG_INITIALIZED, DIALOG_RUNNING, DIALOG_FINISHED };
enum { DIALOG_RESULT_OK, DIALOG_RESULT_CANCELED };
#define DIALOG_NOT_SYSTEM_INITIALIZED ((int32_t)0x80B80001)
#define DIALOG_ALREADY_SYSTEM_INITIALIZED ((int32_t)0x80B80002)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define DIALOG_ALREADY_INITIALIZED ((int32_t)0x80B80004)
#define DIALOG_NOT_FINISHED ((int32_t)0x80B80005)
#define DIALOG_INVALID_STATE ((int32_t)0x80B80006)
#define DIALOG_BUSY ((int32_t)0x80B80008)
#define DIALOG_PARAM_INVALID ((int32_t)0x80B8000A)
#define DIALOG_NOT_RUNNING ((int32_t)0x80B8000B)
#define DIALOG_ARG_NULL ((int32_t)0x80B8000D)
#define DIALOG_NOT_SUPPORTED ((int32_t)0x80B8000F)

typedef struct { uint64_t size; uint8_t reserved[36]; uint32_t magic; } DialogBase;
typedef struct {
    uint32_t button_type, pad;
    const char *message;
    const void *buttons;
    uint8_t reserved[24];
} MessageUserParam;
typedef struct {
    uint32_t type, pad;
    const char *message;
    uint8_t reserved[64];
} MessageProgressParam;
typedef struct { uint32_t type; uint8_t reserved[32]; } MessageSystemParam;
typedef struct {
    DialogBase base;
    uint64_t size;
    uint32_t mode, pad;
    const MessageUserParam *user;
    const MessageProgressParam *progress;
    const MessageSystemParam *system;
    int32_t user_id;
    uint8_t reserved[40];
    uint32_t pad2;
} MessageDialogParam;
typedef struct { uint32_t mode, result, button; uint8_t reserved[32]; } MessageDialogResult;

typedef struct {
    uint32_t button_type, message_type;
    const char *message;
    uint8_t reserved[32];
} SaveUserParam;
typedef struct { uint32_t type, pad; uint64_t value; uint8_t reserved[32]; } SaveSystemParam;
typedef struct {
    uint32_t type, pad;
    const char *message;
    uint32_t system_type;
    uint8_t reserved[28];
} SaveProgressParam;
typedef struct {
    DialogBase base;
    int32_t size;
    uint32_t mode, display_type, pad;
    const void *animation, *items;
    const SaveUserParam *user;
    const SaveSystemParam *system;
    const void *error;
    const SaveProgressParam *progress;
    void *user_data;
    const void *option;
    uint8_t reserved[24];
} SaveDialogParam;
typedef struct {
    uint32_t mode, result, button, pad;
    void *dir_name, *param, *user_data;
    uint8_t reserved[32];
} SaveDialogResult;
typedef struct {
    DialogBase base;
    uint64_t size;
    uint32_t mode;
    int32_t user_id;
    char online_id[20];
    uint32_t pad;
    void *user_data;
    uint8_t reserved[32];
} ProfileDialogParam;
typedef struct {
    int32_t result;
    uint32_t user_action;
    void *user_data;
    uint8_t reserved[32];
} ProfileDialogResult;
typedef struct { int32_t size, error_code, user_id, reserved; } ErrorDialogParam;
typedef struct {
    DialogBase base;
    int32_t size, user_id, mode;
    uint32_t service_label;
    const char *const *targets;
    uint32_t target_count, pad;
    uint64_t features;
    void *user_data;
    uint8_t reserved[32];
} CommerceDialogParam;
typedef struct {
    int32_t result;
    uint8_t authorized, pad[3];
    void *user_data;
    uint8_t reserved[32];
} CommerceDialogResult;

#endif
