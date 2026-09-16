// ARDA_C2_GPU_I420_V2_0
#define LOG_TAG "ArdaGpuI420"

#include "ArdaGpuI420.h"

#include <C2AllocatorGralloc.h>
#include <C2BlockInternal.h>
#include <C2Buffer.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

#include <android/dlext.h>
#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <log/log.h>
#include <sys/system_properties.h>

#include <chrono>
#include <cstring>
#include <utility>

namespace android {
namespace {

using GetExportedNamespaceFn=android_namespace_t *(*)(const char *);
using CreateNamespaceFn=android_namespace_t *(*)(
        const char *,const char *,const char *,uint64_t,const char *,android_namespace_t *);
using LinkNamespacesFn=bool (*)(android_namespace_t *,android_namespace_t *,const char *);
using AhbCreateFromHandleFn=int (*)(
        const AHardwareBuffer_Desc *,
        const native_handle_t *,
        int32_t,
        AHardwareBuffer **);
using AhbReleaseFn=void (*)(AHardwareBuffer *);

static constexpr uint64_t kNamespaceIsolated=1ULL;
// Exact VNDK value for AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE.
static constexpr int32_t kAhbCreateFromHandleClone=3;
static const char *kGraphicsShared="libbinder_ndk.so:libc.so:libdl.so:liblog.so:libm.so:libmediandk.so:libnativewindow.so:libsync.so:libvndksupport.so:libdl_android.so:android.hardware.common-V2-ndk.so:android.hardware.configstore-utils.so:android.hardware.configstore@1.0.so:android.hardware.configstore@1.1.so:android.hardware.graphics.allocator-V2-ndk.so:android.hardware.graphics.allocator@2.0.so:android.hardware.graphics.allocator@3.0.so:android.hardware.graphics.allocator@4.0.so:android.hardware.graphics.common-V7-ndk.so:android.hardware.graphics.common@1.0.so:android.hardware.graphics.common@1.1.so:android.hardware.graphics.common@1.2.so:android.hardware.graphics.mapper@2.0.so:android.hardware.graphics.mapper@2.1.so:android.hardware.graphics.mapper@3.0.so:android.hardware.graphics.mapper@4.0.so:libEGL.so:libGLESv3.so:libSurfaceFlingerProp.so:libaconfig_storage_read_api_cc.so:libapexsupport.so:libbase.so:libbinder.so:libc++.so:libcutils.so:libegl_flags.so:libgralloctypes.so:libgraphicsenv.so:libhidlbase.so:liblzma.so:libnativebridge_lazy.so:libnativeloader_lazy.so:libui.so:libunwindstack.so:libutils.so:libz.so:server_configurable_flags.so";

static const char* kCompute=R"GLSL(
#version 310 es
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
layout(binding=0) uniform highp sampler2D srcTex;
layout(std430,binding=1) buffer I420Buf { uint outWords[]; };

uniform int width;
uniform int height;
uniform uint yWordCount;
uniform uint cWordCount;

int clampi(int v,int lo,int hi){return min(hi,max(lo,v));}
uint y709(ivec3 c){return uint(clampi(((47*c.r+157*c.g+16*c.b)>>8)+16,16,235));}
uint u709(ivec3 c){return uint(clampi(((-26*c.r-86*c.g+112*c.b)>>8)+128,16,240));}
uint v709(ivec3 c){return uint(clampi(((112*c.r-102*c.g-10*c.b)>>8)+128,16,240));}
uint pack4(uint a,uint b,uint c,uint d){return a|(b<<8)|(c<<16)|(d<<24);}
ivec3 rgbAt(int x,int y){
    vec4 f=texelFetch(srcTex,ivec2(x,y),0);
    return ivec3(f.rgb*255.0+0.5);
}

void main(){
    int bx=int(gl_GlobalInvocationID.x)*8;
    int by=int(gl_GlobalInvocationID.y)*2;
    if(bx>=width||by>=height) return;

    uint y0[8],y1[8];
    for(int x=0;x<8;++x){
        ivec3 c0=rgbAt(bx+x,by);
        ivec3 c1=rgbAt(bx+x,by+1);
        y0[x]=y709(c0);
        y1[x]=y709(c1);
    }

    int yWordsPerRow=width/4;
    uint yw0=uint(by*yWordsPerRow+bx/4);
    uint yw1=uint((by+1)*yWordsPerRow+bx/4);
    outWords[yw0]=pack4(y0[0],y0[1],y0[2],y0[3]);
    outWords[yw0+1u]=pack4(y0[4],y0[5],y0[6],y0[7]);
    outWords[yw1]=pack4(y1[0],y1[1],y1[2],y1[3]);
    outWords[yw1+1u]=pack4(y1[4],y1[5],y1[6],y1[7]);

    uint us[4],vs[4];
    for(int p=0;p<4;++p){
        int x=bx+p*2;
        // Canonical Codec2 scalar/NEON semantics: top-left sample of each 2x2 block.
        ivec3 a=rgbAt(x,by);
        us[p]=u709(a);
        vs[p]=v709(a);
    }

    int cWordsPerRow=(width/2)/4;
    uint ci=uint((by/2)*cWordsPerRow+bx/8);
    outWords[yWordCount+ci]=pack4(us[0],us[1],us[2],us[3]);
    outWords[yWordCount+cWordCount+ci]=pack4(vs[0],vs[1],vs[2],vs[3]);
}
)GLSL";

template <typename T>
T sym(void *lib,const char *name) {
    return reinterpret_cast<T>(dlsym(lib,name));
}

} // anonymous namespace

