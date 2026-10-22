// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AEGIS_PAD_DIAGNOSTICS_H
#define AEGIS_PAD_DIAGNOSTICS_H
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
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
/* Common Windows pads (Switch Pro, Stadia, Xbox) are only complete when their HIDAPI/RawInput
 * drivers are on. These are SDL's defaults, but an outer launcher or wrapper may have changed
 * them; the game and the launcher both ask for the full set before SDL starts. */
static inline void pad_set_compatibility_hints(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STADIA, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
}
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
        "SDL_JOYSTICK_HIDAPI_XBOX","SDL_JOYSTICK_HIDAPI_SWITCH","SDL_JOYSTICK_HIDAPI_STADIA",
        "SDL_JOYSTICK_ENHANCED_REPORTS","SDL_XINPUT_ENABLED","SDL_JOYSTICK_RAWINPUT",
        "SDL_JOYSTICK_WGI","SDL_JOYSTICK_GAMEINPUT","SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"};
    for (size_t i=0;i<sizeof hints/sizeof hints[0];++i)
        pad_log(file,"hint %s=%s",hints[i],pad_text(SDL_GetHint(hints[i])));
}
/* Not every common controller has an SDL gamepad mapping. A raw joystick gets a standard
 * position mapping once, so unknown pads can still reach the game instead of being ignored. */
static inline void pad_map_unknown_joysticks(FILE *file) {
    static char known[32][33];
    static int known_count;
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int i=0;i<count;++i) {
        if (SDL_IsGamepad(ids[i])) continue;
        char guid[33];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]),guid,sizeof guid);
        int seen=0;
        for (int j=0;j<known_count;++j) if (!strcmp(known[j],guid)) { seen=1; break; }
        if (seen) continue;
        if (known_count<(int)(sizeof known/sizeof known[0]))
            snprintf(known[known_count++],sizeof known[0],"%s",guid);
        char mapping[512];
        snprintf(mapping,sizeof mapping,
            "%s,Aegis fallback pad,"
            "a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,guide:b8,leftstick:b9,rightstick:b10,"
            "leftshoulder:b4,rightshoulder:b5,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
            "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5",
            guid);
        const int added=SDL_AddGamepadMapping(mapping);
        pad_log(file,"gamepad fallback id=%u name=\"%s\" guid=%s added=%d error=%s",
                (unsigned)ids[i],pad_text(SDL_GetJoystickNameForID(ids[i])),guid,added,
                added<0 ? SDL_GetError() : "");
    }
    SDL_free(ids);
}
static inline void pad_log_joysticks(FILE *file,const char *event) {
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    pad_log(file,"%s joysticks=%d",event,count);
    for (int i=0;i<count;++i) {
        char guid[33];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]),guid,sizeof guid);
        pad_log(file,"%s id=%u name=\"%s\" guid=%s gamepad=%d vendor=%04x product=%04x path=\"%s\"",
                event,(unsigned)ids[i],pad_text(SDL_GetJoystickNameForID(ids[i])),guid,
                SDL_IsGamepad(ids[i]),SDL_GetJoystickVendorForID(ids[i]),
                SDL_GetJoystickProductForID(ids[i]),pad_text(SDL_GetJoystickPathForID(ids[i])));
    }
    SDL_free(ids);
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
