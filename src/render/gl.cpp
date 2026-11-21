#include "render/gl.h"

#include <GLFW/glfw3.h>

#include <string>

namespace gl {
namespace {

const char* g_last_error = "";

void* Resolve(const char* name) {
    void* proc = reinterpret_cast<void*>(glfwGetProcAddress(name));
    if (proc == nullptr) {
        g_last_error = name;
    }
    return proc;
}

}  // namespace

#define X(name, ret, args) PFN_##name name = nullptr;
GL_FUNC_LIST(X)
#undef X

bool LoadAll() {
    g_last_error = "";
    bool ok = true;
#define X(name, ret, args)                                                      \
    name = reinterpret_cast<PFN_##name>(Resolve("gl" #name));                   \
    if (name == nullptr) ok = false;
    GL_FUNC_LIST(X)
#undef X
    return ok;
}

const char* LastError() { return g_last_error; }

}  // namespace gl
