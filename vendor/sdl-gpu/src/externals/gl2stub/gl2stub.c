/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gl2stub.h"
#include "SDL.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

// Visual C does not support static inline
#ifndef static_inline
    #ifdef _MSC_VER
        #define static_inline static
    #else
        #define static_inline static inline
    #endif
#endif

#ifdef _WIN32
  #define GL2STUB_GLES2_LIBRARY "libGLESv2.dll"

  static_inline int IsWindowsVistaOrGreater() {
    // Avoiding windows.h
    struct /* OSVERSIONINFO */ {
      unsigned dwOSVersionInfoSize;
      unsigned dwMajorVersion;
      unsigned dwMinorVersion;
      unsigned dwBuildNumber;
      unsigned dwPlatformId;
      char     szCSDVersion[128];
    } verinfo = {};

    verinfo.dwOSVersionInfoSize = sizeof(verinfo);
    void *kernel32 = SDL_LoadObject("kernel32.dll");
    if (kernel32) {
      char (*__stdcall pGetVersionEx)(void *) = SDL_LoadFunction(kernel32, "GetVersionExA");
      if (pGetVersionEx && !pGetVersionEx(&verinfo))
        verinfo.dwMajorVersion = 0;
      SDL_UnloadObject(kernel32);
    }

    return verinfo.dwMajorVersion >= 6;
  }
#else
  // Currently macOS does not appear to be supported by SDL, default to Unix-like name.
  #define GL2STUB_GLES2_LIBRARY "libGLESv2.so"
#endif

static void *gles2handle;