struct ArdaGpuI420::Impl {
    bool ready=false;
    bool held=false;
    const void *heldBase=nullptr;

    uint32_t width=0;
    uint32_t height=0;
    size_t yBytes=0;
    size_t cBytes=0;
    size_t totalBytes=0;

    uint64_t gpuFrames=0;
    uint64_t fallbackFrames=0;
    uint64_t heldFallbacks=0;
    uint64_t failures=0;

    void *dlAndroid=nullptr;
    void *nativewindowLib=nullptr;
    void *eglLib=nullptr;
    void *glesLib=nullptr;

    EGLDisplay dpy=EGL_NO_DISPLAY;
    EGLSurface surf=EGL_NO_SURFACE;
    EGLContext ctx=EGL_NO_CONTEXT;
    EGLConfig cfg=nullptr;

    GLuint program=0;
    GLuint tex=0;
    GLuint ssbo=0;
    void *mapped=nullptr;

    PFNEGLGETDISPLAYPROC pEglGetDisplay=nullptr;
    PFNEGLINITIALIZEPROC pEglInitialize=nullptr;
    PFNEGLCHOOSECONFIGPROC pEglChooseConfig=nullptr;
    PFNEGLCREATEPBUFFERSURFACEPROC pEglCreatePbufferSurface=nullptr;
    PFNEGLCREATECONTEXTPROC pEglCreateContext=nullptr;
    PFNEGLMAKECURRENTPROC pEglMakeCurrent=nullptr;
    PFNEGLGETPROCADDRESSPROC pEglGetProcAddress=nullptr;
    PFNEGLDESTROYCONTEXTPROC pEglDestroyContext=nullptr;
    PFNEGLDESTROYSURFACEPROC pEglDestroySurface=nullptr;
    PFNEGLTERMINATEPROC pEglTerminate=nullptr;

    PFNEGLCREATEIMAGEKHRPROC pCreateImage=nullptr;
    PFNEGLDESTROYIMAGEKHRPROC pDestroyImage=nullptr;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC pGetNativeClientBuffer=nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTarget=nullptr;
    PFNGLBUFFERSTORAGEEXTPROC pBufferStorage=nullptr;

    AhbCreateFromHandleFn pAhbCreateFromHandle=nullptr;
    AhbReleaseFn pAhbRelease=nullptr;

