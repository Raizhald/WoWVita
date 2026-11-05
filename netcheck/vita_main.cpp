// vita_main.cpp — PS Vita entry point for the network test client (net/tools/w3dlogin.cpp).
//
// The Vita has no command line, so the arguments come from a text file, one argument per line:
//
//     ux0:data/wowvita/w3dlogin.txt
//         192.168.1.10:3724
//         MYACCOUNT
//         mypassword
//         --stay
//         30
//
// Empty lines and lines starting with '#' are skipped. Everything w3dlogin prints goes to
// ux0:data/wowvita/w3dlogin.log (stdout and stderr), so the run can be checked afterwards with VitaShell.
// This is a test of the network layer only: no graphics, the screen stays black until the run ends.

#include <cstdio>
#include <string>
#include <vector>

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

int w3dlogin_main(int argc, char** argv);   // net/tools/w3dlogin.cpp, main renamed by the build

static const char* kDir  = "ux0:data/wowvita";
static const char* kArgs = "ux0:data/wowvita/w3dlogin.txt";
static const char* kLog  = "ux0:data/wowvita/w3dlogin.log";

// Auto-suspend would drop the Wi-Fi in the middle of a long --stay run: tick the power timer from a side thread.
static int keepAwake(SceSize, void*) {
    for (;;) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        sceKernelDelayThread(10 * 1000 * 1000);
    }
    return 0;
}

int main() {
    sceIoMkdir(kDir, 0777);
    std::freopen(kLog, "w", stdout);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   // line-buffered: the log survives a crash up to the last line
    std::freopen(kLog, "a", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::vector<std::string> args{"w3dlogin"};
    if (FILE* f = std::fopen(kArgs, "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
            size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos || s[b] == '#') continue;
            args.push_back(s.substr(b));
        }
        std::fclose(f);
    } else {
        std::printf("missing %s (one argument per line: server[:port], account, password, options)\n", kArgs);
        std::fflush(stdout);
        sceKernelExitProcess(1);
        return 1;
    }

    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    SceUID t = sceKernelCreateThread("w3d_awake", keepAwake, 0x10000100, 0x1000, 0, 0, nullptr);
    if (t >= 0) sceKernelStartThread(t, 0, nullptr);
    scePowerSetArmClockFrequency(444);
    int rc = w3dlogin_main((int)args.size(), argv.data());
    std::printf("\n[w3dlogin exited with %d]\n", rc);
    std::fflush(stdout);
    sceKernelExitProcess(rc);
    return rc;
}
