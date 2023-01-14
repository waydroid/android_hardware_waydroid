/*
 * Copyright © 2022 Waydroid Project.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "egl-tools.h"

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <semaphore.h>
#include <ui/GraphicBuffer.h>

const char* eglStrError(EGLint err)
{
    switch (err){
        case EGL_SUCCESS:           return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED:   return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS:        return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC:         return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE:     return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG:        return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT:       return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY:       return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH:         return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER:     return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE:       return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST:      return "EGL_CONTEXT_LOST";
        default: return "UNKNOWN";
    }
}

const char *vertSource =
    "precision mediump float;\n"
    "attribute vec2 in_position;\n"
    "attribute vec2 in_texcoord;\n"
    "varying vec2 texcoord;\n"
    "\n"
    "void main()\n"
    "{\n"
    "   gl_Position = vec4(in_position, 0.0, 1.0);\n"
    "   texcoord = in_texcoord;\n"
    "}\n";

const char *fragSource =
    "precision mediump float;\n"
    "varying vec2 texcoord;\n"
    "uniform sampler2D texture;\n"
    "\n"
    "void main()\n"
    "{\n"
    "   gl_FragColor = texture2D(texture, texcoord);\n"
    "}\n";

GLint createProgram(const char* vs, const char* fs) {
    GLint success = 0;
    GLint logLength = 0;
    char infoLog[1024];

    GLint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vs, 0);
    glCompileShader(vertexShader);
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(vertexShader, sizeof(infoLog), &logLength, infoLog);
        printf("Vertex shader compilation failed:\n%s\n", infoLog);
    }

    GLint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fs, 0);
    glCompileShader(fragmentShader);
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(fragmentShader, sizeof(infoLog), &logLength, infoLog);
        printf("Fragment shader compilation failed:\n%s\n", infoLog);
    }

    GLint program = glCreateProgram();
    glAttachShader(program, fragmentShader);
    glAttachShader(program, vertexShader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success)
    {
        glGetProgramInfoLog(program, sizeof(infoLog), &logLength, infoLog);
        printf("Program linking failed:\n%s\n", infoLog);
    }
    return program;
}

void drawQuad(int x, int y, int w, int h) {
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLint program;

    const GLfloat viewW = 0.5f * viewport[2];
    const GLfloat viewH = 0.5f * viewport[3];
    const GLfloat texW = 1.0f, texH = 1.0f;
    const GLfloat quadX1 = x       / viewW - 1.0f, quadY1 = y       / viewH - 1.0f;
    const GLfloat quadX2 = (x + w) / viewW - 1.0f, quadY2 = (y + h) / viewH - 1.0f;
    const GLfloat texcoords[] =
    {
         0,       0,
         0,       texH,
         texW,    0,
         texW,    texH
    };

    const GLfloat vertices[] =
    {
        quadX1, quadY1,
        quadX1, quadY2,
        quadX2, quadY1,
        quadX2, quadY2,
    };

    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    GLint positionAttr = glGetAttribLocation(program, "in_position");
    GLint texcoordAttr = glGetAttribLocation(program, "in_texcoord");

    glVertexAttribPointer(positionAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(texcoordAttr, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    glEnableVertexAttribArray(positionAttr);
    glEnableVertexAttribArray(texcoordAttr);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void egl_init(struct display* display) {
    display->egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display->egl_dpy, NULL, NULL);
    ALOGI("eglInitialize: %s", eglStrError(eglGetError()));

    EGLConfig config;
    int num_config;
    EGLint dpy_attrs[] = { EGL_NATIVE_VISUAL_ID, (EGLint)HAL_PIXEL_FORMAT_RGBA_8888, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    eglChooseConfig(display->egl_dpy, dpy_attrs, &config, 1, &num_config);
    ALOGI("eglChooseConfig: %s", eglStrError(eglGetError()));

    EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(display->egl_dpy, config,  EGL_NO_CONTEXT, context_attrs);
    ALOGI("eglCreateContext: %s", eglStrError(eglGetError()));

    EGLint pbuf_attrs[] = { EGL_WIDTH, (EGLint)display->width, EGL_HEIGHT, (EGLint)display->height, EGL_NONE };
    EGLSurface pbuf = eglCreatePbufferSurface(display->egl_dpy, config, pbuf_attrs);
    ALOGI("eglCreatePbufferSurface: %s", eglStrError(eglGetError()));

    eglMakeCurrent(display->egl_dpy, pbuf, pbuf, ctx);
    ALOGI("eglMakeCurrent: %s", eglStrError(eglGetError()));

    GLint prog = createProgram(vertSource, fragSource);
    glUseProgram(prog);
    ALOGI("glUseProgram: %d", glGetError());
}

void egl_render_to_pixels(struct display* display, struct buffer* buf) {
    // Wrap native handle into ANativeWindowBuffer for eglCreateImageKHR
    android::sp<android::GraphicBuffer> graphicBuffer = new android::GraphicBuffer(
            buf->width, buf->height, buf->hal_format, 1 /* layers */,
            android::GraphicBuffer::USAGE_HW_TEXTURE | android::GraphicBuffer::USAGE_SW_READ_OFTEN,
            buf->pixel_stride, (native_handle_t*)buf->handle, false);

    EGLint image_attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    auto image = eglCreateImageKHR(display->egl_dpy, EGL_NO_CONTEXT,
                                EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer) graphicBuffer->getNativeBuffer(),
                                image_attrs);

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

    drawQuad(0, 0, buf->width, buf->height);

    glReadPixels(0, 0, buf->width, buf->height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, buf->shm_data);

    glDeleteTextures(1, &texture);
    eglDestroyImageKHR(display->egl_dpy, image);
}

void* egl_loop(void* data) {
    struct display* display = (struct display*) data;
    egl_init(display);

    while (true) {
        sem_wait(&display->egl_go);
        for (auto const& f : display->egl_work_queue) {
            f();
        }
        display->egl_work_queue.clear();
        sem_post(&display->egl_done);
    }
    return NULL;
}