    PFNGLGETSTRINGPROC pGlGetString=nullptr;
    PFNGLGETERRORPROC pGlGetError=nullptr;
    PFNGLCREATESHADERPROC pGlCreateShader=nullptr;
    PFNGLSHADERSOURCEPROC pGlShaderSource=nullptr;
    PFNGLCOMPILESHADERPROC pGlCompileShader=nullptr;
    PFNGLGETSHADERIVPROC pGlGetShaderiv=nullptr;
    PFNGLGETSHADERINFOLOGPROC pGlGetShaderInfoLog=nullptr;
    PFNGLCREATEPROGRAMPROC pGlCreateProgram=nullptr;
    PFNGLATTACHSHADERPROC pGlAttachShader=nullptr;
    PFNGLLINKPROGRAMPROC pGlLinkProgram=nullptr;
    PFNGLGETPROGRAMIVPROC pGlGetProgramiv=nullptr;
    PFNGLGETPROGRAMINFOLOGPROC pGlGetProgramInfoLog=nullptr;
    PFNGLDELETESHADERPROC pGlDeleteShader=nullptr;
    PFNGLDELETEPROGRAMPROC pGlDeleteProgram=nullptr;
    PFNGLUSEPROGRAMPROC pGlUseProgram=nullptr;
    PFNGLGETUNIFORMLOCATIONPROC pGlGetUniformLocation=nullptr;
    PFNGLUNIFORM1IPROC pGlUniform1i=nullptr;
    PFNGLUNIFORM1UIPROC pGlUniform1ui=nullptr;
    PFNGLGENTEXTURESPROC pGlGenTextures=nullptr;
    PFNGLDELETETEXTURESPROC pGlDeleteTextures=nullptr;
    PFNGLACTIVETEXTUREPROC pGlActiveTexture=nullptr;
    PFNGLBINDTEXTUREPROC pGlBindTexture=nullptr;
    PFNGLTEXPARAMETERIPROC pGlTexParameteri=nullptr;
    PFNGLGENBUFFERSPROC pGlGenBuffers=nullptr;
    PFNGLDELETEBUFFERSPROC pGlDeleteBuffers=nullptr;
    PFNGLBINDBUFFERPROC pGlBindBuffer=nullptr;
    PFNGLBINDBUFFERBASEPROC pGlBindBufferBase=nullptr;
    PFNGLMAPBUFFERRANGEPROC pGlMapBufferRange=nullptr;
    PFNGLUNMAPBUFFERPROC pGlUnmapBuffer=nullptr;
    PFNGLDISPATCHCOMPUTEPROC pGlDispatchCompute=nullptr;
    PFNGLMEMORYBARRIERPROC pGlMemoryBarrier=nullptr;
    PFNGLFENCESYNCPROC pGlFenceSync=nullptr;
    PFNGLCLIENTWAITSYNCPROC pGlClientWaitSync=nullptr;
    PFNGLDELETESYNCPROC pGlDeleteSync=nullptr;

    ~Impl() { destroy(); }

    bool requested() const {
        char v[PROP_VALUE_MAX]={};
        return __system_property_get("debug.arda.c2.gpu_i420",v)>0 && v[0]=='1';
    }

    void fail(const char *stage) {
        ++failures;
        ALOGW("ARDA B4 V2 GPU_FAIL_OPEN stage=%s gpu=%" PRIu64 " fallback=%" PRIu64 " failures=%" PRIu64,
              stage,gpuFrames,fallbackFrames,failures);
    }

    template <typename T>
    bool loadGl(T *out,const char *name) {
        void *p=dlsym(glesLib,name);
        if(!p && pEglGetProcAddress) {
            p=reinterpret_cast<void *>(pEglGetProcAddress(name));
        }
        *out=reinterpret_cast<T>(p);
        return *out != nullptr;
    }