static GLboolean gl2stubInit() {
  // We cannot call SDL_EGL_LoadLibrary, sicne SDL_GL_LoadLibrary will default to WIN_GL_LoadLibrary.
  // Therefore we partially reimplement it.

#ifdef _WIN32
  const char *d3dcompiler = SDL_GetHint(SDL_HINT_VIDEO_WIN_D3DCOMPILER);
  if (!d3dcompiler) {
    if (IsWindowsVistaOrGreater()) {
      d3dcompiler = "d3dcompiler_46.dll";
    } else {
      d3dcompiler = "d3dcompiler_43.dll";
    }
  }
  if (SDL_strcasecmp(d3dcompiler, "none") != 0) {
    if (SDL_LoadObject(d3dcompiler) == NULL) {
        SDL_ClearError();
    }
  }
#endif

  gles2handle = SDL_LoadObject(GL2STUB_GLES2_LIBRARY);

  #define FIND_PROC(s) s = SDL_LoadFunction(gles2handle, #s)
  FIND_PROC(glActiveTexture);
  FIND_PROC(glAttachShader);
  FIND_PROC(glBindAttribLocation);
  FIND_PROC(glBindBuffer);
  FIND_PROC(glBindFramebuffer);
  FIND_PROC(glBindRenderbuffer);
  FIND_PROC(glBindTexture);
  FIND_PROC(glBlendColor);
  FIND_PROC(glBlendEquation);
  FIND_PROC(glBlendEquationSeparate);
  FIND_PROC(glBlendFunc);
  FIND_PROC(glBlendFuncSeparate);
  FIND_PROC(glBufferData);
  FIND_PROC(glBufferSubData);
  FIND_PROC(glCheckFramebufferStatus);
  FIND_PROC(glClear);
  FIND_PROC(glClearColor);
  FIND_PROC(glClearDepthf);
  FIND_PROC(glClearStencil);
  FIND_PROC(glColorMask);
  FIND_PROC(glCompileShader);
  FIND_PROC(glCompressedTexImage2D);
  FIND_PROC(glCompressedTexSubImage2D);
  FIND_PROC(glCopyTexImage2D);
  FIND_PROC(glCopyTexSubImage2D);
  FIND_PROC(glCreateProgram);
  FIND_PROC(glCreateShader);
  FIND_PROC(glCullFace);
  FIND_PROC(glDeleteBuffers);
  FIND_PROC(glDeleteFramebuffers);
  FIND_PROC(glDeleteProgram);
  FIND_PROC(glDeleteRenderbuffers);
  FIND_PROC(glDeleteShader);
  FIND_PROC(glDeleteTextures);
  FIND_PROC(glDepthFunc);
  FIND_PROC(glDepthMask);
  FIND_PROC(glDepthRangef);
  FIND_PROC(glDetachShader);
  FIND_PROC(glDisable);
  FIND_PROC(glDisableVertexAttribArray);
  FIND_PROC(glDrawArrays);
  FIND_PROC(glDrawElements);
  FIND_PROC(glEnable);
  FIND_PROC(glEnableVertexAttribArray);
  FIND_PROC(glFinish);
  FIND_PROC(glFlush);
  FIND_PROC(glFramebufferRenderbuffer);
  FIND_PROC(glFramebufferTexture2D);
  FIND_PROC(glFrontFace);
  FIND_PROC(glGenBuffers);
  FIND_PROC(glGenerateMipmap);
  FIND_PROC(glGenFramebuffers);
  FIND_PROC(glGenRenderbuffers);
  FIND_PROC(glGenTextures);
  FIND_PROC(glGetActiveAttrib);
  FIND_PROC(glGetActiveUniform);
  FIND_PROC(glGetAttachedShaders);
  FIND_PROC(glGetAttribLocation);
  FIND_PROC(glGetBooleanv);
  FIND_PROC(glGetBufferParameteriv);
  FIND_PROC(glGetError);
  FIND_PROC(glGetFloatv);
  FIND_PROC(glGetFramebufferAttachmentParameteriv);
  FIND_PROC(glGetIntegerv);
  FIND_PROC(glGetProgramiv);
  FIND_PROC(glGetProgramInfoLog);
  FIND_PROC(glGetRenderbufferParameteriv);
  FIND_PROC(glGetShaderiv);
  FIND_PROC(glGetShaderInfoLog);
  FIND_PROC(glGetShaderPrecisionFormat);
  FIND_PROC(glGetShaderSource);
  FIND_PROC(glGetString);
  FIND_PROC(glGetTexParameterfv);
  FIND_PROC(glGetTexParameteriv);
  FIND_PROC(glGetUniformfv);
  FIND_PROC(glGetUniformiv);
  FIND_PROC(glGetUniformLocation);
  FIND_PROC(glGetVertexAttribfv);
  FIND_PROC(glGetVertexAttribiv);
  FIND_PROC(glGetVertexAttribPointerv);
  FIND_PROC(glHint);
  FIND_PROC(glIsBuffer);
  FIND_PROC(glIsEnabled);
  FIND_PROC(glIsFramebuffer);
  FIND_PROC(glIsProgram);
  FIND_PROC(glIsRenderbuffer);
  FIND_PROC(glIsShader);
  FIND_PROC(glIsTexture);
  FIND_PROC(glLineWidth);
  FIND_PROC(glLinkProgram);
  FIND_PROC(glPixelStorei);
  FIND_PROC(glPolygonOffset);
  FIND_PROC(glReadPixels);
  FIND_PROC(glReleaseShaderCompiler);
  FIND_PROC(glRenderbufferStorage);
  FIND_PROC(glSampleCoverage);
  FIND_PROC(glScissor);
  FIND_PROC(glShaderBinary);
  FIND_PROC(glShaderSource);
  FIND_PROC(glStencilFunc);
  FIND_PROC(glStencilFuncSeparate);
  FIND_PROC(glStencilMask);
  FIND_PROC(glStencilMaskSeparate);
  FIND_PROC(glStencilOp);
  FIND_PROC(glStencilOpSeparate);
  FIND_PROC(glTexImage2D);
  FIND_PROC(glTexParameterf);
  FIND_PROC(glTexParameterfv);
  FIND_PROC(glTexParameteri);
  FIND_PROC(glTexParameteriv);
  FIND_PROC(glTexSubImage2D);
  FIND_PROC(glUniform1f);
  FIND_PROC(glUniform1fv);
  FIND_PROC(glUniform1i);
  FIND_PROC(glUniform1iv);
  FIND_PROC(glUniform2f);
  FIND_PROC(glUniform2fv);
  FIND_PROC(glUniform2i);
  FIND_PROC(glUniform2iv);
  FIND_PROC(glUniform3f);
  FIND_PROC(glUniform3fv);
  FIND_PROC(glUniform3i);
  FIND_PROC(glUniform3iv);
  FIND_PROC(glUniform4f);
  FIND_PROC(glUniform4fv);
  FIND_PROC(glUniform4i);
  FIND_PROC(glUniform4iv);
  FIND_PROC(glUniformMatrix2fv);
  FIND_PROC(glUniformMatrix3fv);
  FIND_PROC(glUniformMatrix4fv);
  FIND_PROC(glUseProgram);
  FIND_PROC(glValidateProgram);
  FIND_PROC(glVertexAttrib1f);
  FIND_PROC(glVertexAttrib1fv);
  FIND_PROC(glVertexAttrib2f);
  FIND_PROC(glVertexAttrib2fv);
  FIND_PROC(glVertexAttrib3f);
  FIND_PROC(glVertexAttrib3fv);
  FIND_PROC(glVertexAttrib4f);
  FIND_PROC(glVertexAttrib4fv);
  FIND_PROC(glVertexAttribPointer);
  FIND_PROC(glViewport);  
  #undef FIND_PROC

  if (!glActiveTexture ||
      !glAttachShader ||
      !glBindAttribLocation ||
      !glBindBuffer ||
      !glBindFramebuffer ||
      !glBindRenderbuffer ||
      !glBindTexture ||
      !glBlendColor ||
      !glBlendEquation ||
      !glBlendEquationSeparate ||
      !glBlendFunc ||
      !glBlendFuncSeparate ||
      !glBufferData ||
      !glBufferSubData ||
      !glCheckFramebufferStatus ||
      !glClear ||
      !glClearColor ||
      !glClearDepthf ||
      !glClearStencil ||
      !glColorMask ||
      !glCompileShader ||
      !glCompressedTexImage2D ||
      !glCompressedTexSubImage2D ||
      !glCopyTexImage2D ||
      !glCopyTexSubImage2D ||
      !glCreateProgram ||
      !glCreateShader ||
      !glCullFace ||
      !glDeleteBuffers ||
      !glDeleteFramebuffers ||
      !glDeleteProgram ||
      !glDeleteRenderbuffers ||
      !glDeleteShader ||
      !glDeleteTextures ||
      !glDepthFunc ||
      !glDepthMask ||
      !glDepthRangef ||
      !glDetachShader ||
      !glDisable ||
      !glDisableVertexAttribArray ||
      !glDrawArrays ||
      !glDrawElements ||
      !glEnable ||
      !glEnableVertexAttribArray ||
      !glFinish ||
      !glFlush ||
      !glFramebufferRenderbuffer ||
      !glFramebufferTexture2D ||
      !glFrontFace ||
      !glGenBuffers ||
      !glGenerateMipmap ||
      !glGenFramebuffers ||
      !glGenRenderbuffers ||
      !glGenTextures ||
      !glGetActiveAttrib ||
      !glGetActiveUniform ||
      !glGetAttachedShaders ||
      !glGetAttribLocation ||
      !glGetBooleanv ||
      !glGetBufferParameteriv ||
      !glGetError ||
      !glGetFloatv ||
      !glGetFramebufferAttachmentParameteriv ||
      !glGetIntegerv ||
      !glGetProgramiv ||
      !glGetProgramInfoLog ||
      !glGetRenderbufferParameteriv ||
      !glGetShaderiv ||
      !glGetShaderInfoLog ||
      !glGetShaderPrecisionFormat ||
      !glGetShaderSource ||
      !glGetString ||
      !glGetTexParameterfv ||
      !glGetTexParameteriv ||
      !glGetUniformfv ||
      !glGetUniformiv ||
      !glGetUniformLocation ||
      !glGetVertexAttribfv ||
      !glGetVertexAttribiv ||
      !glGetVertexAttribPointerv ||
      !glHint ||
      !glIsBuffer ||
      !glIsEnabled ||
      !glIsFramebuffer ||
      !glIsProgram ||
      !glIsRenderbuffer ||
      !glIsShader ||
      !glIsTexture ||
      !glLineWidth ||
      !glLinkProgram ||
      !glPixelStorei ||
      !glPolygonOffset ||
      !glReadPixels ||
      !glReleaseShaderCompiler ||
      !glRenderbufferStorage ||
      !glSampleCoverage ||
      !glScissor ||
      !glShaderBinary ||
      !glShaderSource ||
      !glStencilFunc ||
      !glStencilFuncSeparate ||
      !glStencilMask ||
      !glStencilMaskSeparate ||
      !glStencilOp ||
      !glStencilOpSeparate ||
      !glTexImage2D ||
      !glTexParameterf ||
      !glTexParameterfv ||
      !glTexParameteri ||
      !glTexParameteriv ||
      !glTexSubImage2D ||
      !glUniform1f ||
      !glUniform1fv ||
      !glUniform1i ||
      !glUniform1iv ||
      !glUniform2f ||
      !glUniform2fv ||
      !glUniform2i ||
      !glUniform2iv ||
      !glUniform3f ||
      !glUniform3fv ||
      !glUniform3i ||
      !glUniform3iv ||
      !glUniform4f ||
      !glUniform4fv ||
      !glUniform4i ||
      !glUniform4iv ||
      !glUniformMatrix2fv ||
      !glUniformMatrix3fv ||
      !glUniformMatrix4fv ||
      !glUseProgram ||
      !glValidateProgram ||
      !glVertexAttrib1f ||
      !glVertexAttrib1fv ||
      !glVertexAttrib2f ||
      !glVertexAttrib2fv ||
      !glVertexAttrib3f ||
      !glVertexAttrib3fv ||
      !glVertexAttrib4f ||
      !glVertexAttrib4fv ||
      !glVertexAttribPointer ||
      !glViewport)
  {
    return GL_FALSE;
  }

  return GL_TRUE;
}

