#include "../src/platform.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

static EGLDisplay display = reinterpret_cast<EGLDisplay>(1);
static EGLContext context = reinterpret_cast<EGLContext>(2);
static EGLImageKHR image = reinterpret_cast<EGLImageKHR>(3);
static EGLSyncKHR sync = reinterpret_cast<EGLSyncKHR>(4);
static EGLint egl_error = EGL_SUCCESS;
static thread_local uint64_t fake_thread_token = 1;
extern "C" uint64_t mgmem_ahb_test_thread_token() noexcept { return fake_thread_token; }
static bool wrong_context = false, missing_extension = false;
static std::vector<GLenum> errors;
static std::string events;
static GLint active = 0x84C3, texture = 77, unpack_buffer = 66, alignment = 8, row_length = 9, skip_rows = 3, skip_pixels = 2;
static int pixel_store_calls = 0;
static unsigned char pixels[64]{};
enum class Mode { normal, oom_after_error, lost_after_error, restore_error, dup_error, delete_error, context_after_target, setup_oom };
static Mode mode = Mode::normal;

extern "C" EGLDisplay eglGetCurrentDisplay() { return display; }
extern "C" EGLContext eglGetCurrentContext() { return wrong_context ? nullptr : context; }
extern "C" EGLenum eglQueryAPI() { return EGL_OPENGL_ES_API; }
extern "C" EGLint eglGetError() { auto result = egl_error; egl_error = EGL_SUCCESS; return result; }
extern "C" const char *eglQueryString(EGLDisplay d, EGLint which) {
    CHECK(d == display && which == EGL_EXTENSIONS);
    return missing_extension ? "EGL_KHR_image_base" :
        "EGL_KHR_image_base EGL_ANDROID_image_native_buffer EGL_ANDROID_get_native_client_buffer "
        "EGL_KHR_fence_sync EGL_ANDROID_native_fence_sync";
}
extern "C" GLenum glGetError() {
    if (errors.empty()) return GL_NO_ERROR;
    auto e = errors.front(); errors.erase(errors.begin()); return e;
}
extern "C" void glGetIntegerv(GLenum name, GLint *v) {
    switch (name) {
    case GL_MAJOR_VERSION: *v = 3; break;
    case GL_ACTIVE_TEXTURE: *v = active; break;
    case GL_TEXTURE_BINDING_2D: *v = texture; break;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: *v = unpack_buffer; break;
    case GL_UNPACK_ALIGNMENT: *v = alignment; break;
    case GL_UNPACK_ROW_LENGTH: *v = row_length; break;
    case GL_UNPACK_SKIP_ROWS: *v = skip_rows; break;
    case GL_UNPACK_SKIP_PIXELS: *v = skip_pixels; break;
    default: CHECK(false);
    }
}
extern "C" const GLubyte *glGetString(GLenum name) {
    CHECK(name == GL_EXTENSIONS); return reinterpret_cast<const GLubyte *>("GL_OES_EGL_image");
}
extern "C" void glGenTextures(GLsizei n, GLuint *out) { CHECK(n == 1); *out = 99; events += 'G'; }
extern "C" void glBindTexture(GLenum type, GLuint value) {
    CHECK(type == GL_TEXTURE_2D); texture = static_cast<GLint>(value); events += value == 77 ? 'r' : 'b';
    if (value == 77 && mode == Mode::restore_error) errors.push_back(GL_INVALID_OPERATION);
}
extern "C" void glBindBuffer(GLenum type, GLuint value) {
    CHECK(type == GL_PIXEL_UNPACK_BUFFER); unpack_buffer = static_cast<GLint>(value);
    if (!value && mode == Mode::setup_oom) errors.push_back(GL_OUT_OF_MEMORY);
}
extern "C" void glActiveTexture(GLenum value) { active = static_cast<GLint>(value); }
extern "C" void glPixelStorei(GLenum name, GLint value) {
    ++pixel_store_calls;
    switch (name) {
    case GL_UNPACK_ALIGNMENT: alignment = value; break;
    case GL_UNPACK_ROW_LENGTH: row_length = value; break;
    case GL_UNPACK_SKIP_ROWS: skip_rows = value; break;
    case GL_UNPACK_SKIP_PIXELS: skip_pixels = value; break;
    default: CHECK(false);
    }
}
extern "C" void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
    GLsizei width, GLsizei height, GLenum format, GLenum type, const void *data) {
    CHECK(target == GL_TEXTURE_2D && !level && !x && !y && width == 4 && height == 4);
    CHECK(format == GL_RGBA && type == GL_UNSIGNED_BYTE && texture == 99);
    CHECK(!unpack_buffer && alignment == 1 && row_length == 4 && !skip_rows && !skip_pixels);
    CHECK(data); std::memcpy(pixels, data, sizeof(pixels)); events += 'U';
    if (mode == Mode::oom_after_error) { errors.push_back(GL_INVALID_OPERATION); errors.push_back(GL_OUT_OF_MEMORY); }
    if (mode == Mode::lost_after_error) { errors.push_back(GL_INVALID_OPERATION); errors.push_back(GL_CONTEXT_LOST); }
}
extern "C" void glFlush() { events += 'L'; }
extern "C" void glDeleteTextures(GLsizei n, const GLuint *v) {
    CHECK(n == 1 && *v == 99); events += 'd';
    if (mode == Mode::delete_error) errors.push_back(GL_INVALID_OPERATION);
}
static EGLClientBuffer client(const AHardwareBuffer *buffer) {
    CHECK(buffer == reinterpret_cast<AHardwareBuffer *>(5)); events += 'N'; return reinterpret_cast<EGLClientBuffer>(6);
}
static EGLImageKHR create_image(EGLDisplay d, EGLContext c, EGLenum target, EGLClientBuffer buffer, const EGLint *attrs) {
    CHECK(d == display && c == EGL_NO_CONTEXT && target == EGL_NATIVE_BUFFER_ANDROID && buffer == reinterpret_cast<EGLClientBuffer>(6));
    CHECK(attrs && attrs[0] == EGL_IMAGE_PRESERVED_KHR && attrs[1] == EGL_TRUE && attrs[2] == EGL_NONE);
    events += 'I'; return image;
}
static EGLBoolean destroy_image(EGLDisplay d, EGLImageKHR i) { CHECK(d == display && i == image); events += 'i'; return EGL_TRUE; }
static void target_image(GLenum target, GLeglImageOES value) {
    CHECK(target == GL_TEXTURE_2D && value == image); events += 'T';
    if (mode == Mode::context_after_target) wrong_context = true;
}
static EGLSyncKHR create_sync(EGLDisplay d, EGLenum kind, const EGLint *attrs) {
    CHECK(d == display && kind == EGL_SYNC_NATIVE_FENCE_ANDROID && !attrs); events += 'F'; return sync;
}
static EGLBoolean destroy_sync(EGLDisplay d, EGLSyncKHR s) { CHECK(d == display && s == sync); events += 'f'; return EGL_TRUE; }
static EGLint dup_fence(EGLDisplay d, EGLSyncKHR s) {
    CHECK(d == display && s == sync); events += 'P';
    if (mode == Mode::dup_error) egl_error = EGL_BAD_ALLOC;
    return 20;
}
extern "C" __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
#define RETURN(name_, fn) if (!std::strcmp(name, name_)) return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(fn)
    RETURN("eglGetNativeClientBufferANDROID", client);
    RETURN("eglCreateImageKHR", create_image);
    RETURN("eglDestroyImageKHR", destroy_image);
    RETURN("glEGLImageTargetTexture2DOES", target_image);
    RETURN("eglCreateSyncKHR", create_sync);
    RETURN("eglDestroySyncKHR", destroy_sync);
    RETURN("eglDupNativeFenceFDANDROID", dup_fence);