    GLuint compileShader() {
        GLuint shader=pGlCreateShader(GL_COMPUTE_SHADER);
        if(!shader) return 0;
        const GLchar *src=kCompute;
        pGlShaderSource(shader,1,&src,nullptr);
        pGlCompileShader(shader);
        GLint ok=0;
        pGlGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
        if(!ok) {
            char log[1024]={};
            GLsizei n=0;
            pGlGetShaderInfoLog(shader,sizeof(log)-1,&n,log);
            ALOGE("ARDA B4 V2 GPU_SHADER_COMPILE_FAIL log=%s",log);
            pGlDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    GLuint linkProgram(GLuint shader) {
        GLuint p=pGlCreateProgram();
        if(!p) return 0;
        pGlAttachShader(p,shader);
        pGlLinkProgram(p);
        GLint ok=0;
        pGlGetProgramiv(p,GL_LINK_STATUS,&ok);
        if(!ok) {
            char log[1024]={};
            GLsizei n=0;
            pGlGetProgramInfoLog(p,sizeof(log)-1,&n,log);
            ALOGE("ARDA B4 V2 GPU_PROGRAM_LINK_FAIL log=%s",log);
            pGlDeleteProgram(p);
            return 0;
        }
        return p;
    }

    bool makeCurrent() {
        return dpy!=EGL_NO_DISPLAY && surf!=EGL_NO_SURFACE && ctx!=EGL_NO_CONTEXT
                && pEglMakeCurrent(dpy,surf,surf,ctx)==EGL_TRUE;
    }

    bool init(uint32_t w,uint32_t h) {
        if(ready && width==w && height==h) return true;
        if(ready) destroyGpuObjects();

        if((w%8)!=0 || (h%2)!=0 || w==0 || h==0) {
            fail("unsupported_dimensions");
            return false;
        }

        if(!dlAndroid) {
            dlAndroid=dlopen("libdl_android.so",RTLD_NOW|RTLD_LOCAL);
            if(!dlAndroid) { fail("dlopen_libdl_android"); return false; }

            auto getExported=sym<GetExportedNamespaceFn>(dlAndroid,"android_get_exported_namespace");
            auto createNs=sym<CreateNamespaceFn>(dlAndroid,"android_create_namespace");
            auto linkNs=sym<LinkNamespacesFn>(dlAndroid,"android_link_namespaces");
            if(!getExported||!createNs||!linkNs) { fail("namespace_symbols"); return false; }

            android_namespace_t *systemNs=getExported("system");
            if(!systemNs) { fail("system_namespace"); return false; }

            const char *search="/system/lib64:/system_ext/lib64";
            const char *permitted="/system/lib64:/system_ext/lib64";
            android_namespace_t *gpuNs=createNs(
                    "arda_c2_gpu_v2",search,search,kNamespaceIsolated,permitted,systemNs);
            if(!gpuNs) { fail("create_namespace"); return false; }
            if(!linkNs(gpuNs,systemNs,kGraphicsShared)) { fail("link_graphics_closure"); return false; }

            android_dlextinfo info={};
            info.flags=ANDROID_DLEXT_USE_NAMESPACE;
            info.library_namespace=gpuNs;

            nativewindowLib=android_dlopen_ext(
                    "libnativewindow.so",RTLD_NOW|RTLD_LOCAL,&info);
            eglLib=android_dlopen_ext("libEGL.so",RTLD_NOW|RTLD_LOCAL,&info);
            glesLib=android_dlopen_ext("libGLESv3.so",RTLD_NOW|RTLD_LOCAL,&info);
            if(!nativewindowLib||!eglLib||!glesLib) {
                fail("load_nativewindow_egl_gles");
                return false;
            }

            pAhbCreateFromHandle=sym<AhbCreateFromHandleFn>(
                    nativewindowLib,"AHardwareBuffer_createFromHandle");
            pAhbRelease=sym<AhbReleaseFn>(
                    nativewindowLib,"AHardwareBuffer_release");
            if(!pAhbCreateFromHandle||!pAhbRelease) {
                fail("ahardwarebuffer_symbols");
                return false;
            }

            pEglGetDisplay=sym<PFNEGLGETDISPLAYPROC>(eglLib,"eglGetDisplay");
            pEglInitialize=sym<PFNEGLINITIALIZEPROC>(eglLib,"eglInitialize");
            pEglChooseConfig=sym<PFNEGLCHOOSECONFIGPROC>(eglLib,"eglChooseConfig");
            pEglCreatePbufferSurface=sym<PFNEGLCREATEPBUFFERSURFACEPROC>(eglLib,"eglCreatePbufferSurface");
            pEglCreateContext=sym<PFNEGLCREATECONTEXTPROC>(eglLib,"eglCreateContext");
            pEglMakeCurrent=sym<PFNEGLMAKECURRENTPROC>(eglLib,"eglMakeCurrent");
            pEglGetProcAddress=sym<PFNEGLGETPROCADDRESSPROC>(eglLib,"eglGetProcAddress");
            pEglDestroyContext=sym<PFNEGLDESTROYCONTEXTPROC>(eglLib,"eglDestroyContext");
            pEglDestroySurface=sym<PFNEGLDESTROYSURFACEPROC>(eglLib,"eglDestroySurface");
            pEglTerminate=sym<PFNEGLTERMINATEPROC>(eglLib,"eglTerminate");

            if(!pEglGetDisplay||!pEglInitialize||!pEglChooseConfig||
               !pEglCreatePbufferSurface||!pEglCreateContext||!pEglMakeCurrent||
               !pEglGetProcAddress||!pEglDestroyContext||!pEglDestroySurface||!pEglTerminate) {
                fail("egl_core_symbols");
                return false;
            }

            dpy=pEglGetDisplay(EGL_DEFAULT_DISPLAY);
            if(dpy==EGL_NO_DISPLAY) { fail("eglGetDisplay"); return false; }

            EGLint major=0,minor=0;
            if(!pEglInitialize(dpy,&major,&minor)) { fail("eglInitialize"); return false; }

            const EGLint cfgAttrs[]={
                EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,
                EGL_NONE};
            EGLint count=0;
            if(!pEglChooseConfig(dpy,cfgAttrs,&cfg,1,&count)||count<1) {
                fail("eglChooseConfig"); return false;
            }

            const EGLint pbAttrs[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
            surf=pEglCreatePbufferSurface(dpy,cfg,pbAttrs);
            if(surf==EGL_NO_SURFACE) { fail("eglCreatePbufferSurface"); return false; }

            const EGLint ctxAttrs[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
            ctx=pEglCreateContext(dpy,cfg,EGL_NO_CONTEXT,ctxAttrs);
            if(ctx==EGL_NO_CONTEXT) { fail("eglCreateContext"); return false; }

            if(!makeCurrent()) { fail("eglMakeCurrent"); return false; }

            pCreateImage=reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(pEglGetProcAddress("eglCreateImageKHR"));
            pDestroyImage=reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(pEglGetProcAddress("eglDestroyImageKHR"));
            pGetNativeClientBuffer=reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
                    pEglGetProcAddress("eglGetNativeClientBufferANDROID"));
            pImageTarget=reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
                    pEglGetProcAddress("glEGLImageTargetTexture2DOES"));
            pBufferStorage=reinterpret_cast<PFNGLBUFFERSTORAGEEXTPROC>(
                    pEglGetProcAddress("glBufferStorageEXT"));

#define LOAD_GL(member,name) do { if(!loadGl(&(member),(name))) { fail(name); return false; } } while(0)
            LOAD_GL(pGlGetString,"glGetString");
            LOAD_GL(pGlGetError,"glGetError");
            LOAD_GL(pGlCreateShader,"glCreateShader");
            LOAD_GL(pGlShaderSource,"glShaderSource");
            LOAD_GL(pGlCompileShader,"glCompileShader");
            LOAD_GL(pGlGetShaderiv,"glGetShaderiv");
            LOAD_GL(pGlGetShaderInfoLog,"glGetShaderInfoLog");
            LOAD_GL(pGlCreateProgram,"glCreateProgram");
            LOAD_GL(pGlAttachShader,"glAttachShader");
            LOAD_GL(pGlLinkProgram,"glLinkProgram");
            LOAD_GL(pGlGetProgramiv,"glGetProgramiv");
            LOAD_GL(pGlGetProgramInfoLog,"glGetProgramInfoLog");
            LOAD_GL(pGlDeleteShader,"glDeleteShader");
            LOAD_GL(pGlDeleteProgram,"glDeleteProgram");
            LOAD_GL(pGlUseProgram,"glUseProgram");
            LOAD_GL(pGlGetUniformLocation,"glGetUniformLocation");
            LOAD_GL(pGlUniform1i,"glUniform1i");
            LOAD_GL(pGlUniform1ui,"glUniform1ui");
            LOAD_GL(pGlGenTextures,"glGenTextures");
            LOAD_GL(pGlDeleteTextures,"glDeleteTextures");
            LOAD_GL(pGlActiveTexture,"glActiveTexture");
            LOAD_GL(pGlBindTexture,"glBindTexture");
            LOAD_GL(pGlTexParameteri,"glTexParameteri");
            LOAD_GL(pGlGenBuffers,"glGenBuffers");
            LOAD_GL(pGlDeleteBuffers,"glDeleteBuffers");
            LOAD_GL(pGlBindBuffer,"glBindBuffer");
            LOAD_GL(pGlBindBufferBase,"glBindBufferBase");
            LOAD_GL(pGlMapBufferRange,"glMapBufferRange");
            LOAD_GL(pGlUnmapBuffer,"glUnmapBuffer");
            LOAD_GL(pGlDispatchCompute,"glDispatchCompute");
            LOAD_GL(pGlMemoryBarrier,"glMemoryBarrier");
            LOAD_GL(pGlFenceSync,"glFenceSync");
            LOAD_GL(pGlClientWaitSync,"glClientWaitSync");
            LOAD_GL(pGlDeleteSync,"glDeleteSync");
#undef LOAD_GL

            if(!pCreateImage||!pDestroyImage||!pGetNativeClientBuffer||
               !pImageTarget||!pBufferStorage) {
                fail("required_extension_procs");
                return false;
            }

            const char *ext=reinterpret_cast<const char *>(pGlGetString(GL_EXTENSIONS));
            if(!ext||!std::strstr(ext,"GL_EXT_buffer_storage")) {
                fail("GL_EXT_buffer_storage");
                return false;
            }

            ALOGI("ARDA B4 V2 GPU_INIT EGL=%d.%d vendor=%s renderer=%s version=%s",
                  major,minor,
                  reinterpret_cast<const char *>(pGlGetString(GL_VENDOR)),
                  reinterpret_cast<const char *>(pGlGetString(GL_RENDERER)),
                  reinterpret_cast<const char *>(pGlGetString(GL_VERSION)));
        } else if(!makeCurrent()) {
            fail("eglMakeCurrent_reuse");
            return false;
        }

        width=w;
        height=h;
        yBytes=static_cast<size_t>(w)*h;
        cBytes=yBytes/4;
        totalBytes=yBytes+2*cBytes;

        GLuint shader=compileShader();
        if(!shader) { fail("compile_shader"); return false; }
        program=linkProgram(shader);
        pGlDeleteShader(shader);
        if(!program) { fail("link_program"); return false; }

        pGlUseProgram(program);
        pGlUniform1i(pGlGetUniformLocation(program,"srcTex"),0);
        pGlUniform1i(pGlGetUniformLocation(program,"width"),static_cast<GLint>(w));
        pGlUniform1i(pGlGetUniformLocation(program,"height"),static_cast<GLint>(h));
        pGlUniform1ui(pGlGetUniformLocation(program,"yWordCount"),static_cast<GLuint>(yBytes/4));
        pGlUniform1ui(pGlGetUniformLocation(program,"cWordCount"),static_cast<GLuint>(cBytes/4));

        pGlGenTextures(1,&tex);
        pGlActiveTexture(GL_TEXTURE0);
        pGlBindTexture(GL_TEXTURE_2D,tex);
        pGlTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        pGlTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);

        pGlGenBuffers(1,&ssbo);
        pGlBindBuffer(GL_SHADER_STORAGE_BUFFER,ssbo);

        const GLbitfield flags=
                GL_MAP_READ_BIT|GL_MAP_PERSISTENT_BIT_EXT|GL_MAP_COHERENT_BIT_EXT;
        pBufferStorage(GL_SHADER_STORAGE_BUFFER,
                       static_cast<GLsizeiptr>(totalBytes),nullptr,flags);

        mapped=pGlMapBufferRange(
                GL_SHADER_STORAGE_BUFFER,0,static_cast<GLsizeiptr>(totalBytes),flags);
        if(!mapped) { fail("persistent_map"); return false; }

        pGlBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,ssbo);

        GLenum e=pGlGetError();
        if(e!=GL_NO_ERROR) {
            ALOGE("ARDA B4 V2 GPU_INIT_GL_ERROR=0x%x",e);
            fail("gpu_object_setup");
            return false;
        }

        ready=true;
        ALOGI("ARDA B4 V2 GPU_SLOT_READY %ux%u bytes=%zu ptr=%p",w,h,totalBytes,mapped);
        return true;
    }

