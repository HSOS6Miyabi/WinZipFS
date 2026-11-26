#pragma once
#include <stdint.h>
#include <wchar.h>
#ifdef WZ_BUILD
#define WZ_API __declspec(dllexport)
#else
#define WZ_API __declspec(dllimport)
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct WzOptions {
    int batch, delaySeconds, idleMinutes, level, method, threads, solid;
} WzOptions;
typedef struct WzVolume {
    wchar_t archive[32768], mount[16], status[512];
    int readOnly, dirty;
    unsigned handles;
    wchar_t sessionId[64];
    WzOptions options;
} WzVolume;
WZ_API int wz_mount(const wchar_t* path, const WzOptions* options, wchar_t* mount, unsigned mountSize);
typedef struct WzRecovery {
    wchar_t id[64];
    wchar_t archive[32768];
    wchar_t folder[32768];
} WzRecovery;
WZ_API int wz_mount_password(const wchar_t* path, const WzOptions* options, const wchar_t* password, wchar_t* mount, unsigned mountSize);
WZ_API int wz_password_required(void);
WZ_API int wz_recovery_count(void);
WZ_API int wz_recovery_get(int index, WzRecovery* info);
WZ_API int wz_recovery_export(const wchar_t* id, const wchar_t* destination);
WZ_API int wz_recovery_discard(const wchar_t* id);
WZ_API int wz_mount_readonly(const wchar_t* path, const WzOptions* options, wchar_t* mount, unsigned mountSize);
WZ_API int wz_save_copy(const wchar_t* mount, const wchar_t* destination);
WZ_API int wz_discard(const wchar_t* mount);
WZ_API int wz_count(void);
WZ_API int wz_get(int index, WzVolume* info);
WZ_API int wz_options(const wchar_t* mount, const WzOptions* options);
WZ_API int wz_flush(const wchar_t* mount);
WZ_API int wz_eject(const wchar_t* mount);
WZ_API int wz_tick(void);
WZ_API int wz_shutdown(void);
WZ_API void wz_error(wchar_t* buffer, unsigned size);
WZ_API void wz_default_options(WzOptions* options);
WZ_API const wchar_t* wz_extensions(void);
#ifdef __cplusplus
}
#endif