#pragma GCC diagnostic pop

/* Function pointer definitions */
GL_APICALL void (*GL_APIENTRY glActiveTexture) (GLenum texture);
GL_APICALL void (*GL_APIENTRY glAttachShader) (GLuint program, GLuint shader);
GL_APICALL void (*GL_APIENTRY glBindAttribLocation) (GLuint program, GLuint index, const GLchar *name);
GL_APICALL void (*GL_APIENTRY glBindBuffer) (GLenum target, GLuint buffer);
GL_APICALL void (*GL_APIENTRY glBindFramebuffer) (GLenum target, GLuint framebuffer);
GL_APICALL void (*GL_APIENTRY glBindRenderbuffer) (GLenum target, GLuint renderbuffer);
GL_APICALL void (*GL_APIENTRY glBindTexture) (GLenum target, GLuint texture);
GL_APICALL void (*GL_APIENTRY glBlendColor) (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
GL_APICALL void (*GL_APIENTRY glBlendEquation) (GLenum mode);
GL_APICALL void (*GL_APIENTRY glBlendEquationSeparate) (GLenum modeRGB, GLenum modeAlpha);
GL_APICALL void (*GL_APIENTRY glBlendFunc) (GLenum sfactor, GLenum dfactor);
GL_APICALL void (*GL_APIENTRY glBlendFuncSeparate) (GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
GL_APICALL void (*GL_APIENTRY glBufferData) (GLenum target, GLsizeiptr size, const void *data, GLenum usage);
GL_APICALL void (*GL_APIENTRY glBufferSubData) (GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
GL_APICALL GLenum (*GL_APIENTRY glCheckFramebufferStatus) (GLenum target);
GL_APICALL void (*GL_APIENTRY glClear) (GLbitfield mask);
GL_APICALL void (*GL_APIENTRY glClearColor) (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
GL_APICALL void (*GL_APIENTRY glClearDepthf) (GLfloat d);
GL_APICALL void (*GL_APIENTRY glClearStencil) (GLint s);
GL_APICALL void (*GL_APIENTRY glColorMask) (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
GL_APICALL void (*GL_APIENTRY glCompileShader) (GLuint shader);
GL_APICALL void (*GL_APIENTRY glCompressedTexImage2D) (GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
GL_APICALL void (*GL_APIENTRY glCompressedTexSubImage2D) (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data);
GL_APICALL void (*GL_APIENTRY glCopyTexImage2D) (GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
GL_APICALL void (*GL_APIENTRY glCopyTexSubImage2D) (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
GL_APICALL GLuint (*GL_APIENTRY glCreateProgram) (void);
GL_APICALL GLuint (*GL_APIENTRY glCreateShader) (GLenum type);
GL_APICALL void (*GL_APIENTRY glCullFace) (GLenum mode);
GL_APICALL void (*GL_APIENTRY glDeleteBuffers) (GLsizei n, const GLuint *buffers);
GL_APICALL void (*GL_APIENTRY glDeleteFramebuffers) (GLsizei n, const GLuint *framebuffers);
GL_APICALL void (*GL_APIENTRY glDeleteProgram) (GLuint program);
GL_APICALL void (*GL_APIENTRY glDeleteRenderbuffers) (GLsizei n, const GLuint *renderbuffers);
GL_APICALL void (*GL_APIENTRY glDeleteShader) (GLuint shader);
GL_APICALL void (*GL_APIENTRY glDeleteTextures) (GLsizei n, const GLuint *textures);
GL_APICALL void (*GL_APIENTRY glDepthFunc) (GLenum func);
GL_APICALL void (*GL_APIENTRY glDepthMask) (GLboolean flag);
GL_APICALL void (*GL_APIENTRY glDepthRangef) (GLfloat n, GLfloat f);
GL_APICALL void (*GL_APIENTRY glDetachShader) (GLuint program, GLuint shader);
GL_APICALL void (*GL_APIENTRY glDisable) (GLenum cap);
GL_APICALL void (*GL_APIENTRY glDisableVertexAttribArray) (GLuint index);
GL_APICALL void (*GL_APIENTRY glDrawArrays) (GLenum mode, GLint first, GLsizei count);
GL_APICALL void (*GL_APIENTRY glDrawElements) (GLenum mode, GLsizei count, GLenum type, const void *indices);
GL_APICALL void (*GL_APIENTRY glEnable) (GLenum cap);
GL_APICALL void (*GL_APIENTRY glEnableVertexAttribArray) (GLuint index);
GL_APICALL void (*GL_APIENTRY glFinish) (void);
GL_APICALL void (*GL_APIENTRY glFlush) (void);
GL_APICALL void (*GL_APIENTRY glFramebufferRenderbuffer) (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
GL_APICALL void (*GL_APIENTRY glFramebufferTexture2D) (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
GL_APICALL void (*GL_APIENTRY glFrontFace) (GLenum mode);
GL_APICALL void (*GL_APIENTRY glGenBuffers) (GLsizei n, GLuint *buffers);
GL_APICALL void (*GL_APIENTRY glGenerateMipmap) (GLenum target);
GL_APICALL void (*GL_APIENTRY glGenFramebuffers) (GLsizei n, GLuint *framebuffers);
GL_APICALL void (*GL_APIENTRY glGenRenderbuffers) (GLsizei n, GLuint *renderbuffers);
GL_APICALL void (*GL_APIENTRY glGenTextures) (GLsizei n, GLuint *textures);
GL_APICALL void (*GL_APIENTRY glGetActiveAttrib) (GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
GL_APICALL void (*GL_APIENTRY glGetActiveUniform) (GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
GL_APICALL void (*GL_APIENTRY glGetAttachedShaders) (GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders);
GL_APICALL GLint (*GL_APIENTRY glGetAttribLocation) (GLuint program, const GLchar *name);
GL_APICALL void (*GL_APIENTRY glGetBooleanv) (GLenum pname, GLboolean *data);
GL_APICALL void (*GL_APIENTRY glGetBufferParameteriv) (GLenum target, GLenum pname, GLint *params);
GL_APICALL GLenum (*GL_APIENTRY glGetError) (void);
GL_APICALL void (*GL_APIENTRY glGetFloatv) (GLenum pname, GLfloat *data);
GL_APICALL void (*GL_APIENTRY glGetFramebufferAttachmentParameteriv) (GLenum target, GLenum attachment, GLenum pname, GLint *params);
GL_APICALL void (*GL_APIENTRY glGetIntegerv) (GLenum pname, GLint *data);
GL_APICALL void (*GL_APIENTRY glGetProgramiv) (GLuint program, GLenum pname, GLint *params);
GL_APICALL void (*GL_APIENTRY glGetProgramInfoLog) (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
GL_APICALL void (*GL_APIENTRY glGetRenderbufferParameteriv) (GLenum target, GLenum pname, GLint *params);
GL_APICALL void (*GL_APIENTRY glGetShaderiv) (GLuint shader, GLenum pname, GLint *params);
GL_APICALL void (*GL_APIENTRY glGetShaderInfoLog) (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
GL_APICALL void (*GL_APIENTRY glGetShaderPrecisionFormat) (GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision);
GL_APICALL void (*GL_APIENTRY glGetShaderSource) (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source);
GL_APICALL const GLubyte *(*GL_APIENTRY glGetString) (GLenum name);
GL_APICALL void (*GL_APIENTRY glGetTexParameterfv) (GLenum target, GLenum pname, GLfloat *params);
GL_APICALL void (*GL_APIENTRY glGetTexParameteriv) (GLenum target, GLenum pname, GLint *params);
GL_APICALL void (*GL_APIENTRY glGetUniformfv) (GLuint program, GLint location, GLfloat *params);
GL_APICALL void (*GL_APIENTRY glGetUniformiv) (GLuint program, GLint location, GLint *params);
GL_APICALL GLint (*GL_APIENTRY glGetUniformLocation) (GLuint program, const GLchar *name);
GL_APICALL void (*GL_APIENTRY glGetVertexAttribfv) (GLuint index, GLenum pname, GLfloat *params);
GL_APICALL void (*GL_APIENTRY glGetVertexAttribiv) (GLuint index, GLenum pname, GLint *params);
GL_APICALL void (*GL_APIENTRY glGetVertexAttribPointerv) (GLuint index, GLenum pname, void **pointer);
GL_APICALL void (*GL_APIENTRY glHint) (GLenum target, GLenum mode);
GL_APICALL GLboolean (*GL_APIENTRY glIsBuffer) (GLuint buffer);
GL_APICALL GLboolean (*GL_APIENTRY glIsEnabled) (GLenum cap);
GL_APICALL GLboolean (*GL_APIENTRY glIsFramebuffer) (GLuint framebuffer);
GL_APICALL GLboolean (*GL_APIENTRY glIsProgram) (GLuint program);
GL_APICALL GLboolean (*GL_APIENTRY glIsRenderbuffer) (GLuint renderbuffer);
GL_APICALL GLboolean (*GL_APIENTRY glIsShader) (GLuint shader);
GL_APICALL GLboolean (*GL_APIENTRY glIsTexture) (GLuint texture);
GL_APICALL void (*GL_APIENTRY glLineWidth) (GLfloat width);
GL_APICALL void (*GL_APIENTRY glLinkProgram) (GLuint program);
GL_APICALL void (*GL_APIENTRY glPixelStorei) (GLenum pname, GLint param);
GL_APICALL void (*GL_APIENTRY glPolygonOffset) (GLfloat factor, GLfloat units);
GL_APICALL void (*GL_APIENTRY glReadPixels) (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
GL_APICALL void (*GL_APIENTRY glReleaseShaderCompiler) (void);
GL_APICALL void (*GL_APIENTRY glRenderbufferStorage) (GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
GL_APICALL void (*GL_APIENTRY glSampleCoverage) (GLfloat value, GLboolean invert);
GL_APICALL void (*GL_APIENTRY glScissor) (GLint x, GLint y, GLsizei width, GLsizei height);
GL_APICALL void (*GL_APIENTRY glShaderBinary) (GLsizei count, const GLuint *shaders, GLenum binaryformat, const void *binary, GLsizei length);
GL_APICALL void (*GL_APIENTRY glShaderSource) (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
GL_APICALL void (*GL_APIENTRY glStencilFunc) (GLenum func, GLint ref, GLuint mask);
GL_APICALL void (*GL_APIENTRY glStencilFuncSeparate) (GLenum face, GLenum func, GLint ref, GLuint mask);
GL_APICALL void (*GL_APIENTRY glStencilMask) (GLuint mask);
GL_APICALL void (*GL_APIENTRY glStencilMaskSeparate) (GLenum face, GLuint mask);
GL_APICALL void (*GL_APIENTRY glStencilOp) (GLenum fail, GLenum zfail, GLenum zpass);
GL_APICALL void (*GL_APIENTRY glStencilOpSeparate) (GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
GL_APICALL void (*GL_APIENTRY glTexImage2D) (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
GL_APICALL void (*GL_APIENTRY glTexParameterf) (GLenum target, GLenum pname, GLfloat param);
GL_APICALL void (*GL_APIENTRY glTexParameterfv) (GLenum target, GLenum pname, const GLfloat *params);
GL_APICALL void (*GL_APIENTRY glTexParameteri) (GLenum target, GLenum pname, GLint param);
GL_APICALL void (*GL_APIENTRY glTexParameteriv) (GLenum target, GLenum pname, const GLint *params);
GL_APICALL void (*GL_APIENTRY glTexSubImage2D) (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
GL_APICALL void (*GL_APIENTRY glUniform1f) (GLint location, GLfloat v0);
GL_APICALL void (*GL_APIENTRY glUniform1fv) (GLint location, GLsizei count, const GLfloat *value);
GL_APICALL void (*GL_APIENTRY glUniform1i) (GLint location, GLint v0);
GL_APICALL void (*GL_APIENTRY glUniform1iv) (GLint location, GLsizei count, const GLint *value);
GL_APICALL void (*GL_APIENTRY glUniform2f) (GLint location, GLfloat v0, GLfloat v1);
GL_APICALL void (*GL_APIENTRY glUniform2fv) (GLint location, GLsizei count, const GLfloat *value);
GL_APICALL void (*GL_APIENTRY glUniform2i) (GLint location, GLint v0, GLint v1);
GL_APICALL void (*GL_APIENTRY glUniform2iv) (GLint location, GLsizei count, const GLint *value);
GL_APICALL void (*GL_APIENTRY glUniform3f) (GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
GL_APICALL void (*GL_APIENTRY glUniform3fv) (GLint location, GLsizei count, const GLfloat *value);
GL_APICALL void (*GL_APIENTRY glUniform3i) (GLint location, GLint v0, GLint v1, GLint v2);
GL_APICALL void (*GL_APIENTRY glUniform3iv) (GLint location, GLsizei count, const GLint *value);
GL_APICALL void (*GL_APIENTRY glUniform4f) (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
GL_APICALL void (*GL_APIENTRY glUniform4fv) (GLint location, GLsizei count, const GLfloat *value);
GL_APICALL void (*GL_APIENTRY glUniform4i) (GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
GL_APICALL void (*GL_APIENTRY glUniform4iv) (GLint location, GLsizei count, const GLint *value);
GL_APICALL void (*GL_APIENTRY glUniformMatrix2fv) (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GL_APICALL void (*GL_APIENTRY glUniformMatrix3fv) (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GL_APICALL void (*GL_APIENTRY glUniformMatrix4fv) (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GL_APICALL void (*GL_APIENTRY glUseProgram) (GLuint program);
GL_APICALL void (*GL_APIENTRY glValidateProgram) (GLuint program);
GL_APICALL void (*GL_APIENTRY glVertexAttrib1f) (GLuint index, GLfloat x);
GL_APICALL void (*GL_APIENTRY glVertexAttrib1fv) (GLuint index, const GLfloat *v);
GL_APICALL void (*GL_APIENTRY glVertexAttrib2f) (GLuint index, GLfloat x, GLfloat y);
GL_APICALL void (*GL_APIENTRY glVertexAttrib2fv) (GLuint index, const GLfloat *v);
GL_APICALL void (*GL_APIENTRY glVertexAttrib3f) (GLuint index, GLfloat x, GLfloat y, GLfloat z);
GL_APICALL void (*GL_APIENTRY glVertexAttrib3fv) (GLuint index, const GLfloat *v);
GL_APICALL void (*GL_APIENTRY glVertexAttrib4f) (GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
GL_APICALL void (*GL_APIENTRY glVertexAttrib4fv) (GLuint index, const GLfloat *v);
GL_APICALL void (*GL_APIENTRY glVertexAttribPointer) (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
GL_APICALL void (*GL_APIENTRY glViewport) (GLint x, GLint y, GLsizei width, GLsizei height);
