// paths.cpp — 3DS paths -> platform paths, applied to the game's file calls (linked with -Wl,--wrap=fopen,...).
//
//   sdmc:/3ds/w3dproto/... -> Vita ux0:data/wowvita/...   (the game's folder: packs, settings, logs)
//   sdmc:/3ds/...   -> Vita ux0:data/...
//   sdmc:/...       -> Vita ux0:data/...
//   romfs:/...      -> Vita app0:/...           (files inside the VPK)
// On the PC the roots come from W3D_SDMC (default ./pc_sdmc) and W3D_ROMFS (default ./data).
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

std::string root(bool romfs) {
#if defined(__vita__)
    return romfs ? "app0:/" : "ux0:data/";
#else
    const char* e = getenv(romfs ? "W3D_ROMFS" : "W3D_SDMC");
    std::string r = e && *e ? e : (romfs ? "data" : "pc_sdmc");
    if (r.back() != '/') r += '/';
    return r;
#endif
}

}  // namespace

namespace compat {

std::string mapPath(const char* p) {
    if (!p) return std::string();
    if (!strncmp(p, "sdmc:/3ds/w3dproto", 18) && (p[18] == '/' || !p[18])) return root(false) + "wowvita" + (p + 18);
    if (!strncmp(p, "sdmc:/3ds/", 10)) return root(false) + (p + 10);
    if (!strcmp(p, "sdmc:/3ds")) {
        std::string r = root(false);
        r.pop_back();
        return r;
    }
    if (!strncmp(p, "sdmc:/", 6)) return root(false) + (p + 6);
    if (!strncmp(p, "romfs:/", 7)) return root(true) + (p + 7);
    return p;
}

}  // namespace compat

extern "C" {

FILE* __real_fopen(const char* path, const char* mode);
int __real_open(const char* path, int flags, ...);
int __real_mkdir(const char* path, mode_t mode);
int __real_remove(const char* path);
int __real_rename(const char* a, const char* b);

FILE* __wrap_fopen(const char* path, const char* mode) { return __real_fopen(compat::mapPath(path).c_str(), mode); }

int __wrap_open(const char* path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return __real_open(compat::mapPath(path).c_str(), flags, mode);
}

int __wrap_mkdir(const char* path, mode_t mode) { return __real_mkdir(compat::mapPath(path).c_str(), mode); }
int __wrap_remove(const char* path) { return __real_remove(compat::mapPath(path).c_str()); }
int __wrap_rename(const char* a, const char* b) { return __real_rename(compat::mapPath(a).c_str(), compat::mapPath(b).c_str()); }

}  // extern "C"