    void destroyGpuObjects() {
        if(dpy!=EGL_NO_DISPLAY && ctx!=EGL_NO_CONTEXT && surf!=EGL_NO_SURFACE && pEglMakeCurrent) {
            (void)makeCurrent();
        }
        if(mapped && pGlBindBuffer && pGlUnmapBuffer) {
            pGlBindBuffer(GL_SHADER_STORAGE_BUFFER,ssbo);
            pGlUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        }
        mapped=nullptr;
        if(ssbo && pGlDeleteBuffers) pGlDeleteBuffers(1,&ssbo);
        if(tex && pGlDeleteTextures) pGlDeleteTextures(1,&tex);
        if(program && pGlDeleteProgram) pGlDeleteProgram(program);
        ssbo=0;
        tex=0;
        program=0;
        ready=false;
        held=false;
        heldBase=nullptr;
        width=height=0;
        yBytes=cBytes=totalBytes=0;
    }

    void destroy() {
        destroyGpuObjects();
        if(dpy!=EGL_NO_DISPLAY && pEglMakeCurrent) {
            pEglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        }
        if(ctx!=EGL_NO_CONTEXT && pEglDestroyContext) pEglDestroyContext(dpy,ctx);
        if(surf!=EGL_NO_SURFACE && pEglDestroySurface) pEglDestroySurface(dpy,surf);
        if(dpy!=EGL_NO_DISPLAY && pEglTerminate) pEglTerminate(dpy);
        ctx=EGL_NO_CONTEXT;
        surf=EGL_NO_SURFACE;
        dpy=EGL_NO_DISPLAY;
        if(glesLib) dlclose(glesLib);
        if(eglLib) dlclose(eglLib);
        if(nativewindowLib) dlclose(nativewindowLib);
        if(dlAndroid) dlclose(dlAndroid);
        glesLib=eglLib=nativewindowLib=dlAndroid=nullptr;
    }

