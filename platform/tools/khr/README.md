Minimal EGL / GLES2 headers for the PC test build: only the declarations and constants platform/ uses.
Use them when the system has Mesa's libraries but not their headers (libegl-dev, libgles-dev):

    cmake -S . -B build-pc -DW3D_PC=1 -DKHR_INCLUDE=$PWD/platform/tools/khr/include
