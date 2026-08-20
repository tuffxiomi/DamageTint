#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string_view>
#include <vector>

namespace damagetint {
namespace {

constexpr char kTag[] = "DamageTint";
constexpr std::size_t kHurtTimeOffset = 0x194;
constexpr std::size_t kClientInstanceGetLocalPlayerVtableIndex = 32;

struct PatternByte { std::uint8_t value, mask; };

std::vector<PatternByte> parsePattern(std::string_view pattern) {
    std::vector<PatternByte> out;
    for (std::size_t i = 0; i < pattern.size();) {
        while (i < pattern.size() && std::isspace(static_cast<unsigned char>(pattern[i]))) ++i;
        if (i >= pattern.size()) break;
        if (pattern[i] == '?') {
            out.push_back({0, 0});
            ++i;
            if (i < pattern.size() && pattern[i] == '?') ++i;
            continue;
        }
        if (i + 1 >= pattern.size()) break;
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = hex(pattern[i]), lo = hex(pattern[i + 1]);
        if (hi < 0 || lo < 0) { ++i; continue; }
        out.push_back({static_cast<std::uint8_t>((hi << 4) | lo), 0xFF});
        i += 2;
    }
    return out;
}

struct ScanContext {
    const std::vector<PatternByte>* pattern;
    std::uintptr_t result = 0;
};

int scanCallback(dl_phdr_info* info, std::size_t, void* raw) {
    auto& ctx = *static_cast<ScanContext*>(raw);
    if (ctx.result || !info->dlpi_name || !*info->dlpi_name) return ctx.result ? 1 : 0;
    const std::string_view name(info->dlpi_name);
    if (name.find("libminecraftpe.so") == std::string_view::npos) return 0;
    const auto& pat = *ctx.pattern;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X) || ph.p_memsz < pat.size()) continue;
        const auto base = static_cast<std::uintptr_t>(info->dlpi_addr) + ph.p_vaddr;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(base);
        const auto size = static_cast<std::size_t>(ph.p_memsz);
        for (std::size_t off = 0; off + pat.size() <= size; ++off) {
            bool match = true;
            for (std::size_t j = 0; j < pat.size(); ++j) {
                if ((bytes[off + j] & pat[j].mask) != (pat[j].value & pat[j].mask)) { match = false; break; }
            }
            if (match) { ctx.result = base + off; return 1; }
        }
    }
    return 0;
}

std::uintptr_t resolveClientInstanceUpdate() {
    constexpr std::string_view kPattern =
        "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 "
        "FD 03 00 91 ? ? ? D1 59 D0 3B D5 F3 03 00 AA F4 03 01 2A "
        "? ? ? F9 ? ? ? F8 ? ? ? F9 ? ? ? F9";
    const auto parsed = parsePattern(kPattern);
    ScanContext ctx{&parsed};
    dl_iterate_phdr(scanCallback, &ctx);
    return ctx.result;
}

using ClientInstanceUpdateFn = void* (*)(void*, bool);
using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);

ClientInstanceUpdateFn g_clientUpdateOriginal = nullptr;
EglSwapBuffersFn g_swapBuffersOriginal = nullptr;

std::int64_t monotonicNs() {
    timespec ts{};
    return clock_gettime(CLOCK_MONOTONIC, &ts) == 0
        ? static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec : 0;
}

GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    char log[256]{};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    __android_log_print(ANDROID_LOG_ERROR, kTag, "Shader error: %s", log);
    glDeleteShader(shader);
    return 0;
}

} // namespace

class DamageTint {
public:
    static DamageTint& instance() { static DamageTint x; return x; }
    bool load() { return true; }
    bool enable() { m_enabled.store(true, std::memory_order_release); return installHooks(); }
    bool disable() { m_enabled.store(false, std::memory_order_release); m_currentAlpha = 0.0f; return true; }
    bool unload() { return disable(); }

private:
    bool installHooks() {
        if (!installClientHook()) return false;
        return installSwapHook();
    }

