// GL 入口：Windows 只导出 OpenGL 1.1，2.0 以上的函数一律经 glfwGetProcAddress
// 装载。全工程只在这里声明函数指针，别处只用 gl:: 命名空间调用。
#pragma once

#include <windows.h>
#include <GL/gl.h>

#include <cstddef>

using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

// GL 1.1 头文件里没有的枚举（按 GL 3.3 core 规范取值）
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_R8 0x8229
#define GL_RED 0x1903
#define GL_RGBA8 0x8058
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_MULTISAMPLE 0x809D
#define GL_FUNC_ADD 0x8006
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

#define GL_FUNC_LIST(X)                                                        \
    X(GenVertexArrays, void, (GLsizei n, GLuint* arrays))                      \
    X(BindVertexArray, void, (GLuint array))                                   \
    X(DeleteVertexArrays, void, (GLsizei n, const GLuint* arrays))             \
    X(GenBuffers, void, (GLsizei n, GLuint* buffers))                          \
    X(BindBuffer, void, (GLenum target, GLuint buffer))                        \
    X(BufferData, void, (GLenum target, GLsizeiptr size, const void* data,      \
                         GLenum usage))                                        \
    X(DeleteBuffers, void, (GLsizei n, const GLuint* buffers))                 \
    X(VertexAttribPointer, void, (GLuint index, GLint size, GLenum type,        \
                                  GLboolean normalized, GLsizei stride,         \
                                  const void* pointer))                        \
    X(EnableVertexAttribArray, void, (GLuint index))                           \
    X(DisableVertexAttribArray, void, (GLuint index))                          \
    X(CreateShader, GLuint, (GLenum type))                                     \
    X(ShaderSource, void, (GLuint shader, GLsizei count,                        \
                           const GLchar* const* string, const GLint* length))   \
    X(CompileShader, void, (GLuint shader))                                    \
    X(GetShaderiv, void, (GLuint shader, GLenum pname, GLint* params))         \
    X(GetShaderInfoLog, void, (GLuint shader, GLsizei bufSize,                  \
                               GLsizei* length, GLchar* infoLog))               \
    X(CreateProgram, GLuint, (void))                                           \
    X(AttachShader, void, (GLuint program, GLuint shader))                     \
    X(LinkProgram, void, (GLuint program))                                     \
    X(GetProgramiv, void, (GLuint program, GLenum pname, GLint* params))       \
    X(GetProgramInfoLog, void, (GLuint program, GLsizei bufSize,                \
                                GLsizei* length, GLchar* infoLog))              \
    X(UseProgram, void, (GLuint program))                                      \
    X(DeleteShader, void, (GLuint shader))                                     \
    X(DeleteProgram, void, (GLuint program))                                   \
    X(GetUniformLocation, GLint, (GLuint program, const GLchar* name))         \
    X(Uniform1i, void, (GLint location, GLint v0))                             \
    X(Uniform1f, void, (GLint location, GLfloat v0))                           \
    X(Uniform2f, void, (GLint location, GLfloat v0, GLfloat v1))               \
    X(Uniform3f, void, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))   \
    X(Uniform4f, void, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2,     \
                        GLfloat v3))                                           \
    X(Uniform1fv, void, (GLint location, GLsizei count, const GLfloat* value)) \
    X(Uniform2fv, void, (GLint location, GLsizei count, const GLfloat* value)) \
    X(UniformMatrix4fv, void, (GLint location, GLsizei count,                   \
                               GLboolean transpose, const GLfloat* value))     \
    X(ActiveTexture, void, (GLenum texture))                                   \
    X(GenFramebuffers, void, (GLsizei n, GLuint* framebuffers))                \
    X(BindFramebuffer, void, (GLenum target, GLuint framebuffer))              \
    X(FramebufferTexture2D, void, (GLenum target, GLenum attachment,            \
                                   GLenum textarget, GLuint texture,            \
                                   GLint level))                                \
    X(CheckFramebufferStatus, GLenum, (GLenum target))                         \
    X(DeleteFramebuffers, void, (GLsizei n, const GLuint* framebuffers))       \
    X(BlendFuncSeparate, void, (GLenum sfactorRGB, GLenum dfactorRGB,           \
                                GLenum sfactorAlpha, GLenum dfactorAlpha))     \
    X(GenerateMipmap, void, (GLenum target))

namespace gl {

#define X(name, ret, args) using PFN_##name = ret(__stdcall*) args;
GL_FUNC_LIST(X)
#undef X

#define X(name, ret, args) extern PFN_##name name;
GL_FUNC_LIST(X)
#undef X

// 装载全部函数指针；任一为空说明驱动不支持 GL 3.3 core。
bool LoadAll();
const char* LastError();

}  // namespace gl