    uint8_t *convert(const C2ConstGraphicBlock& block,uint32_t w,uint32_t h) {
        if(!requested()) return nullptr;

        if(held) {
            ++fallbackFrames;
            ++heldFallbacks;
            if((heldFallbacks%120)==1) {
                ALOGI("ARDA B4 V2 GPU_FALLBACK reason=slot_held held=%" PRIu64,heldFallbacks);
            }
            return nullptr;
        }

        auto poolData=_C2BlockFactory::GetGraphicBlockPoolData(block);

        AHardwareBuffer *ahb=nullptr;
        bool ownsAhb=false;
        const int poolType=poolData
                ? static_cast<int>(poolData->getType())
                : -1;

        if(poolData &&
           poolData->getType()==_C2BlockPoolData::TYPE_AHWBUFFER) {
            if(!_C2BlockFactory::GetAHardwareBuffer(poolData,&ahb) || !ahb) {
                ++fallbackFrames;
                fail("get_ahardwarebuffer");
                return nullptr;
            }
        } else {
            if(!init(w,h)) {
                ++fallbackFrames;
                return nullptr;
            }

            const C2Handle *c2Handle=block.handle();
            if(!c2Handle) {
                ++fallbackFrames;
                fail("missing_graphic_block_handle");
                return nullptr;
            }

            uint32_t metaWidth=0;
            uint32_t metaHeight=0;
            uint32_t metaFormat=0;
            uint64_t metaUsage=0;
            uint32_t metaStride=0;
            uint32_t metaGeneration=0;
            uint64_t metaBqId=0;
            uint32_t metaBqSlot=0;

            ::android::_UnwrapNativeCodec2GrallocMetadata(
                    c2Handle,
                    &metaWidth,
                    &metaHeight,
                    &metaFormat,
                    &metaUsage,
                    &metaStride,
                    &metaGeneration,
                    &metaBqId,
                    &metaBqSlot);

            native_handle_t *grallocHandle=
                    ::android::UnwrapNativeCodec2GrallocHandle(c2Handle);
            if(!grallocHandle) {
                ALOGW("ARDA B4 V2 HANDLE_UNWRAP_FAIL poolType=%d",poolType);
                ++fallbackFrames;
                fail("unwrap_graphic_block_gralloc_handle");
                return nullptr;
            }

            AHardwareBuffer_Desc desc={};
            desc.width=metaWidth;
            desc.height=metaHeight;
            desc.layers=1;
            desc.format=metaFormat;
            desc.usage=metaUsage;
            desc.stride=metaStride;

            const int ahbRc=pAhbCreateFromHandle(
                    &desc,
                    grallocHandle,
                    kAhbCreateFromHandleClone,
                    &ahb);
            native_handle_delete(grallocHandle);

            if(ahbRc!=0 || !ahb) {
                ALOGW("ARDA B4 V2 HANDLE_AHB_CREATE_FAIL rc=%d poolType=%d "
                      "w=%u h=%u fmt=%u usage=%" PRIu64
                      " stride=%u gen=%u bqid=%" PRIu64 " slot=%u",
                      ahbRc,poolType,
                      metaWidth,metaHeight,metaFormat,metaUsage,metaStride,
                      metaGeneration,metaBqId,metaBqSlot);
                ++fallbackFrames;
                fail("graphic_handle_ahardwarebuffer_create");
                return nullptr;
            }
            ownsAhb=true;

            static bool loggedHandleBridge=false;
            if(!loggedHandleBridge) {
                loggedHandleBridge=true;
                ALOGI("ARDA B4 V2 HANDLE_AHB_BRIDGE=PASS poolType=%d "
                      "w=%u h=%u fmt=%u usage=%" PRIu64
                      " stride=%u gen=%u bqid=%" PRIu64 " slot=%u",
                      poolType,
                      metaWidth,metaHeight,metaFormat,metaUsage,metaStride,
                      metaGeneration,metaBqId,metaBqSlot);
            }
        }

        if(!init(w,h)) {
            if(ownsAhb && ahb) pAhbRelease(ahb);
            ++fallbackFrames;
            return nullptr;
        }
        if(!makeCurrent()) {
            if(ownsAhb && ahb) pAhbRelease(ahb);
            ++fallbackFrames;
            fail("make_current_convert");
            return nullptr;
        }

        EGLClientBuffer client=pGetNativeClientBuffer(ahb);
        if(!client) {
            if(ownsAhb && ahb) pAhbRelease(ahb);
            ++fallbackFrames;
            fail("native_client_buffer");
            return nullptr;
        }

        const EGLint attrs[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
        EGLImageKHR image=pCreateImage(
                dpy,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,client,attrs);
        if(image==EGL_NO_IMAGE_KHR) {
            if(ownsAhb && ahb) pAhbRelease(ahb);
            ++fallbackFrames;
            fail("eglCreateImageKHR");
            return nullptr;
        }

        pGlUseProgram(program);
        pGlActiveTexture(GL_TEXTURE0);
        pGlBindTexture(GL_TEXTURE_2D,tex);
        pImageTarget(GL_TEXTURE_2D,reinterpret_cast<GLeglImageOES>(image));
        pGlBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,ssbo);

        auto t0=std::chrono::steady_clock::now();
        pGlDispatchCompute((w+63)/64,(h+15)/16,1);
        pGlMemoryBarrier(
                GL_SHADER_STORAGE_BARRIER_BIT|
                GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT_EXT);

        GLsync fence=pGlFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
        if(!fence) {
            pDestroyImage(dpy,image);
            if(ownsAhb && ahb) pAhbRelease(ahb);
            ++fallbackFrames;
            fail("glFenceSync");
            return nullptr;
        }

        GLenum wait=GL_TIMEOUT_EXPIRED;
        for(int i=0;i<20 && wait==GL_TIMEOUT_EXPIRED;++i) {
            wait=pGlClientWaitSync(
                    fence,GL_SYNC_FLUSH_COMMANDS_BIT,100000000ULL);
        }
        pGlDeleteSync(fence);
        pDestroyImage(dpy,image);
        if(ownsAhb && ahb) {
            pAhbRelease(ahb);
            ahb=nullptr;
        }

        if(wait!=GL_ALREADY_SIGNALED && wait!=GL_CONDITION_SATISFIED) {
            ++fallbackFrames;
            fail("gpu_fence_wait");
            return nullptr;
        }

        GLenum e=pGlGetError();
        if(e!=GL_NO_ERROR) {
            ALOGE("ARDA B4 V2 GPU_FRAME_GL_ERROR=0x%x",e);
            ++fallbackFrames;
            fail("frame_gl_error");
            return nullptr;
        }

        ++gpuFrames;
        const double ms=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-t0).count();
        if(gpuFrames<=8 || (gpuFrames%120)==0) {
            ALOGI("ARDA B4 V2 GPU_FRAME=PASS n=%" PRIu64 " ms=%.3f ptr=%p",
                  gpuFrames,ms,mapped);
        }
        return static_cast<uint8_t *>(mapped);
    }
};