#undef RETURN
    return nullptr;
}
static mgmem_upload_platform *open() {
    mgmem_upload_platform *p = nullptr; int32_t detail = -1;
    CHECK(mgmem_upload_platform_open(&p, &detail) == MGMEM_OK && p); return p;
}
static mgmem_gpu_ahb_view view(unsigned char *data) {
    return {data, 64, reinterpret_cast<AHardwareBuffer *>(5), 32, 4, 4, -1};
}
static void initial_state() {
    CHECK(active == 0x84C3 && texture == 77 && unpack_buffer == 66 && alignment == 8 && row_length == 9 && skip_rows == 3 && skip_pixels == 2);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    unsigned char data[64]; for (int i = 0; i < 64; ++i) data[i] = static_cast<unsigned char>(i);
    int32_t detail = 0;
    if (!std::strcmp(argv[1], "unsupported")) {
        missing_extension = true; mgmem_upload_platform *p = nullptr;
        CHECK(mgmem_upload_platform_open(&p, &detail) == MGMEM_UNSUPPORTED && !p && events.empty());
        std::puts("PASS capability failure before target use"); return 0;
    }
    if (!std::strcmp(argv[1], "token")) {
        fake_thread_token = 0; mgmem_upload_platform *invalid = nullptr;
        CHECK(mgmem_upload_platform_open(&invalid, &detail) == MGMEM_BUSY && !invalid && events.empty());
        fake_thread_token = 1;
        auto *live = open();
        CHECK(mgmem_upload_platform_write(live, view(data), 16, &detail) == MGMEM_OK);
        int producer = -1;
        CHECK(mgmem_upload_platform_fence(live, &producer, &detail) == MGMEM_OK && producer == 20);
        fake_thread_token = 2; /* Model a reused std::thread::id with the same EGL handles. */
        CHECK(mgmem_upload_platform_cleanup(live, &detail) == MGMEM_BUSY && events == "NIGbTUrFLP");
        fake_thread_token = 1;
        CHECK(mgmem_upload_platform_cleanup(live, &detail) == MGMEM_OK && events == "NIGbTUrFLPdif");
        mgmem_upload_platform_discard(live);
        std::puts("PASS thread token exhaustion and reused identifier fail closed"); return 0;
    }
    auto *p = open();
    if (!std::strcmp(argv[1], "switch")) {
        mode = Mode::context_after_target;
        CHECK(mgmem_upload_platform_write(p, view(data), 16, &detail) == MGMEM_PROVIDER_ERROR);
        CHECK(events.find('T') != std::string::npos && events.find('U') == std::string::npos && events.find('r') == std::string::npos);
        std::puts("PASS context switch stops before upload and restoration"); return 0;
    }
    if (!std::strcmp(argv[1], "setup")) {
        mode = Mode::setup_oom;
        CHECK(mgmem_upload_platform_write(p, view(data), 16, &detail) == MGMEM_PROVIDER_ERROR);
        CHECK(detail == GL_OUT_OF_MEMORY && pixel_store_calls == 0);
        CHECK(events.find('T') == std::string::npos && events.find('U') == std::string::npos);
        std::puts("PASS fatal setup error stops subsequent state mutations"); return 0;
    }
    if (!std::strcmp(argv[1], "oom") || !std::strcmp(argv[1], "lost")) {
        mode = !std::strcmp(argv[1], "oom") ? Mode::oom_after_error : Mode::lost_after_error;
        CHECK(mgmem_upload_platform_write(p, view(data), 16, &detail) == MGMEM_PROVIDER_ERROR);
        CHECK(detail == (mode == Mode::oom_after_error ? GL_OUT_OF_MEMORY : GL_CONTEXT_LOST));
        CHECK(events.find('U') != std::string::npos && events.find('r') == std::string::npos);
        std::puts("PASS fatal GL error never attempts restoration"); return 0;
    }
    if (!std::strcmp(argv[1], "restore")) {
        mode = Mode::restore_error;
        CHECK(mgmem_upload_platform_write(p, view(data), 16, &detail) == MGMEM_PROVIDER_ERROR);
        CHECK(events.find('U') != std::string::npos && events.find('r') != std::string::npos);
        std::puts("PASS restoration failure is reported"); return 0;
    }
    CHECK(mgmem_upload_platform_write(p, view(data), 16, &detail) == MGMEM_OK);
    CHECK(events == "NIGbTUr" && !std::memcmp(pixels, data, 64)); initial_state();
    int fd = -1;
    if (!std::strcmp(argv[1], "dup")) {
        mode = Mode::dup_error;
        CHECK(mgmem_upload_platform_fence(p, &fd, &detail) == MGMEM_PROVIDER_ERROR);
        CHECK(fd == 20 && events == "NIGbTUrFLP");
        std::puts("PASS fence FD survives later EGL error"); return 0;
    }
    CHECK(mgmem_upload_platform_fence(p, &fd, &detail) == MGMEM_OK && fd == 20);
    CHECK(events == "NIGbTUrFLP");
    mgmem_status other_thread = MGMEM_OK;
    std::thread thread([&] { int32_t thread_detail = 0; other_thread = mgmem_upload_platform_cleanup(p, &thread_detail); });
    thread.join();
    CHECK(other_thread == MGMEM_BUSY && events == "NIGbTUrFLP");
    wrong_context = true;
    CHECK(mgmem_upload_platform_cleanup(p, &detail) == MGMEM_BUSY && events == "NIGbTUrFLP");
    wrong_context = false;
    if (!std::strcmp(argv[1], "delete")) {
        mode = Mode::delete_error;
        CHECK(mgmem_upload_platform_cleanup(p, &detail) == MGMEM_PROVIDER_ERROR && events == "NIGbTUrFLPd");
        std::puts("PASS uncertain partial cleanup is reported"); return 0;
    }
    CHECK(!std::strcmp(argv[1], "success"));
    CHECK(mgmem_upload_platform_cleanup(p, &detail) == MGMEM_OK && events == "NIGbTUrFLPdif");
    mgmem_upload_platform_discard(p);
    std::puts("PASS upload, state restoration and ordered cleanup");
}
