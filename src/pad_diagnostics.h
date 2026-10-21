// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AEGIS_PAD_DIAGNOSTICS_H
#define AEGIS_PAD_DIAGNOSTICS_H
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* A successful SDL return is driver acceptance, not confirmation from a motor. */
static inline void pad_log(FILE *file, const char *format, ...) {
    if (!file) return;
    fprintf(file,"Pad[%llu ms]: ",(unsigned long long)SDL_GetTicks());
    va_list args; va_start(args,format); vfprintf(file,format,args); va_end(args);
    fputc('\n',file); fflush(file);
}
static inline const char *pad_text(const char *text) { return text ? text : "(unset)"; }
static inline void pad_log_sdl(FILE *file,const char *role) {
    const int version=SDL_GetVersion();
    pad_log(file,"session role=%s SDL=%d.%d.%d revision=%s platform=%s; SDL success means driver acceptance only",
            role,SDL_VERSIONNUM_MAJOR(version),SDL_VERSIONNUM_MINOR(version),SDL_VERSIONNUM_MICRO(version),
            SDL_GetRevision(),SDL_GetPlatform());
#ifdef _WIN32
    wchar_t path[32768]; char utf8[65536];
    HMODULE module=GetModuleHandleW(L"SDL3.dll");
    DWORD size=module ? GetModuleFileNameW(module,path,32768) : 0;
    if (size && size<32768 && WideCharToMultiByte(CP_UTF8,0,path,-1,utf8,sizeof utf8,NULL,NULL))
        pad_log(file,"SDL module=%s",utf8);
#endif
    const char *hints[]={"SDL_JOYSTICK_HIDAPI","SDL_JOYSTICK_HIDAPI_PS4","SDL_JOYSTICK_HIDAPI_PS5",
        "SDL_JOYSTICK_ENHANCED_REPORTS","SDL_XINPUT_ENABLED","SDL_JOYSTICK_RAWINPUT",
        "SDL_JOYSTICK_WGI","SDL_JOYSTICK_GAMEINPUT","SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"};
    for (size_t i=0;i<sizeof hints/sizeof hints[0];++i)
        pad_log(file,"hint %s=%s",hints[i],pad_text(SDL_GetHint(hints[i])));
}
static inline void pad_log_device(FILE *file,SDL_Gamepad *pad,const char *event) {
    if (!pad) { pad_log(file,"%s device=none error=%s",event,SDL_GetError()); return; }
    const SDL_JoystickID id=SDL_GetGamepadID(pad);
    char guid[33]; SDL_GUIDToString(SDL_GetGamepadGUIDForID(id),guid,sizeof guid);
    const SDL_PropertiesID props=SDL_GetGamepadProperties(pad);
    pad_log(file,"%s id=%u name=\"%s\" guid=%s vendor=%04x product=%04x version=%04x type=%s real_type=%s connection=%d rumble=%d trigger_rumble=%d RGB=%d path=\"%s\"",
        event,(unsigned)id,pad_text(SDL_GetGamepadName(pad)),guid,SDL_GetGamepadVendor(pad),
        SDL_GetGamepadProduct(pad),SDL_GetGamepadProductVersion(pad),
        pad_text(SDL_GetGamepadStringForType(SDL_GetGamepadType(pad))),
        pad_text(SDL_GetGamepadStringForType(SDL_GetRealGamepadType(pad))),
        (int)SDL_GetGamepadConnectionState(pad),
        SDL_GetBooleanProperty(props,SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN,false),
        SDL_GetBooleanProperty(props,SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN,false),
        SDL_GetBooleanProperty(props,SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN,false),pad_text(SDL_GetGamepadPath(pad)));
}
static inline bool pad_log_rumble(FILE *file,SDL_Gamepad *pad,const char *source,
                                  Uint16 low,Uint16 high,Uint32 duration) {
    SDL_ClearError();
    const bool result=pad && SDL_RumbleGamepad(pad,low,high,duration);
    pad_log(file,"output source=%s id=%u low=%u high=%u duration_ms=%u SDL_accepted=%d error=\"%s\"",
        source,pad ? (unsigned)SDL_GetGamepadID(pad) : 0,(unsigned)low,(unsigned)high,(unsigned)duration,
        result,result ? "" : pad ? SDL_GetError() : "no device");
    return result;
}
#endif