    bool installClientHook() {
        if (m_clientHookHandle) return true;
        const auto target = resolveClientInstanceUpdate();
        if (!target) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "ClientInstanceUpdate signature not found");
            return false;
        }
        void* original = nullptr;
        if (pl::memory::hook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&clientInstanceUpdateHook), &original) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "ClientInstanceUpdate hook failed");
            return false;
        }
        g_clientUpdateOriginal = reinterpret_cast<ClientInstanceUpdateFn>(original);
        m_clientHookHandle = reinterpret_cast<void*>(target);
        return true;
    }

    bool installSwapHook() {
        if (m_swapHookHandle) return true;
        void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
        if (!egl) egl = dlopen("libEGL.so", RTLD_NOW);
        if (!egl) return false;
        void* target = dlsym(egl, "eglSwapBuffers");
        if (!target) { dlclose(egl); return false; }
        void* original = nullptr;
        if (pl::memory::hook(target, reinterpret_cast<void*>(&swapBuffersHook), &original) != 0) {
            dlclose(egl); return false;
        }
        g_swapBuffersOriginal = reinterpret_cast<EglSwapBuffersFn>(original);
        m_swapHookHandle = target;
        dlclose(egl);
        return true;
    }

    static bool valid(const void* p) { return reinterpret_cast<std::uintptr_t>(p) >= 0x10000ULL; }

    static void* clientInstanceUpdateHook(void* self, bool value) {
        if (self) instance().m_clientInstance = self;
        return g_clientUpdateOriginal ? g_clientUpdateOriginal(self, value) : nullptr;
    }

    static int hurtTime(void* client) {
        if (!valid(client)) return 0;
        auto** vtable = *reinterpret_cast<void***>(client);
        if (!vtable) return 0;
        auto fn = reinterpret_cast<void* (*)(void*)>(vtable[kClientInstanceGetLocalPlayerVtableIndex]);
        if (!fn) return 0;
        void* player = fn(client);
        if (!valid(player)) return 0;
        return *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + kHurtTimeOffset);
    }

    void initOverlay() {
        if (m_program) return;
        static constexpr char vsSrc[] =
            "attribute vec2 aPosition; void main(){ gl_Position=vec4(aPosition,0.0,1.0); }";
        static constexpr char fsSrc[] =
            "precision mediump float; uniform float uAlpha; void main(){ gl_FragColor=vec4(1.0,0.0,0.0,uAlpha); }";
        GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
        if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return; }
        GLuint p = glCreateProgram();
        if (!p) { glDeleteShader(vs); glDeleteShader(fs); return; }
        glAttachShader(p, vs); glAttachShader(p, fs); glBindAttribLocation(p, 0, "aPosition"); glLinkProgram(p);
        glDeleteShader(vs); glDeleteShader(fs);
        GLint linked = GL_FALSE; glGetProgramiv(p, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) { glDeleteProgram(p); return; }
        static constexpr GLfloat quad[] = {-1.f,-1.f, 1.f,-1.f, -1.f,1.f, 1.f,1.f};
        GLuint b = 0; glGenBuffers(1, &b); if (!b) { glDeleteProgram(p); return; }
        glBindBuffer(GL_ARRAY_BUFFER, b); glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        m_program = p; m_vertexBuffer = b; m_alphaLocation = glGetUniformLocation(p, "uAlpha");
    }

    static EGLBoolean swapBuffersHook(EGLDisplay display, EGLSurface surface) {
        auto& mod = instance();
        if (!mod.m_enabled.load(std::memory_order_acquire)) return g_swapBuffersOriginal ? g_swapBuffersOriginal(display, surface) : EGL_FALSE;
        if (eglGetCurrentContext() == EGL_NO_CONTEXT || display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE)
            return g_swapBuffersOriginal ? g_swapBuffersOriginal(display, surface) : EGL_FALSE;

        mod.initOverlay();
        if (!mod.m_program) return g_swapBuffersOriginal ? g_swapBuffersOriginal(display, surface) : EGL_FALSE;

        const int hurt = hurtTime(mod.m_clientInstance);
        const float target = hurt > 0 ? std::clamp(static_cast<float>(hurt) / 10.0f * 0.38f, 0.0f, 0.38f) : 0.0f;
        const auto now = monotonicNs();
        const float dt = (mod.m_lastFrameNs > 0 && now > mod.m_lastFrameNs)
            ? std::clamp(static_cast<float>(now - mod.m_lastFrameNs) / 1.0e9f, 0.0f, 0.1f) : 1.0f / 60.0f;
        mod.m_lastFrameNs = now;
        const float smooth = 1.0f - std::exp(-18.0f * dt);
        mod.m_currentAlpha += (target - mod.m_currentAlpha) * smooth;

        if (mod.m_currentAlpha > 0.001f) {
            GLint oldProgram = 0, oldBuffer = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
            const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);
            GLint oldSrc = 0, oldDst = 0; glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldSrc); glGetIntegerv(GL_BLEND_DST_ALPHA, &oldDst);
            glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(mod.m_program); glBindBuffer(GL_ARRAY_BUFFER, mod.m_vertexBuffer); glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glUniform1f(mod.m_alphaLocation, mod.m_currentAlpha); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glDisableVertexAttribArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, oldBuffer); glUseProgram(static_cast<GLuint>(oldProgram)); glBlendFunc(oldSrc, oldDst);
            blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND); depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
            cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE); scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
        }
        return g_swapBuffersOriginal ? g_swapBuffersOriginal(display, surface) : EGL_FALSE;
    }

    std::atomic_bool m_enabled{false};
    void* m_clientInstance = nullptr;
    void* m_clientHookHandle = nullptr;
    void* m_swapHookHandle = nullptr;
    GLuint m_program = 0, m_vertexBuffer = 0;
    GLint m_alphaLocation = -1;
    float m_currentAlpha = 0.0f;
    std::int64_t m_lastFrameNs = 0;
};

} // namespace damagetint

class DamageTintMod {
public:
    static DamageTintMod& instance() { static DamageTintMod x; return x; }
    bool load(pl::mod::ModContext&) { return damagetint::DamageTint::instance().load(); }
    bool enable(pl::mod::ModContext&) { return damagetint::DamageTint::instance().enable(); }
    bool disable(pl::mod::ModContext&) { return damagetint::DamageTint::instance().disable(); }
    bool unload(pl::mod::ModContext&) { return damagetint::DamageTint::instance().unload(); }
};

PL_REGISTER_MOD(DamageTintMod, DamageTintMod::instance())
