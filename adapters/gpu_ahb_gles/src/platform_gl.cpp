#include "platform.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <thread>

struct mgmem_upload_platform {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    std::thread::id thread;
    uint64_t thread_token = 0;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC client = nullptr;
    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_image = nullptr;
    PFNEGLCREATESYNCKHRPROC create_sync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    EGLSyncKHR fence = EGL_NO_SYNC_KHR;
    GLuint texture = 0;
};

namespace {
#if defined(MGMEM_AHB_GLES_TESTING)
extern "C" uint64_t mgmem_ahb_test_thread_token() noexcept;
uint64_t thread_token() noexcept { return mgmem_ahb_test_thread_token(); }
#else
/* Thread identifiers may be reused after a thread exits. Each thread obtains
 * a never-reused token for this linked image. Exhaustion/contention fails
 * closed; it cannot authorize cleanup on another thread. */
uint64_t allocate_thread_token() noexcept {
    static std::atomic<uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        if (!value || value == std::numeric_limits<uint64_t>::max()) return 0;
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
    return 0;
}
uint64_t thread_token() noexcept {
    thread_local uint64_t token = 0;
    if (!token) token = allocate_thread_token();
    return token;
}
#endif
bool has_extension(const char *extensions, const char *name) noexcept {
    if (!extensions || !name || !*name || std::strchr(name, ' ')) return false;
    const auto n = std::strlen(name);
    for (const char *p = extensions; (p = std::strstr(p, name)); p += n)
        if ((p == extensions || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return true;
    return false;
}
bool current(const mgmem_upload_platform *p) noexcept {
    return p && p->thread_token && p->thread_token == thread_token() && p->thread == std::this_thread::get_id() &&
        eglGetCurrentDisplay() == p->display && eglGetCurrentContext() == p->context;
}
/* Error queries consume pre-existing flags. Bound the drain to avoid spinning
 * forever on a lost or broken context. */
bool gl_clean(int32_t *detail) noexcept {
    GLenum first = GL_NO_ERROR;
    for (unsigned i = 0; i < 16; ++i) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) { if (first && detail) *detail = static_cast<int32_t>(first); return first == GL_NO_ERROR; }
        if (error == GL_OUT_OF_MEMORY || error == 0x0507) {
            if (detail) *detail = static_cast<int32_t>(error);
            return false;
        }
        if (!first) first = error;
    }
    if (detail) *detail = -1; /* error queue did not drain: state is uncertain */
    return false;
}
bool egl_clean(int32_t *detail) noexcept {
    const EGLint status = eglGetError();
    if (status == EGL_SUCCESS) return true;
    if (detail) *detail = status;
    return false;
}
bool fatal_gl(int32_t status) noexcept {
    return status == -1 || status == GL_OUT_OF_MEMORY || status == 0x0507; /* GL_CONTEXT_LOST */
}
struct State {
    GLint texture = 0, unpack_buffer = 0, alignment = 0, row_length = 0, skip_rows = 0, skip_pixels = 0;
    bool snapshot(const mgmem_upload_platform *p, int32_t *detail) noexcept {
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        if (!gl_clean(detail) || !current(p)) return false;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
        if (!gl_clean(detail) || !current(p)) return false;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        if (!gl_clean(detail) || !current(p)) return false;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
        if (!gl_clean(detail) || !current(p)) return false;
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
        if (!gl_clean(detail) || !current(p)) return false;
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
        return gl_clean(detail) && current(p);
    }
    bool set_pixel_store(const mgmem_upload_platform *p, const State &to, int32_t *detail) const noexcept {
        const auto set = [&](GLenum name, GLint from, GLint value) {
            if (from == value) return true;
            glPixelStorei(name, value);
            return gl_clean(detail) && current(p);
        };
        return set(GL_UNPACK_ALIGNMENT, alignment, to.alignment) &&
            set(GL_UNPACK_ROW_LENGTH, row_length, to.row_length) &&
            set(GL_UNPACK_SKIP_ROWS, skip_rows, to.skip_rows) &&
            set(GL_UNPACK_SKIP_PIXELS, skip_pixels, to.skip_pixels);
    }
    bool restore(const mgmem_upload_platform *p, const State &upload, int32_t *detail) const noexcept {
        if (!upload.set_pixel_store(p, *this, detail)) return false;
        if (unpack_buffer) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack_buffer));
            if (!gl_clean(detail) || !current(p)) return false;
        }
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
        return gl_clean(detail) && current(p);
    }
};
}

