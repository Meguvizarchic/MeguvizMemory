#ifndef MGMEM_FAKE_GL3_H
#define MGMEM_FAKE_GL3_H
#include <stdint.h>
typedef uint32_t GLenum;
typedef uint32_t GLuint;
typedef int32_t GLint;
typedef int32_t GLsizei;
typedef uint8_t GLubyte;
#define GL_NO_ERROR 0
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY 0x0505
#define GL_CONTEXT_LOST 0x0507
#define GL_MAJOR_VERSION 0x821B
#define GL_EXTENSIONS 0x1F03
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
extern "C" GLenum glGetError();
extern "C" void glGetIntegerv(GLenum, GLint *);
extern "C" const GLubyte *glGetString(GLenum);
extern "C" void glGenTextures(GLsizei, GLuint *);
extern "C" void glBindTexture(GLenum, GLuint);
extern "C" void glActiveTexture(GLenum);
extern "C" void glBindBuffer(GLenum, GLuint);
extern "C" void glPixelStorei(GLenum, GLint);
extern "C" void glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
extern "C" void glFlush();
extern "C" void glDeleteTextures(GLsizei, const GLuint *);
#endif
