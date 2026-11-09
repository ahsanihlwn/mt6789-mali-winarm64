/*
 * mali_gl_smoke.c  -  offscreen OpenGL bring-up for the Mesa panfrost GDI driver
 *                     (Windows ARM64, MT6789 Mali-G57).
 *
 * WHY THIS EXISTS
 * ---------------
 * DEPLOY.md section 3: the Gallium panfrost build (libgallium_wgl.dll +
 * opengl32.dll + z-1.dll) is the only path on this GPU that can actually render,
 * and it had no test program. This is that program.
 *
 * It deliberately links nothing: opengl32.dll is loaded by name from the folder
 * the executable lives in, so the local Mesa build wins over System32 without
 * touching the system driver or the registry. GL entry points above 1.1 come
 * from wglGetProcAddress, exactly as a real application would fetch them.
 *
 * Stages
 *   G1  load the local opengl32.dll                              (fail = stop)
 *   G2  hidden window + pixel format + WGL context                (fail = stop)
 *   G3  GL_VENDOR / GL_RENDERER / GL_VERSION                      (informational)
 *   G4  FBO + renderbuffer, clear, read back, verify              (pass criterion)
 *   G5  shader triangle into the FBO, read back, verify           (best effort)
 *
 * RESULT: PASS requires G1-G4. G5 is reported separately because a shader path
 * can be missing while the winsys handshake is still proven.
 *
 * The interesting failure is G2: it means the DRM winsys handshake failed, i.e.
 * Mesa never reached /dev/dri/renderD128 (mali_drm.cpp) or the KMD rejected the
 * render node. The GL version string in G3 tells us which driver actually
 * answered (panfrost vs. a fallback).
 *
 * The KMD counters (malitest.exe --irq) should show job completions after this
 * runs; an all-zero counter set with RESULT: PASS means the CPU did the work.
 *
 * SPDX-License-Identifier: MIT
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS 1

#include <windows.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* GL types and the handful of constants we need (no GL headers)       */
/* ------------------------------------------------------------------ */
typedef unsigned int   GLenum;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLboolean;
typedef unsigned char  GLubyte;
typedef float          GLfloat;
typedef unsigned int   GLbitfield;
typedef ptrdiff_t      GLsizeiptr;

#define GL_VENDOR                          0x1F00
#define GL_RENDERER                        0x1F01
#define GL_VERSION                         0x1F02
#define GL_EXTENSIONS                      0x1F03

#define GL_COLOR_BUFFER_BIT                0x00004000
#define GL_RGBA                            0x1908
#define GL_RGBA8                           0x8058
#define GL_UNSIGNED_BYTE                   0x1401
#define GL_FLOAT                           0x1406

#define GL_FRAMEBUFFER                     0x8D40
#define GL_RENDERBUFFER                    0x8D41
#define GL_COLOR_ATTACHMENT0               0x8CE0
#define GL_FRAMEBUFFER_COMPLETE            0x8CD5

#define GL_VERTEX_SHADER                   0x8B31
#define GL_FRAGMENT_SHADER                 0x8B30
#define GL_COMPILE_STATUS                  0x8B81
#define GL_LINK_STATUS                     0x8B82

#define GL_ARRAY_BUFFER                    0x8892
#define GL_STATIC_DRAW                     0x88E4
#define GL_TRIANGLES                       0x0004

#define FB_W   64
#define FB_H   64

/* ------------------------------------------------------------------ */
/* function pointers                                                   */
/* ------------------------------------------------------------------ */
static HMODULE g_gl;
static HMODULE g_gdi;
static PROC (WINAPI *p_wglGetProcAddress)(LPCSTR);

static void *gl(const char *name)
{
    PROC p = NULL;
    if (p_wglGetProcAddress)
        p = p_wglGetProcAddress(name);
    if (!p && g_gl)
        p = GetProcAddress(g_gl, name);
    return (void *)p;
}