mgmem_status mgmem_upload_platform_open(mgmem_upload_platform **out, int32_t *detail) noexcept {
    if (!out || !detail) return MGMEM_INVALID_ARGUMENT;
    *out = nullptr; *detail = 0;
    const auto display = eglGetCurrentDisplay();
    const auto context = eglGetCurrentContext();
    if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT || eglQueryAPI() != EGL_OPENGL_ES_API)
        return MGMEM_UNSUPPORTED;
    const uint64_t token = thread_token();
    if (!token) return MGMEM_BUSY;
    if (!egl_clean(detail) || !gl_clean(detail)) return MGMEM_PROVIDER_ERROR;
    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    if (!gl_clean(detail) || major < 3) return MGMEM_UNSUPPORTED;
    const char *egl_ext = eglQueryString(display, EGL_EXTENSIONS);
    const char *gl_ext = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
    if (!egl_clean(detail) || !gl_clean(detail)) return MGMEM_PROVIDER_ERROR;
    if (!has_extension(egl_ext, "EGL_KHR_image_base") ||
        !has_extension(egl_ext, "EGL_ANDROID_image_native_buffer") ||
        !has_extension(egl_ext, "EGL_ANDROID_get_native_client_buffer") ||
        !has_extension(egl_ext, "EGL_KHR_fence_sync") ||
        !has_extension(egl_ext, "EGL_ANDROID_native_fence_sync") ||
        !has_extension(gl_ext, "GL_OES_EGL_image")) return MGMEM_UNSUPPORTED;
    auto *p = new (std::nothrow) mgmem_upload_platform{};
    if (!p) return MGMEM_OUT_OF_MEMORY;
    p->display = display; p->context = context; p->thread = std::this_thread::get_id(); p->thread_token = token;
#define LOAD(field, type, name) p->field = reinterpret_cast<type>(eglGetProcAddress(name))
    LOAD(client, PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC, "eglGetNativeClientBufferANDROID");
    LOAD(create_image, PFNEGLCREATEIMAGEKHRPROC, "eglCreateImageKHR");
    LOAD(destroy_image, PFNEGLDESTROYIMAGEKHRPROC, "eglDestroyImageKHR");
    LOAD(target_image, PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, "glEGLImageTargetTexture2DOES");
    LOAD(create_sync, PFNEGLCREATESYNCKHRPROC, "eglCreateSyncKHR");
    LOAD(destroy_sync, PFNEGLDESTROYSYNCKHRPROC, "eglDestroySyncKHR");
    LOAD(dup_fence, PFNEGLDUPNATIVEFENCEFDANDROIDPROC, "eglDupNativeFenceFDANDROID");
#undef LOAD
    if (!egl_clean(detail) || !p->client || !p->create_image || !p->destroy_image ||
        !p->target_image || !p->create_sync || !p->destroy_sync || !p->dup_fence) {
        delete p; return MGMEM_UNSUPPORTED;
    }
    *out = p;
    return MGMEM_OK;
}

