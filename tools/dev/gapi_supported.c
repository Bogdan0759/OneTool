#include <stdio.h>
#include <dlfcn.h>

static void check_gapi(const char* name, const char* libname, const char* fallback_lib) {
    void* handle = dlopen(libname, RTLD_LAZY);
    if (handle) {
        printf("[OK] %s supported (found %s)\n", name, libname);
        dlclose(handle);
        return;
    }
    
    handle = dlopen(fallback_lib, RTLD_LAZY);
    if (handle) {
        printf("[OK] %s supported (found %s)\n", name, fallback_lib);
        dlclose(handle);
        return;
    }
    
    printf("[--] %s NOT available\n", name);
}

int main(int argc, char *argv[]) {
    printf("This tool is the part of OneTool project that published in MPL-2.0 license\n");
    
    check_gapi  ("Vulkan", "libvulkan.so.1", "libvulkan.so");
    check_gapi("OpenGL", "libGL.so.1", "libGL.so");
    check_gapi("EGL", "libEGL.so.1", "libEGL.so");
    check_gapi("OpenGL ES 2.0/3.0", "libGLESv2.so.2", "libGLESv2.so");
    check_gapi("OpenGL ES 1.1", "libGLESv1_CM.so.1", "libGLESv1_CM.so");
    check_gapi("OpenCL", "libOpenCL.so.1", "libOpenCL.so");
    
    return 0;
}