typedef const GLubyte *(WINAPI *PFN_glGetString)(GLenum);
typedef void   (WINAPI *PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void   (WINAPI *PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (WINAPI *PFN_glClear)(GLbitfield);
typedef void   (WINAPI *PFN_glFinish)(void);
typedef void   (WINAPI *PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void   (WINAPI *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void   (WINAPI *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void   (WINAPI *PFN_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void   (WINAPI *PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void   (WINAPI *PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (WINAPI *PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (WINAPI *PFN_glCheckFramebufferStatus)(GLenum);

typedef GLuint (WINAPI *PFN_glCreateShader)(GLenum);
typedef void   (WINAPI *PFN_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void   (WINAPI *PFN_glCompileShader)(GLuint);
typedef void   (WINAPI *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void   (WINAPI *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef GLuint (WINAPI *PFN_glCreateProgram)(void);
typedef void   (WINAPI *PFN_glAttachShader)(GLuint, GLuint);
typedef void   (WINAPI *PFN_glLinkProgram)(GLuint);
typedef void   (WINAPI *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void   (WINAPI *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void   (WINAPI *PFN_glUseProgram)(GLuint);
typedef GLint  (WINAPI *PFN_glGetAttribLocation)(GLuint, const char *);
typedef void   (WINAPI *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void   (WINAPI *PFN_glBindVertexArray)(GLuint);
typedef void   (WINAPI *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void   (WINAPI *PFN_glBindBuffer)(GLenum, GLuint);
typedef void   (WINAPI *PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void   (WINAPI *PFN_glEnableVertexAttribArray)(GLuint);
typedef void   (WINAPI *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void   (WINAPI *PFN_glDrawArrays)(GLenum, GLint, GLsizei);

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static int fail(const char *stage, const char *what)
{
    printf("%s FAILED: %s (GetLastError=%lu)\n", stage, what, (unsigned long)GetLastError());
    return 1;
}

static void exe_dir(char *out, size_t cap)
{
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) { out[0] = '\0'; return; }
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/')
        n--;
    out[n] = '\0';
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)h; (void)m; (void)w; (void)l;
    return DefWindowProcA(h, m, w, l);
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    char dir[MAX_PATH];
    char dll[MAX_PATH];
    exe_dir(dir, sizeof(dir));
    _snprintf(dll, sizeof(dll), "%s\\opengl32.dll", dir);

    /* ---------------- G1: load the local Mesa opengl32.dll ---------------- */
    g_gl = LoadLibraryA(dll);
    if (!g_gl)
        return fail("G1", "LoadLibraryA(opengl32.dll)");
    {
        char got[MAX_PATH];
        GetModuleFileNameA(g_gl, got, sizeof(got));
        printf("G1 opengl32.dll loaded: %s\n", got);
    }

    p_wglGetProcAddress = (PROC (WINAPI *)(LPCSTR))GetProcAddress(g_gl, "wglGetProcAddress");

    /* ---------------- G2: hidden window + pixel format + context ---------------- */
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "MaliGlSmoke";
    if (!RegisterClassA(&wc))
        return fail("G2", "RegisterClassA");

    HWND hwnd = CreateWindowExA(0, "MaliGlSmoke", "mali-gl-smoke",
                                WS_POPUP, 0, 0, FB_W, FB_H,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
        return fail("G2", "CreateWindowExA");

    HDC dc = GetDC(hwnd);
    if (!dc)
        return fail("G2", "GetDC");
    HDC screen = GetDC(NULL);

    typedef int  (WINAPI *PFN_ChoosePixelFormat)(HDC, const PIXELFORMATDESCRIPTOR *);
    typedef int  (WINAPI *PFN_DescribePixelFormat)(HDC, int, UINT, PIXELFORMATDESCRIPTOR *);
    typedef BOOL (WINAPI *PFN_SetPixelFormat)(HDC, int, const PIXELFORMATDESCRIPTOR *);
    typedef HGLRC(WINAPI *PFN_wglCreateContext)(HDC);
    typedef BOOL (WINAPI *PFN_wglMakeCurrent)(HDC, HGLRC);
    typedef BOOL (WINAPI *PFN_wglDeleteContext)(HGLRC);

    /* ChoosePixelFormat / SetPixelFormat / DescribePixelFormat live in GDI32, not
     * opengl32 (the real Windows opengl32.dll does not export them either);
     * wglCreateContext and friends come from the loaded opengl32.dll. */
    g_gdi = LoadLibraryA("gdi32.dll");
    if (!g_gdi)
        return fail("G2", "LoadLibraryA(gdi32.dll)");

    PFN_ChoosePixelFormat   p_ChoosePixelFormat   = (PFN_ChoosePixelFormat)GetProcAddress(g_gdi, "ChoosePixelFormat");
    PFN_DescribePixelFormat p_DescribePixelFormat = (PFN_DescribePixelFormat)GetProcAddress(g_gdi, "DescribePixelFormat");
    PFN_SetPixelFormat      p_SetPixelFormat      = (PFN_SetPixelFormat)GetProcAddress(g_gdi, "SetPixelFormat");
    PFN_wglCreateContext    p_wglCreateContext    = (PFN_wglCreateContext)GetProcAddress(g_gl, "wglCreateContext");
    PFN_wglMakeCurrent      p_wglMakeCurrent      = (PFN_wglMakeCurrent)GetProcAddress(g_gl, "wglMakeCurrent");
    PFN_wglDeleteContext    p_wglDeleteContext    = (PFN_wglDeleteContext)GetProcAddress(g_gl, "wglDeleteContext");
    if (!p_ChoosePixelFormat || !p_SetPixelFormat || !p_wglCreateContext || !p_wglMakeCurrent)
        return fail("G2", "missing WGL/GDI entry points");

    /* GDI's pixel formats come from the display driver, not from Mesa, so on a
     * machine whose display driver has no OpenGL support this can legitimately be
     * empty. Probe a few descriptors across the window and screen DCs and report
     * what exists rather than failing on the first attempt. */
    static const struct {
        const char *name;
        DWORD flags;
        BYTE color, depth, alpha;
    } cands[] = {
        { "win/double 32/24/8", PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, 32, 24, 8 },
        { "win/double 32/24",   PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, 32, 24, 0 },
        { "win/double 32",      PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, 32,  0, 0 },
        { "win/double 24",      PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, 24,  0, 0 },
        { "win/single 32",      PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL,                   32,  0, 0 },
        { "win/single 24",      PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL,                   24,  0, 0 },
        { "bitmap 24",          PFD_DRAW_TO_BITMAP | PFD_SUPPORT_OPENGL,                   24,  0, 0 },
    };
    const unsigned n_cands = sizeof(cands) / sizeof(cands[0]);

    HDC  best_dc  = NULL;
    int  best_fmt = 0;
    PIXELFORMATDESCRIPTOR best_pfd;
    memset(&best_pfd, 0, sizeof(best_pfd));

    for (int c = 0; c < 2 && best_fmt == 0; c++) {
        HDC d = (c == 0) ? dc : screen;
        const char *dn = (c == 0) ? "window DC" : "screen DC";
        if (!d)
            continue;
        if (p_DescribePixelFormat) {
            int n = 0;
            PIXELFORMATDESCRIPTOR q;
            for (int i = 1; i <= 512; i++) {
                memset(&q, 0, sizeof(q));
                if (p_DescribePixelFormat(d, i, sizeof(q), &q) == 0)
                    break;
                n++;
            }
            printf("G2 %s: %d pixel format(s) available\n", dn, n);
        }
        for (unsigned k = 0; k < n_cands; k++) {
            PIXELFORMATDESCRIPTOR pfd;
            memset(&pfd, 0, sizeof(pfd));
            pfd.nSize      = sizeof(pfd);
            pfd.nVersion   = 1;
            pfd.dwFlags    = cands[k].flags;
            pfd.iPixelType = PFD_TYPE_RGBA;
            pfd.cColorBits = cands[k].color;
            pfd.cDepthBits = cands[k].depth;
            pfd.cAlphaBits = cands[k].alpha;
            int fmt = p_ChoosePixelFormat(d, &pfd);
            if (fmt) {
                printf("G2 %s: chose format %d (%s)\n", dn, fmt, cands[k].name);
                best_dc  = d;
                best_fmt = fmt;
                best_pfd = pfd;
                break;
            }
        }
    }
    if (best_fmt == 0)
        return fail("G2", "no GDI pixel format (display driver has no OpenGL)");
    if (!p_SetPixelFormat(best_dc, best_fmt, &best_pfd))
        return fail("G2", "SetPixelFormat");
    dc = best_dc;
    printf("G2 pixel format %d set\n", best_fmt);

    HGLRC rc = p_wglCreateContext(dc);
    if (!rc)
        return fail("G2", "wglCreateContext (DRM winsys handshake)");
    if (!p_wglMakeCurrent(dc, rc))
        return fail("G2", "wglMakeCurrent");
    printf("G2 WGL context current\n");

    /* ---------------- G3: who answered? ---------------- */
    PFN_glGetString p_glGetString = (PFN_glGetString)gl("glGetString");
    if (p_glGetString) {
        const GLubyte *v = p_glGetString(GL_VENDOR);
        const GLubyte *r = p_glGetString(GL_RENDERER);
        const GLubyte *g = p_glGetString(GL_VERSION);
        printf("G3 vendor   %s\n", v ? (const char *)v : "(null)");
        printf("G3 renderer %s\n", r ? (const char *)r : "(null)");
        printf("G3 version  %s\n", g ? (const char *)g : "(null)");
    } else {
        printf("G3 glGetString unavailable\n");
    }

    /* ---------------- G4: FBO + renderbuffer, clear, read back ---------------- */
    PFN_glGenFramebuffers         p_glGenFramebuffers     = (PFN_glGenFramebuffers)gl("glGenFramebuffers");
    PFN_glBindFramebuffer         p_glBindFramebuffer     = (PFN_glBindFramebuffer)gl("glBindFramebuffer");
    PFN_glGenRenderbuffers        p_glGenRenderbuffers    = (PFN_glGenRenderbuffers)gl("glGenRenderbuffers");
    PFN_glBindRenderbuffer        p_glBindRenderbuffer    = (PFN_glBindRenderbuffer)gl("glBindRenderbuffer");
    PFN_glRenderbufferStorage     p_glRenderbufferStorage = (PFN_glRenderbufferStorage)gl("glRenderbufferStorage");
    PFN_glFramebufferRenderbuffer p_glFramebufferRenderbuffer = (PFN_glFramebufferRenderbuffer)gl("glFramebufferRenderbuffer");
    PFN_glCheckFramebufferStatus  p_glCheckFramebufferStatus = (PFN_glCheckFramebufferStatus)gl("glCheckFramebufferStatus");
    PFN_glViewport    p_glViewport    = (PFN_glViewport)gl("glViewport");
    PFN_glClearColor  p_glClearColor  = (PFN_glClearColor)gl("glClearColor");
    PFN_glClear       p_glClear       = (PFN_glClear)gl("glClear");
    PFN_glFinish      p_glFinish      = (PFN_glFinish)gl("glFinish");
    PFN_glReadPixels  p_glReadPixels  = (PFN_glReadPixels)gl("glReadPixels");

    if (!p_glGenFramebuffers || !p_glBindFramebuffer || !p_glGenRenderbuffers ||
        !p_glBindRenderbuffer || !p_glRenderbufferStorage || !p_glFramebufferRenderbuffer ||
        !p_glCheckFramebufferStatus || !p_glViewport || !p_glClearColor || !p_glClear ||
        !p_glFinish || !p_glReadPixels)
        return fail("G4", "FBO/clear entry points unavailable");

    GLuint fbo = 0, rbo = 0;
    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glGenRenderbuffers(1, &rbo);
    p_glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, FB_W, FB_H);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);

    GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        printf("G4 framebuffer incomplete: 0x%04X\n", (unsigned)st);
        return fail("G4", "glCheckFramebufferStatus");
    }

    p_glViewport(0, 0, FB_W, FB_H);
    p_glClearColor(0.25f, 0.50f, 0.75f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glFinish();

    unsigned char px[4] = { 0, 0, 0, 0 };
    p_glReadPixels(FB_W / 2, FB_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("G4 clear readback %u %u %u %u\n", px[0], px[1], px[2], px[3]);

    int g4_ok = (px[0] > 40 && px[0] < 90) &&
                (px[1] > 100 && px[1] < 160) &&
                (px[2] > 160 && px[2] < 220) &&
                (px[3] > 200);
    if (!g4_ok) {
        printf("G4 FAILED: expected ~64,128,191,255\n");
        return 1;
    }
    printf("G4 offscreen clear + readback OK (GPU job submitted)\n");

    /* ---------------- G5: shader triangle (best effort) ---------------- */
    int g5_ok = 0;
    {
        PFN_glCreateShader   p_glCreateShader   = (PFN_glCreateShader)gl("glCreateShader");
        PFN_glShaderSource   p_glShaderSource   = (PFN_glShaderSource)gl("glShaderSource");
        PFN_glCompileShader  p_glCompileShader  = (PFN_glCompileShader)gl("glCompileShader");
        PFN_glGetShaderiv    p_glGetShaderiv    = (PFN_glGetShaderiv)gl("glGetShaderiv");
        PFN_glGetShaderInfoLog p_glGetShaderInfoLog = (PFN_glGetShaderInfoLog)gl("glGetShaderInfoLog");
        PFN_glCreateProgram  p_glCreateProgram  = (PFN_glCreateProgram)gl("glCreateProgram");
        PFN_glAttachShader   p_glAttachShader   = (PFN_glAttachShader)gl("glAttachShader");
        PFN_glLinkProgram    p_glLinkProgram    = (PFN_glLinkProgram)gl("glLinkProgram");
        PFN_glGetProgramiv   p_glGetProgramiv   = (PFN_glGetProgramiv)gl("glGetProgramiv");
        PFN_glGetProgramInfoLog p_glGetProgramInfoLog = (PFN_glGetProgramInfoLog)gl("glGetProgramInfoLog");
        PFN_glUseProgram     p_glUseProgram     = (PFN_glUseProgram)gl("glUseProgram");
        PFN_glGetAttribLocation p_glGetAttribLocation = (PFN_glGetAttribLocation)gl("glGetAttribLocation");
        PFN_glGenVertexArrays   p_glGenVertexArrays   = (PFN_glGenVertexArrays)gl("glGenVertexArrays");
        PFN_glBindVertexArray   p_glBindVertexArray   = (PFN_glBindVertexArray)gl("glBindVertexArray");
        PFN_glGenBuffers     p_glGenBuffers     = (PFN_glGenBuffers)gl("glGenBuffers");
        PFN_glBindBuffer     p_glBindBuffer     = (PFN_glBindBuffer)gl("glBindBuffer");
        PFN_glBufferData     p_glBufferData     = (PFN_glBufferData)gl("glBufferData");
        PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray = (PFN_glEnableVertexAttribArray)gl("glEnableVertexAttribArray");
        PFN_glVertexAttribPointer     p_glVertexAttribPointer     = (PFN_glVertexAttribPointer)gl("glVertexAttribPointer");
        PFN_glDrawArrays     p_glDrawArrays     = (PFN_glDrawArrays)gl("glDrawArrays");

        if (!p_glCreateShader || !p_glShaderSource || !p_glCompileShader || !p_glGetShaderiv ||
            !p_glCreateProgram || !p_glAttachShader || !p_glLinkProgram || !p_glGetProgramiv ||
            !p_glUseProgram || !p_glGetAttribLocation || !p_glGenVertexArrays || !p_glBindVertexArray ||
            !p_glGenBuffers || !p_glBindBuffer || !p_glBufferData || !p_glEnableVertexAttribArray ||
            !p_glVertexAttribPointer || !p_glDrawArrays) {
            printf("G5 skipped: shader entry points unavailable\n");
        } else {
            static const char *vs_src =
                "#version 150\n"
                "in vec2 p;\n"
                "void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
            static const char *fs_src =
                "#version 150\n"
                "out vec4 c;\n"
                "void main() { c = vec4(1.0, 0.0, 0.0, 1.0); }\n";

            GLuint vs = p_glCreateShader(GL_VERTEX_SHADER);
            GLuint fs = p_glCreateShader(GL_FRAGMENT_SHADER);
            p_glShaderSource(vs, 1, &vs_src, NULL);
            p_glShaderSource(fs, 1, &fs_src, NULL);
            p_glCompileShader(vs);
            p_glCompileShader(fs);

            GLint ok = 0;
            p_glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[512] = { 0 };
                if (p_glGetShaderInfoLog) p_glGetShaderInfoLog(vs, sizeof(log) - 1, NULL, log);
                printf("G5 vertex shader compile failed: %s\n", log);
            } else {
                GLuint prog = p_glCreateProgram();
                p_glAttachShader(prog, vs);
                p_glAttachShader(prog, fs);
                p_glLinkProgram(prog);
                p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
                if (!ok) {
                    char log[512] = { 0 };
                    if (p_glGetProgramInfoLog) p_glGetProgramInfoLog(prog, sizeof(log) - 1, NULL, log);
                    printf("G5 program link failed: %s\n", log);
                } else {
                    static const GLfloat verts[6] = {
                        -0.8f, -0.8f,
                         0.8f, -0.8f,
                         0.0f,  0.8f,
                    };
                    GLuint vao = 0, vbo = 0;
                    p_glGenVertexArrays(1, &vao);
                    p_glBindVertexArray(vao);
                    p_glGenBuffers(1, &vbo);
                    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
                    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(verts), verts, GL_STATIC_DRAW);

                    p_glUseProgram(prog);
                    GLint loc = p_glGetAttribLocation(prog, "p");
                    if (loc >= 0) {
                        p_glEnableVertexAttribArray((GLuint)loc);
                        p_glVertexAttribPointer((GLuint)loc, 2, GL_FLOAT, 0, 0, NULL);
                    }

                    p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    p_glClear(GL_COLOR_BUFFER_BIT);
                    p_glDrawArrays(GL_TRIANGLES, 0, 3);
                    p_glFinish();

                    unsigned char tp[4] = { 0, 0, 0, 0 };
                    p_glReadPixels(FB_W / 2, FB_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, tp);
                    printf("G5 triangle centre %u %u %u %u\n", tp[0], tp[1], tp[2], tp[3]);
                    g5_ok = (tp[0] > 200 && tp[1] < 60 && tp[2] < 60);
                    if (g5_ok)
                        printf("G5 shader triangle rendered OK\n");
                    else
                        printf("G5 shader triangle: unexpected pixel\n");
                }
            }
        }
    }

    if (p_wglDeleteContext && rc)
        p_wglDeleteContext(rc);

    printf("RESULT: %s\n", g4_ok ? "PASS" : "FAIL");
    printf("G5: %s\n", g5_ok ? "PASS" : "SKIP/FAIL");
    return g4_ok ? 0 : 1;
}