mgmem_status mgmem_upload_platform_write(mgmem_upload_platform *p, const mgmem_gpu_ahb_view &view,
    uint64_t source_stride, int32_t *detail) noexcept {
    if (!p || !detail || !view.buffer || !view.data || !current(p)) return MGMEM_PROVIDER_ERROR;
    *detail = 0;
    if (!egl_clean(detail) || !gl_clean(detail)) return MGMEM_PROVIDER_ERROR;
    State previous{};
    if (!previous.snapshot(p, detail)) return MGMEM_PROVIDER_ERROR;
    State upload{};
    // GLES unpack alignment is 1, 2, 4 or 8. Keep it only when both the
    // source address and row stride meet the current alignment.
    const auto source_layout = reinterpret_cast<uintptr_t>(view.data) | source_stride;
    const auto alignment_mask = static_cast<unsigned>(previous.alignment) - 1;
    upload.alignment = (source_layout & alignment_mask) == 0 ? previous.alignment : 1;
    const auto row_pixels = source_stride / 4;
    // A zero row length already means the upload width.
    if (row_pixels != view.width || previous.row_length != 0)
        upload.row_length = static_cast<GLint>(row_pixels);
    const auto native = p->client(view.buffer);
    if (!native || !egl_clean(detail) || !current(p)) return MGMEM_PROVIDER_ERROR;
    const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    p->image = p->create_image(p->display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, native, attrs);
    if (p->image == EGL_NO_IMAGE_KHR || !egl_clean(detail) || !current(p)) return MGMEM_PROVIDER_ERROR;
    glGenTextures(1, &p->texture);
    if (!p->texture || !gl_clean(detail) || !current(p)) return MGMEM_PROVIDER_ERROR;
    glBindTexture(GL_TEXTURE_2D, p->texture);
    bool ok = gl_clean(detail) && current(p);
    if (ok && previous.unpack_buffer) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        ok = gl_clean(detail) && current(p);
    }
    if (ok) ok = previous.set_pixel_store(p, upload, detail);
    if (ok) {
        /* This call itself may invalidate target pixels. Its errors and every
         * later error require quarantine even if TexSubImage was not issued. */
        p->target_image(GL_TEXTURE_2D, static_cast<GLeglImageOES>(p->image));
        ok = gl_clean(detail) && current(p);
    }
    if (ok) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(view.width),
            static_cast<GLsizei>(view.height), GL_RGBA, GL_UNSIGNED_BYTE, view.data);
        ok = gl_clean(detail) && current(p);
    }
    if (!current(p) || fatal_gl(*detail)) return MGMEM_PROVIDER_ERROR;
    const int32_t body_error = *detail;
    int32_t restore_error = 0;
    const bool restored = previous.restore(p, upload, &restore_error);
    if (!restored) { *detail = restore_error; return MGMEM_PROVIDER_ERROR; }
    *detail = body_error;
    return ok ? MGMEM_OK : MGMEM_PROVIDER_ERROR;
}

mgmem_status mgmem_upload_platform_fence(mgmem_upload_platform *p, int *fd, int32_t *detail) noexcept {
    if (!p || !fd || !detail || !current(p)) return MGMEM_PROVIDER_ERROR;
    *fd = -1; *detail = 0;
    if (!egl_clean(detail) || !gl_clean(detail)) return MGMEM_PROVIDER_ERROR;
    p->fence = p->create_sync(p->display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (p->fence == EGL_NO_SYNC_KHR || !egl_clean(detail) || !current(p)) return MGMEM_PROVIDER_ERROR;
    glFlush();
    if (!gl_clean(detail) || !current(p)) return MGMEM_PROVIDER_ERROR;
    const int result = p->dup_fence(p->display, p->fence);
    if (result >= 0) *fd = result;
    if (result < 0 || !egl_clean(detail) || !current(p)) return MGMEM_PROVIDER_ERROR;
    return MGMEM_OK;
}

mgmem_status mgmem_upload_platform_cleanup(mgmem_upload_platform *p, int32_t *detail) noexcept {
    if (!p || !detail) return MGMEM_INVALID_ARGUMENT;
    if (!current(p)) return MGMEM_BUSY;
    *detail = 0;
    if (!egl_clean(detail)) return *detail == EGL_CONTEXT_LOST ? MGMEM_PROVIDER_ERROR : MGMEM_BUSY;
    if (!gl_clean(detail)) return fatal_gl(*detail) ? MGMEM_PROVIDER_ERROR : MGMEM_BUSY;
    if (p->texture) {
        glDeleteTextures(1, &p->texture);
        if (!gl_clean(detail) || !current(p)) return MGMEM_PROVIDER_ERROR;
        p->texture = 0;
    }
    if (p->image != EGL_NO_IMAGE_KHR) {
        if (!p->destroy_image(p->display, p->image) || !egl_clean(detail) || !current(p)) return MGMEM_PROVIDER_ERROR;
        p->image = EGL_NO_IMAGE_KHR;
    }
    if (p->fence != EGL_NO_SYNC_KHR) {
        if (!p->destroy_sync(p->display, p->fence) || !egl_clean(detail) || !current(p)) return MGMEM_PROVIDER_ERROR;
        p->fence = EGL_NO_SYNC_KHR;
    }
    return MGMEM_OK;
}

void mgmem_upload_platform_discard(mgmem_upload_platform *p) noexcept { delete p; }