ArdaGpuI420::ArdaGpuI420() : mImpl(std::make_unique<Impl>()) {}
ArdaGpuI420::~ArdaGpuI420() = default;

bool ArdaGpuI420::requested() const {
    return mImpl->requested();
}

uint8_t* ArdaGpuI420::tryConvert(
        const C2ConstGraphicBlock& block,uint32_t width,uint32_t height) {
    return mImpl->convert(block,width,height);
}

void ArdaGpuI420::markSubmitted(const void* base) {
    if(base && base==mImpl->mapped) {
        mImpl->held=true;
        mImpl->heldBase=base;
        ALOGV("ARDA B4 V2 GPU_SLOT_HELD ptr=%p",base);
    }
}

void ArdaGpuI420::releaseIfFreed(const void* freed) {
    if(freed && mImpl->held && freed==mImpl->heldBase) {
        mImpl->held=false;
        mImpl->heldBase=nullptr;
        ALOGV("ARDA B4 V2 GPU_SLOT_RELEASE freed=%p",freed);
    }
}

bool ArdaGpuI420::slotHeld() const {
    return mImpl->held;
}

const void* ArdaGpuI420::slotBase() const {
    return mImpl->heldBase;
}

void ArdaGpuI420::reset() {
    mImpl->destroyGpuObjects();
}

} // namespace android
