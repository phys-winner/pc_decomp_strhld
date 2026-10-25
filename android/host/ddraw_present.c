/* RGB565 DirectDraw presentation on the GPU. An in-game frame arrives as the
 * world raster and up to three captured UI layers (display.c); the world and
 * the UI are upsampled with their own filters (bilinear, sharp pixels,
 * bicubic, AMD FSR 1), the UI as premultiplied alpha over the world, then
 * the armed-button glow, crisp text and the placement loupe are drawn on
 * top. Frames without composition (menus, videos) use the UI filter. Only
 * changed layer rows are uploaded. Without OpenGL ES 3 the CPU composite
 * (stronghold_display_compose) is drawn with one filter, as before; it is
 * also the frame dump oracle. */
#include "host.h"
#include "display.h"
#include "feedback.h"
#include "text_overlay.h"
#include "fsr1.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <stdatomic.h>

__attribute__((weak)) void *void_android_window_current(uint32_t *serial,int wait)
{ (void)wait;*serial=0;return NULL; }
__attribute__((weak)) void void_android_window_ack(uint32_t serial){(void)serial;}
__attribute__((weak)) void void_android_frame_presented(void){}

static EGLDisplay display=EGL_NO_DISPLAY;
static EGLContext context=EGL_NO_CONTEXT;
static EGLSurface window_surface=EGL_NO_SURFACE,pbuffer=EGL_NO_SURFACE;
static EGLConfig config;
static uint32_t window_serial;
static int gles3,failed,dump_next,dump_every;
static uint64_t frame_count,report_frame,report_ns;
static uint8_t *upload_buffer;
static size_t upload_capacity;
static _Atomic int loupe_size = 100, loupe_zoom = 200;

void stronghold_present_loupe_settings(int size,int zoom)
{
    atomic_store(&loupe_size,size < 50 ? 50 : size > 200 ? 200 : size);
    atomic_store(&loupe_zoom,zoom < 150 ? 150 : zoom > 400 ? 400 : zoom);
}

void d3d_request_frame_dump(void){dump_next=1;}
void d3d_request_trace(void){dump_next=1;}

static GLuint compile_shader(GLenum kind,int count,const char *const *sources,const char *what)
{
    GLuint s=glCreateShader(kind);glShaderSource(s,count,sources,NULL);glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[1024];glGetShaderInfoLog(s,sizeof log,NULL,log);void_log("[ddraw] %s shader: %s",what,log);glDeleteShader(s);return 0;}
    return s;
}
static GLuint link_program(GLuint v,GLuint f,const char *what)
{
    if(!v||!f){if(v)glDeleteShader(v);if(f)glDeleteShader(f);return 0;}
    GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glBindAttribLocation(p,0,"p");glLinkProgram(p);
    glDeleteShader(v);glDeleteShader(f);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){char log[1024];glGetProgramInfoLog(p,sizeof log,NULL,log);void_log("[ddraw] %s link: %s",what,log);glDeleteProgram(p);return 0;}
    return p;
}

/* ---- OpenGL ES 2 fallback: the CPU composite through one filter. ----
 * 0 hardware bilinear; 1 sharp pixels: nearest inside a source pixel with a
 * one-output-pixel blend at its edges; 2 Catmull-Rom bicubic in nine bilinear
 * taps. Modes 0 and 2 take optional contrast-adaptive sharpening on the source
 * grid (AMD FidelityFX CAS weights), limited by local contrast so flat areas
 * and already sharp edges do not ring. */
static GLuint programs[3],texture;
static GLint size_uniform[3],scale_uniform[3],sharp_uniform[3];
static int tex_w,tex_h,tex_bpp;
static const char *filter_source=
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
    "varying vec2 uv;uniform sampler2D image;uniform vec2 size;uniform vec2 scale;uniform float sharp;\n"
    "vec4 tap(vec2 t){return texture2D(image,t);}\n"
    "#if MODE==1\n"
    "vec4 filtered(vec2 t){vec2 p=t*size,f=floor(p),c=p-f-0.5,r=max(vec2(0.0),0.5-0.5/scale);"
    "return tap((f+(c-clamp(c,-r,r))*scale+0.5)/size);}\n"
    "#elif MODE==2\n"
    "vec4 filtered(vec2 t){vec2 p=t*size,t1=floor(p-0.5)+0.5,f=p-t1;"
    "vec2 w0=f*(-0.5+f*(1.0-0.5*f)),w1=1.0+f*f*(-2.5+1.5*f),w2=f*(0.5+f*(2.0-1.5*f)),w3=f*f*(-0.5+0.5*f);"
    "vec2 w12=w1+w2,t0=(t1-1.0)/size,t3=(t1+2.0)/size,tc=(t1+w2/w12)/size;"
    "return tap(t0)*w0.x*w0.y+tap(vec2(tc.x,t0.y))*w12.x*w0.y+tap(vec2(t3.x,t0.y))*w3.x*w0.y"
    "+tap(vec2(t0.x,tc.y))*w0.x*w12.y+tap(tc)*w12.x*w12.y+tap(vec2(t3.x,tc.y))*w3.x*w12.y"
    "+tap(vec2(t0.x,t3.y))*w0.x*w3.y+tap(vec2(tc.x,t3.y))*w12.x*w3.y+tap(t3)*w3.x*w3.y;}\n"
    "#else\n"
    "vec4 filtered(vec2 t){return tap(t);}\n"
    "#endif\n"
    "void main(){vec3 c=clamp(filtered(uv).rgb,0.0,1.0);\n"
    "#if MODE!=1\n"
    "if(sharp>0.0){vec2 d=1.0/size;vec3 n=tap(uv-vec2(0.0,d.y)).rgb,s=tap(uv+vec2(0.0,d.y)).rgb,"
    "e=tap(uv+vec2(d.x,0.0)).rgb,w=tap(uv-vec2(d.x,0.0)).rgb;"
    "vec3 mn=min(c,min(min(n,s),min(e,w))),mx=max(c,max(max(n,s),max(e,w)));"
    "vec3 k=sqrt(clamp(min(mn,2.0-mx)/max(mx,vec3(1.0/255.0)),0.0,1.0))*(-1.0/mix(8.0,5.0,sharp));"
    "c=clamp((c+(n+s+e+w)*k)/(1.0+4.0*k),0.0,1.0);}\n"
    "#endif\n"
    "gl_FragColor=vec4(c,1.0);}\n";

static GLuint link_filter(int mode)
{
    static const char *vs="attribute vec2 p;varying vec2 uv;void main(){gl_Position=vec4(p,0.0,1.0);uv=vec2((p.x+1.0)*0.5,(1.0-p.y)*0.5);}";
    char define[24];snprintf(define,sizeof define,"#define MODE %d\n",mode);
    const char *parts[2]={define,filter_source};
    return link_program(compile_shader(GL_VERTEX_SHADER,1,&vs,"filter"),compile_shader(GL_FRAGMENT_SHADER,2,parts,"filter"),"filter");
}
static int initialize_gles2(void)
{
    for(int mode=0;mode<3;mode++)if(!(programs[mode]=link_filter(mode))){
        if(!mode)return 0;
        programs[mode]=programs[0];
        void_log("[ddraw] image filter %d unavailable; bilinear is used",mode);
    }
    for(int mode=0;mode<3;mode++){
        glUseProgram(programs[mode]);glUniform1i(glGetUniformLocation(programs[mode],"image"),0);
        size_uniform[mode]=glGetUniformLocation(programs[mode],"size");
        scale_uniform[mode]=glGetUniformLocation(programs[mode],"scale");
        sharp_uniform[mode]=glGetUniformLocation(programs[mode],"sharp");
    }
    glGenTextures(1,&texture);
    glBindTexture(GL_TEXTURE_2D,texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);
    return 1;
}
/* 16-bit frames upload as they are; others become RGBA8 (BGR byte order). */
static const uint8_t *frame_upload(const uint8_t *px,int w,int h,int pitch,int bpp,GLenum *format,GLenum *type)
{
    *format=GL_RGB;*type=GL_UNSIGNED_SHORT_5_6_5;
    if(bpp==16&&pitch==w*2)return px;
    size_t needed=(size_t)w*h*(bpp==16?2:4);
    if(needed>upload_capacity){uint8_t *p=realloc(upload_buffer,needed);if(!p)return NULL;upload_buffer=p;upload_capacity=needed;}
    if(bpp==16)for(int y=0;y<h;y++)memcpy(upload_buffer+(size_t)y*w*2,px+(size_t)y*pitch,(size_t)w*2);
    else {*format=GL_RGBA;*type=GL_UNSIGNED_BYTE;
        for(int y=0;y<h;y++)for(int x=0;x<w;x++){const uint8_t *s=px+(size_t)y*pitch+x*(bpp/8);uint8_t *d=upload_buffer+((size_t)y*w+x)*4;
            d[0]=bpp==8?s[0]:s[2];d[1]=bpp==8?s[0]:s[1];d[2]=s[0];d[3]=255;}
    }
    return upload_buffer;
}
static void present_gles2(const uint8_t *px,int w,int h,int pitch,int bpp,int ww,int hh,int layered)
{
    GLenum format,type;const uint8_t *data=frame_upload(px,w,h,pitch,bpp,&format,&type);
    if(!data)return;
    glPixelStorei(GL_UNPACK_ALIGNMENT,format==GL_RGB?2:4);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);
    if(w!=tex_w||h!=tex_h||bpp!=tex_bpp){glTexImage2D(GL_TEXTURE_2D,0,format,w,h,0,format,type,data);tex_w=w;tex_h=h;tex_bpp=bpp;}
    else glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,format,type,data);
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    glViewport(0,0,ww,hh);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    double scale=(double)ww/w;if((double)hh/h<scale)scale=(double)hh/h;
    int vw=(int)(w*scale),vh=(int)(h*scale);glViewport((ww-vw)/2,(hh-vh)/2,vw,vh);
    float sharpness;int mode=stronghold_display_image_filter(&sharpness);
    static const GLfloat positions[]={-1,-1,3,-1,-1,3};glUseProgram(programs[mode]);
    glUniform2f(size_uniform[mode],(float)w,(float)h);
    glUniform2f(scale_uniform[mode],(float)vw/(float)w,(float)vh/(float)h);
    glUniform1f(sharp_uniform[mode],sharpness);
    glBindBuffer(GL_ARRAY_BUFFER,0);glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,positions);glDrawArrays(GL_TRIANGLES,0,3);
    glViewport(0,0,ww,hh);
    /* The layers are already one image here: their texts go on top in order. */
    if(layered)for(int p=0;p<SH_UI_LAYERS;p++)stronghold_text_overlay_draw(SH_UI_ORDER[p],ww,hh);
    else stronghold_text_overlay_draw(-1,ww,hh);
}

/* ---- OpenGL ES 3 pipeline ----
 * Coordinates: "window" pixels have their origin top-left (gl_FragCoord is
 * flipped by `target`), source positions are in texels, texel i spanning
 * [i,i+1), and src = window * map.xy + map.zw. Passes that produce textures
 * keep the image layout (texel row 0 = first image row = gl_FragCoord.y 0.5). */
static const char gl3_vertex[]=
    "#version 300 es\n"
    "layout(location=0) in vec2 p;uniform highp vec4 rect;uniform highp vec4 ndc;\n"
    "void main(){gl_Position=vec4(mix(rect.xy,rect.zw,p)*ndc.xy+ndc.zw,0.0,1.0);}\n";
static const char gl3_common[]=
    "precision mediump float;precision highp int;precision highp sampler2D;\n"
    "uniform highp sampler2D image;uniform highp sampler2D image2;\n"
    "uniform highp vec4 map;uniform highp vec4 size;uniform highp float target;uniform highp vec4 clip;\n"
    "uniform mediump float sharp;uniform highp vec2 outscale;uniform mediump float dither;\n"
    "layout(location=0) out mediump vec4 color;\n"
    "highp vec2 window_pos(){return vec2(gl_FragCoord.x,target-gl_FragCoord.y);}\n"
    /* Interleaved gradient noise in [0,1), for sub-LSB dithering. */
    "float noise(){highp float n=fract(52.9829189*fract(dot(gl_FragCoord.xy,vec2(0.06711056,0.00583715))));return n;}\n"
    "#if KIND==0&&FILTER==3\n"
    /* EASU taps, nearest sampler: the textures repeat their edge texels
     * beyond the valid area; the loupe can look further and clamps. */
    "#if LOUPE\n"
    "#define FSR_EASU_BASE(fp) highp vec2 fb=fp;\n"
    "#define T(x,y) texture(image,(clamp(fb+vec2(float(x),float(y)),clip.xy,clip.zw-1.0)+0.5)*size.zw)\n"
    "#else\n"
    "#define FSR_EASU_BASE(fp) highp vec2 fb=(fp+0.5)*size.zw;\n"
    "#define T(x,y) texture(image,fb+vec2(float(x),float(y))*size.zw)\n"
    "#endif\n"
    "#endif\n"
    "#if KIND==1\n"
    "uniform highp vec2 offset;\n"
    "#define FSR_RCAS_BASE highp vec2 q=(gl_FragCoord.xy-offset)*size.zw;\n"
    "#define L(x,y) texture(image,q+vec2(float(x),float(y))*size.zw)\n"
    "#endif\n";
static const char gl3_main[]=
    "#if KIND==0\n"
    /* Upsampling filters; taps stay inside the valid texels. */
    "vec4 tap(highp vec2 p){return texture(image,clamp(p,clip.xy+0.5,clip.zw-0.5)*size.zw);}\n"
    "#if FILTER==0\n"
    "vec4 filtered(highp vec2 p){return tap(p);}\n"
    "#elif FILTER==1\n"
    /* Sharp pixels: nearest inside a texel, a one-output-pixel blend at its edges. */
    "vec4 filtered(highp vec2 p){highp vec2 f=floor(p),c=p-f-0.5,r=max(vec2(0.0),0.5-0.5/outscale);"
    "return tap(f+(c-clamp(c,-r,r))*outscale+0.5);}\n"
    "#elif FILTER==2\n"
    /* Catmull-Rom bicubic in nine bilinear taps. */
    "vec4 filtered(highp vec2 p){highp vec2 t1=floor(p-0.5)+0.5,f=p-t1;\n"
    " highp vec2 w0=f*(-0.5+f*(1.0-0.5*f)),w1=1.0+f*f*(-2.5+1.5*f),w2=f*(0.5+f*(2.0-1.5*f)),w3=f*f*(-0.5+0.5*f);\n"
    " highp vec2 w12=w1+w2,t0=t1-1.0,t3=t1+2.0,tc=t1+w2/w12;\n"
    " return tap(t0)*w0.x*w0.y+tap(vec2(tc.x,t0.y))*w12.x*w0.y+tap(vec2(t3.x,t0.y))*w3.x*w0.y\n"
    "  +tap(vec2(t0.x,tc.y))*w0.x*w12.y+tap(tc)*w12.x*w12.y+tap(vec2(t3.x,tc.y))*w3.x*w12.y\n"
    "  +tap(vec2(t0.x,t3.y))*w0.x*w3.y+tap(vec2(tc.x,t3.y))*w12.x*w3.y+tap(t3)*w3.x*w3.y;}\n"
    "#else\n"
    "vec4 filtered(highp vec2 p){return FsrEasuH(p);}\n"
    "#endif\n"
    "#if LOUPE\n"
    "uniform highp vec4 loupe;uniform highp vec4 loupe_src;\n"
    "#endif\n"
    "void main(){\n"
    "#if LOUPE\n"
    " highp vec2 d=window_pos()-loupe.xy;highp float r=length(d);\n"
    " if(r>loupe.z+1.0)discard;\n"
    " highp vec2 p=loupe_src.xy+d*loupe_src.zw;\n"
    "#else\n"
    " highp vec2 p=window_pos()*map.xy+map.zw;\n"
    "#endif\n"
    " vec4 c=clamp(filtered(p),0.0,1.0);\n"
    "#if FILTER==0||FILTER==2\n"
    /* Contrast-adaptive sharpening on the source grid (AMD FidelityFX CAS
     * weights), limited by local contrast so flat areas and sharp edges do not ring. */
    " if(sharp>0.0){vec4 n=tap(p-vec2(0.0,1.0)),s=tap(p+vec2(0.0,1.0)),e=tap(p+vec2(1.0,0.0)),w=tap(p-vec2(1.0,0.0));\n"
    "  vec4 mn=min(c,min(min(n,s),min(e,w))),mx=max(c,max(max(n,s),max(e,w)));\n"
    "  vec4 k=sqrt(clamp(min(mn,2.0-mx)/max(mx,vec4(1.0/255.0)),0.0,1.0))*(-1.0/mix(8.0,5.0,sharp));\n"
    "  c=clamp((c+(n+s+e+w)*k)/(1.0+4.0*k),0.0,1.0);}\n"
    "#endif\n"
    "#if FSR_ALPHA\n"
    " c.rgb=min(c.rgb,c.aaa);\n"
    "#else\n"
    " c=vec4(c.rgb+(noise()-0.5)*dither,1.0);\n"
    "#endif\n"
    "#if LOUPE\n"
    /* Crosshair at the finger point: white core with a dark edge. */
    " highp vec2 ad=abs(d);highp float arm=max(ad.x,ad.y),across=min(ad.x,ad.y);\n"
    " float inarm=step(loupe.z*0.04,arm)*step(arm,loupe.z*0.17);\n"
    " c.rgb=mix(c.rgb,vec3(0.06),inarm*(1.0-smoothstep(1.6,2.4,float(across)))*0.8);\n"
    " c.rgb=mix(c.rgb,vec3(1.0),inarm*(1.0-smoothstep(0.5,1.2,float(across))));\n"
    /* Gold rim #E7B45C, lit from above, with dark lines on both sides. */
    " highp float inner=loupe.z-loupe.w;\n"
    " vec3 gold=vec3(0.906,0.706,0.361)*(1.0-0.18*float(d.y/max(r,1.0)));\n"
    " c.rgb=mix(c.rgb,gold,smoothstep(inner-0.7,inner+0.7,float(r)));\n"
    " float line=max(1.0-smoothstep(0.3,1.3,abs(float(r-inner))),1.0-smoothstep(0.3,1.3,abs(float(r-loupe.z+0.9))));\n"
    " c.rgb=mix(c.rgb,vec3(0.24,0.16,0.05),line*0.85);\n"
    " float a=1.0-smoothstep(loupe.z-0.7,loupe.z+0.7,float(r));\n"
    " c=vec4(c.rgb*a,a);\n"
    "#endif\n"
    " color=c;}\n"
    "#elif KIND==1\n"
    "void main(){vec4 c=FsrRcasH(sharp);\n"
    "#if FSR_ALPHA\n"
    " color=clamp(c,0.0,1.0);\n"
    "#else\n"
    " color=vec4(clamp(c.rgb,0.0,1.0)+(noise()-0.5)*dither,1.0);\n"
    "#endif\n"
    "}\n"
    "#elif KIND==2\n"
    /* World (or frame) source: RGB565 to RGBA, edge texels repeated beyond
     * the raster. Deband averages ring samples within one RGB565 step of
     * the pixel (none past two), only where its neighbours are that smooth:
     * gradient steps become ramps, edges and texture stay. */
    "uniform highp ivec4 iclip;\n"
    "vec3 at(ivec2 p){return texelFetch(image,clamp(p,iclip.xy,iclip.zw-1),0).rgb;}\n"
    "float steps(vec3 a,vec3 b){vec3 d=abs(a-b)*vec3(31.0,63.0,31.0);return max(d.r,max(d.g,d.b));}\n"
    "void main(){ivec2 p=min(ivec2(gl_FragCoord.xy),iclip.zw-1);vec3 c=at(p);\n"
    "#if DEBAND\n"
    " float near=max(max(steps(at(p+ivec2(1,0)),c),steps(at(p-ivec2(1,0)),c)),max(steps(at(p+ivec2(0,1)),c),steps(at(p-ivec2(0,1)),c)));\n"
    " if(near<=1.5){\n"
    "  highp float a=6.2831853*fract(52.9829189*fract(dot(vec2(p),vec2(0.06711056,0.00583715))));\n"
    "  vec2 dir=vec2(cos(a),sin(a));vec3 sum=c;float total=1.0;\n"
    "  for(int ring=0;ring<3;ring++){\n"
    "   float radius=ring==0?2.5:ring==1?5.5:9.5;\n"
    "   for(int k=0;k<4;k++){vec3 s=at(p+ivec2(round(dir*radius)));float w=clamp(2.0-steps(s,c),0.0,1.0);\n"
    "    sum+=s*w;total+=w;dir=vec2(-dir.y,dir.x);}\n"
    "   dir=vec2(dir.x*0.8660254-dir.y*0.5,dir.x*0.5+dir.y*0.8660254);}\n"
    "  c=sum/total;}\n"
    "#endif\n"
    " color=vec4(c+(noise()-0.5)*dither,1.0);}\n"
    "#elif KIND==3\n"
    /* UI premultiply: premul = pixel - key*(1-alpha), zero where uncovered
     * (whatever RGB the layer holds there); rows clamp to the part's window. */
    "uniform highp ivec2 rows;uniform mediump vec3 key;\n"
    "void main(){ivec2 p=ivec2(gl_FragCoord.xy);p.y=clamp(p.y,rows.x,rows.y-1);\n"
    " float a=texelFetch(image2,p,0).r;vec3 c=texelFetch(image,p,0).rgb;\n"
    " color=a>0.0?vec4(max(c-key*(1.0-a),0.0),a):vec4(0.0);}\n"
    "#else\n"
    /* Armed construction button: soft, slowly pulsing light over its own
     * UI-covered pixels (blended as dst + (1-dst)*src). */
    "uniform highp vec4 fmap;uniform highp vec4 glow;uniform mediump float pulse;\n"
    "void main(){highp vec2 w=window_pos();highp vec2 d=((w*fmap.xy+fmap.zw)-glow.xy)*glow.zw;\n"
    " float k=(0.16+0.30*max(0.0,1.0-float(dot(d,d))))*pulse;\n"
    " float m=clamp(texture(image,(w*map.xy+map.zw)*size.zw).a*4.0,0.0,1.0);\n"
    " color=vec4(k,k,0.7*k,0.0)*m;}\n"
    "#endif\n";

enum { K_FILTER, K_RCAS, K_SOURCE, K_PREMUL, K_GLOW };
#define P_FILTER(filter,alpha,loupe) ((filter)*4+(alpha)*2+(loupe))
#define P_RCAS(alpha) (16+(alpha))
#define P_SOURCE(deband) (18+(deband))
#define P_PREMUL 20
#define P_GLOW 21
#define PROGRAMS 22
typedef struct { GLint rect,ndc,map,size,target,clip,sharp,outscale,dither,iclip,rows,key,fmap,glow,pulse,loupe,loupe_src,offset; } locations;
static GLuint gl3_programs[PROGRAMS];
static locations gl3_uniforms[PROGRAMS];
static char gl3_failed[PROGRAMS];
static GLuint quad_buffer,sampler_nearest,sampler_linear;
static int half_float_target;

static GLuint gl3_program(int id)
{
    if(gl3_programs[id]||gl3_failed[id])return gl3_programs[id];
    int kind=id<16?K_FILTER:id<18?K_RCAS:id<20?K_SOURCE:id==P_PREMUL?K_PREMUL:K_GLOW;
    int filter=id<16?id/4:0,alpha=id<16?(id>>1)&1:id<18?id-16:0,loupe=id<16?id&1:0,deband=id==P_SOURCE(1);
    char defines[160];
    snprintf(defines,sizeof defines,"#define KIND %d\n#define FILTER %d\n#define FSR_ALPHA %d\n#define LOUPE %d\n#define DEBAND %d\n",
             kind,filter,alpha,loupe,deband);
    const char *fragment[6]={"#version 300 es\n",defines,gl3_common,kind==K_FILTER&&filter==SH_FILTER_FSR?fsr1_easu_glsl:"",
                             kind==K_RCAS?fsr1_rcas_glsl:"",gl3_main};
    const char *vertex=gl3_vertex;
    char what[32];snprintf(what,sizeof what,"program %d",id);
    GLuint p=link_program(compile_shader(GL_VERTEX_SHADER,1,&vertex,what),compile_shader(GL_FRAGMENT_SHADER,6,fragment,what),what);
    if(!p){gl3_failed[id]=1;return 0;}
    locations *u=&gl3_uniforms[id];
#define U(name) u->name=glGetUniformLocation(p,#name)
    U(rect);U(ndc);U(map);U(size);U(target);U(clip);U(sharp);U(outscale);U(dither);U(iclip);U(rows);U(key);
    U(fmap);U(glow);U(pulse);U(loupe);U(loupe_src);U(offset);
#undef U
    glUseProgram(p);glUniform1i(glGetUniformLocation(p,"image"),0);glUniform1i(glGetUniformLocation(p,"image2"),1);
    return gl3_programs[id]=p;
}
/* A filter program, falling back to bilinear if a variant cannot be built. */
static int filter_program(int filter,int alpha,int loupe)
{
    int id=P_FILTER(filter,alpha,loupe);
    if(!gl3_program(id))id=P_FILTER(SH_FILTER_BILINEAR,alpha,loupe);
    return gl3_program(id)?id:-1;
}
static const locations *use(int id){glUseProgram(gl3_programs[id]);return &gl3_uniforms[id];}
/* Window targets flip y (top-left origin); texture targets keep the image layout. */
static void quad(const locations *u,float x0,float y0,float x1,float y1,int tw,int th,int window)
{
    glUniform4f(u->rect,x0,y0,x1,y1);
    glUniform4f(u->ndc,2.f/tw,window?-2.f/th:2.f/th,-1,window?1:-1);
    if(u->target>=0)glUniform1f(u->target,(float)th);
    glBindBuffer(GL_ARRAY_BUFFER,quad_buffer);glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);
    glDisableVertexAttribArray(1); /* the text overlay may leave it on */
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}
static void bind(int unit,GLuint texture_name,GLuint sampler)
{
    glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,texture_name);glBindSampler(unit,sampler);
}
static void scissor(float x0,float y0,float x1,float y1,int th,int window)
{
    int l=(int)floorf(x0),r=(int)ceilf(x1),t=(int)floorf(y0),b=(int)ceilf(y1);
    if(l<0)l=0;if(t<0)t=0;
    glEnable(GL_SCISSOR_TEST);
    if(window)glScissor(l,th-b,r>l?r-l:0,b>t?b-t:0);else glScissor(l,t,r>l?r-l:0,b>t?b-t:0);
}

static GLuint texture2d(GLenum format,int w,int h)
{
    GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);glTexStorage2D(GL_TEXTURE_2D,1,format,w,h);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    return t;
}
static GLuint target_for(GLuint t)
{
    GLuint f;glGenFramebuffers(1,&f);glBindFramebuffer(GL_FRAMEBUFFER,f);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){glDeleteFramebuffers(1,&f);return 0;}
    return f;
}
/* A target whose old contents are never read again: nothing to load. */
static void begin_target(GLuint f)
{
    static const GLenum color=GL_COLOR_ATTACHMENT0;
    glBindFramebuffer(GL_FRAMEBUFFER,f);glInvalidateFramebuffer(GL_FRAMEBUFFER,1,&color);
}
static void clear_target(GLuint f)
{
    glBindFramebuffer(GL_FRAMEBUFFER,f);glDisable(GL_SCISSOR_TEST);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
}

/* The resource overlay may grow below128; reserve its maximum size once. */
#define PART_A_ROWS (SH_UI_TOP_ROWS ? g.fh + 4 : 4)
static struct {
    int fw,fh,top_rows;                     /* frame size and resource/HUD boundary */
    GLuint src565;                          /* uploaded RGB565: world raster or whole frame */
    GLuint world,world_fbo;int world_w,world_h;GLenum world_format;
    GLuint ui565[SH_UI_LAYERS],uicov[SH_UI_LAYERS],pm[SH_UI_LAYERS+1],pm_fbo[SH_UI_LAYERS+1];
    GLuint out,out_fbo;int ow,oh;
    GLuint part[SH_UI_PARTS],part_fbo[SH_UI_PARTS];int part_w[SH_UI_PARTS],part_h[SH_UI_PARTS]; /* EASU of FSR UI parts */
    float part_key[SH_UI_PARTS][9];                    /* what each part texture holds */
    GLuint rgba;int rgba_w,rgba_h;          /* 24/32-bit frames */
    int refresh;                            /* re-upload whole layers */
    int world_stale;                        /* a whole frame replaced the world */
} g;

static void delete_texture(GLuint *t,GLuint *f)
{
    if(f&&*f){glDeleteFramebuffers(1,f);*f=0;}
    if(*t){glDeleteTextures(1,t);*t=0;}
}
/* World rasters and whole frames reach the GPU through CPU-written
 * AHardwareBuffers sampled as EGLImages: glTexSubImage2D would convert the
 * 1.5 MB to the GPU's tiled layout on this (the game's) thread every frame.
 * A ring of three with fences keeps a buffer's GPU reads finished before it
 * is written again; without the extensions glTexSubImage2D is used. */
typedef struct { AHardwareBuffer *buffer; EGLImageKHR image; GLuint texture; GLsync fence; uint32_t stride; } staging;
static staging stages[3];
static int stage_w,stage_h,stage_next,stage_failed;
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC get_client_buffer;
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture;
static void stage_free(void)
{
    for(int i=0;i<3;i++){
        staging *s=&stages[i];
        if(s->fence)glDeleteSync(s->fence);
        if(s->texture)glDeleteTextures(1,&s->texture);
        if(s->image)destroy_image(display,s->image);
        if(s->buffer)AHardwareBuffer_release(s->buffer);
        memset(s,0,sizeof *s);
    }
    stage_w=stage_h=0;
}
static int stage_init(int w,int h)
{
    if(stage_failed)return 0;
    if(stage_w==w&&stage_h==h)return 1;
    if(!image_texture){
        const char *egl=eglQueryString(display,EGL_EXTENSIONS),*gl=(const char *)glGetString(GL_EXTENSIONS);
        get_client_buffer=(PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
        create_image=(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        destroy_image=(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        image_texture=(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if(!egl||!gl||!strstr(egl,"EGL_ANDROID_get_native_client_buffer")||!strstr(egl,"EGL_ANDROID_image_native_buffer")||
           !strstr(gl,"GL_OES_EGL_image")||!get_client_buffer||!create_image||!destroy_image||!image_texture){
            image_texture=NULL;stage_failed=1;return 0;
        }
    }
    stage_free();
    for(int i=0;i<3;i++){
        staging *s=&stages[i];
        AHardwareBuffer_Desc desc={.width=(uint32_t)w,.height=(uint32_t)h,.layers=1,.format=AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM,
            .usage=AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN|AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE};
        if(AHardwareBuffer_allocate(&desc,&s->buffer)){s->buffer=NULL;break;}
        AHardwareBuffer_describe(s->buffer,&desc);s->stride=desc.stride;
        static const EGLint attrs[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
        s->image=create_image(display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,get_client_buffer(s->buffer),attrs);
        if(s->image==EGL_NO_IMAGE_KHR){s->image=NULL;break;}
        glGenTextures(1,&s->texture);glBindTexture(GL_TEXTURE_2D,s->texture);
        image_texture(GL_TEXTURE_2D,(GLeglImageOES)s->image);
        if(glGetError()!=GL_NO_ERROR)break;
        if(i==2){stage_w=w;stage_h=h;return 1;}
    }
    void_log("[ddraw] AHardwareBuffer uploads unavailable; using glTexSubImage2D");
    stage_free();stage_failed=1;return 0;
}
/* The rows go into the next staging buffer; its texture, or 0 to upload otherwise. */
static GLuint stage_upload(const uint16_t *pixels,int pitch,int w,int h)
{
    if(!stage_init(g.fw,g.fh))return 0;
    staging *s=&stages[stage_next];
    if(s->fence){glClientWaitSync(s->fence,GL_SYNC_FLUSH_COMMANDS_BIT,100000000);glDeleteSync(s->fence);s->fence=0;}
    void *out=NULL;
    if(AHardwareBuffer_lock(s->buffer,AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,-1,NULL,&out)||!out){
        void_log("[ddraw] AHardwareBuffer lock failed; using glTexSubImage2D");stage_free();stage_failed=1;return 0;
    }
    for(int y=0;y<h;y++)memcpy((uint16_t *)out+(size_t)y*s->stride,pixels+(size_t)y*pitch,(size_t)w*2);
    AHardwareBuffer_unlock(s->buffer,NULL);
    return s->texture;
}
static void stage_used(void)
{
    staging *s=&stages[stage_next];
    s->fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);stage_next=(stage_next+1)%3;
}
static int frame_textures(int fw,int fh,int deband)
{
    if(g.fw!=fw||g.fh!=fh){
        delete_texture(&g.src565,NULL);delete_texture(&g.world,&g.world_fbo);
        for(int i=0;i<=SH_UI_LAYERS;i++){if(i<SH_UI_LAYERS){delete_texture(&g.ui565[i],NULL);delete_texture(&g.uicov[i],NULL);}delete_texture(&g.pm[i],&g.pm_fbo[i]);}
        g.src565=texture2d(GL_RGB565,fw,fh);
        if(stage_w)stage_free();
        g.fw=fw;g.fh=fh;g.world_format=0;g.refresh=1;
        memset(g.part_key,0,sizeof g.part_key);
    }
    GLenum format=deband&&half_float_target?GL_RGBA16F:GL_RGBA8;
    if(format!=g.world_format){
        delete_texture(&g.world,&g.world_fbo);
        /* Four texels more than the raster: its edge repeats for every filter. */
        g.world_w=fw+4;g.world_h=fh+4;g.world=texture2d(format,g.world_w,g.world_h);g.world_fbo=target_for(g.world);
        if(!g.world_fbo&&format==GL_RGBA16F){half_float_target=0;return frame_textures(fw,fh,0);}
        if(!g.world_fbo)return 0;
        g.world_format=format;
    }
    return 1;
}
static int window_textures(int ww,int wh)
{
    if(g.ow!=ww||g.oh!=wh){
        delete_texture(&g.out,&g.out_fbo);
        g.out=texture2d(GL_RGBA8,ww,wh);g.out_fbo=target_for(g.out);g.ow=ww;g.oh=wh;
        if(!g.out_fbo){g.ow=0;return 0;}
    }
    return 1;
}
static void upload(GLuint t,const void *base,int pitch,int bytes,GLenum format,GLenum type,int x,int y,int w,int h)
{
    if(w<=0||h<=0)return;
    glBindTexture(GL_TEXTURE_2D,t);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,pitch);glPixelStorei(GL_UNPACK_ALIGNMENT,bytes==2?2:1);
    glTexSubImage2D(GL_TEXTURE_2D,0,x,y,w,h,format,type,(const uint8_t *)base+((size_t)y*pitch+x)*bytes);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_ALIGNMENT,4);
}

/* RGB565 pixels (w x h, row pitch in pixels) to g.world, through `source`. */
static void upload_source(const uint16_t *pixels,int pitch,int w,int h,int deband);

/* RGB565 in `src` to g.world: [0,w)x[0,h) plus the repeated border. */
static void source_pass(GLuint src,int w,int h,int deband)
{
    int id=P_SOURCE(deband);
    if(!gl3_program(id))id=P_SOURCE(0);
    if(!gl3_program(id))return;
    begin_target(g.world_fbo);glViewport(0,0,g.world_w,g.world_h);glDisable(GL_BLEND);
    int bw=w+4<g.world_w?w+4:g.world_w,bh=h+4<g.world_h?h+4:g.world_h;
    scissor(0,0,(float)bw,(float)bh,g.world_h,0);
    const locations *u=use(id);bind(0,src,sampler_nearest);
    glUniform4i(u->iclip,0,0,w,h);
    /* Smoothed values need finer steps than RGBA8 keeps without dithering. */
    glUniform1f(u->dither,deband&&g.world_format!=GL_RGBA16F?1.f/255:0);
    quad(u,0,0,(float)bw,(float)bh,g.world_w,g.world_h,0);
}
static void upload_source(const uint16_t *pixels,int pitch,int w,int h,int deband)
{
    GLuint src=stage_upload(pixels,pitch,w,h);
    if(src){source_pass(src,w,h,deband);stage_used();return;}
    upload(g.src565,pixels,pitch,2,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,0,0,w,h);
    source_pass(g.src565,w,h,deband);
}
/* Premultiplies rows [r.top,r.bottom) of layer `layer` into texture `t`,
 * reading layer rows clamped to [y0,y1). */
static void premul_pass(int t,int layer,stronghold_rect r,int y0,int y1,uint16_t key)
{
    int th=t==SH_UI_LAYERS?PART_A_ROWS:g.fh;
    if(r.left<0)r.left=0;if(r.top<0)r.top=0;if(r.right>g.fw)r.right=g.fw;if(r.bottom>th)r.bottom=th;
    if(r.left>=r.right||r.top>=r.bottom||!gl3_program(P_PREMUL))return;
    glBindFramebuffer(GL_FRAMEBUFFER,g.pm_fbo[t]);glViewport(0,0,g.fw,th);glDisable(GL_BLEND);
    scissor((float)r.left,(float)r.top,(float)r.right,(float)r.bottom,th,0);
    const locations *u=use(P_PREMUL);bind(0,g.ui565[layer],sampler_nearest);bind(1,g.uicov[layer],sampler_nearest);
    glUniform2i(u->rows,y0,y1);
    glUniform3f(u->key,(float)(key>>11)/31.f,(float)((key>>5)&63)/63.f,(float)(key&31)/31.f);
    quad(u,(float)r.left,(float)r.top,(float)r.right,(float)r.bottom,g.fw,th,0);
}
/* Rare overlay classes pay for textures only while they are visible. A
 * reappearing class is uploaded in full, so no stale edge coverage survives. */
static int layer_textures(int i)
{
    g.ui565[i]=texture2d(GL_RGB565,g.fw,g.fh);g.uicov[i]=texture2d(GL_R8,g.fw,g.fh);
    g.pm[i]=texture2d(GL_RGBA8,g.fw,g.fh);g.pm_fbo[i]=target_for(g.pm[i]);
    if(!g.ui565[i]||!g.uicov[i]||!g.pm_fbo[i])goto failed;
    clear_target(g.pm_fbo[i]);
#if SH_UI_TOP_ROWS
    if(!i&&!g.pm[SH_UI_LAYERS]){
        g.pm[SH_UI_LAYERS]=texture2d(GL_RGBA8,g.fw,PART_A_ROWS);
        g.pm_fbo[SH_UI_LAYERS]=target_for(g.pm[SH_UI_LAYERS]);
        if(!g.pm_fbo[SH_UI_LAYERS])goto failed;
        clear_target(g.pm_fbo[SH_UI_LAYERS]);
    }
#endif
    return 1;
failed:
    delete_texture(&g.ui565[i],NULL);delete_texture(&g.uicov[i],NULL);delete_texture(&g.pm[i],&g.pm_fbo[i]);
    if(!i)delete_texture(&g.pm[SH_UI_LAYERS],&g.pm_fbo[SH_UI_LAYERS]);
    return 0;
}
static int update_layer(const stronghold_frame *f,int i)
{
    if(!f->layer[i].used){
        delete_texture(&g.ui565[i],NULL);delete_texture(&g.uicov[i],NULL);delete_texture(&g.pm[i],&g.pm_fbo[i]);
        return 1;
    }
    if(!f->layer[i].pixels||!f->layer[i].coverage)return 0;
    int fresh=!g.ui565[i];
    if(fresh){if(!layer_textures(i))return 0;memset(g.part_key,0,sizeof g.part_key);}
    int top_rows=SH_UI_TOP_ROWS;
    if(f->parts&&f->part[0].layer==0&&f->part[0].anchor_x==0&&f->part[0].anchor_y==0)top_rows=f->part[0].bottom;
    if(!i&&g.top_rows!=top_rows){g.top_rows=top_rows;g.refresh=1;memset(g.part_key,0,sizeof g.part_key);}
    stronghold_rect d=f->layer[i].dirty;
    if(g.refresh||fresh)d=(stronghold_rect){0,0,f->width,f->height};
    if(d.left>=d.right||d.top>=d.bottom)return 1;
    upload(g.ui565[i],f->layer[i].pixels,f->width,2,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,d.left,d.top,d.right-d.left,d.bottom-d.top);
    upload(g.uicov[i],f->layer[i].coverage,f->width,1,GL_RED,GL_UNSIGNED_BYTE,d.left,d.top,d.right-d.left,d.bottom-d.top);
    if(i){premul_pass(i,i,d,0,f->height,f->key);return 1;}
    /* Group 0: the bottom HUD and the complete resource overlay, each
     * repeating its edge row across the boundary between them. */
    if(d.bottom>top_rows){
        stronghold_rect b=d;b.top=d.top<=top_rows?(top_rows>4?top_rows-4:0):d.top;
        premul_pass(0,0,b,top_rows,f->height,f->key);
    }
    if(d.top<top_rows){
        stronghold_rect a=d;if(a.bottom>=top_rows)a.bottom=top_rows+4;
        premul_pass(SH_UI_LAYERS,0,a,0,top_rows,f->key);
    }
    return 1;
}

typedef struct { double fit,ox,oy; int ww,wh; } view;
static void overlay(int group,int ww,int wh)
{
    /* Contract: full-window viewport; restore program, textures, blending,
     * buffers and attributes afterwards (every draw here sets its own). */
    glBindFramebuffer(GL_FRAMEBUFFER,0);glDisable(GL_SCISSOR_TEST);glViewport(0,0,ww,wh);
    glBindSampler(0,0);glBindSampler(1,0);glActiveTexture(GL_TEXTURE0);
    stronghold_text_overlay_draw(group,ww,wh);
    glActiveTexture(GL_TEXTURE0);glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,ww,wh);
}
static void blend_premultiplied(int on)
{
    if(!on){glDisable(GL_BLEND);return;}
    glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
}
static float rcas_con(float sharpness){return fsr1_rcas_con(2.f*(1.f-sharpness));}

/* One upsampling of a source texture into a window rectangle. FSR with
 * sharpening runs EASU into an intermediate first (g.out for the world, a
 * texture of the part's own size for a UI part, origin at window pixel
 * ox,oy), then RCAS into the window; everything else draws the window
 * directly. All intermediates are written before the window pass starts. */
typedef struct {
    int filter,alpha,rcas;
    GLuint t;int tw,th;
    float map[4],clip[4],scale[2],x0,y0,x1,y1,sharpness,dither;
    GLuint mid,mid_fbo;int ox,oy,mw,mh;
} upsample;
static void upsample_init(upsample *s,int filter,int alpha,float sharpness)
{
    memset(s,0,sizeof *s);
    int fsr=filter==SH_FILTER_FSR&&gl3_program(P_FILTER(SH_FILTER_FSR,alpha,0));
    s->filter=filter==SH_FILTER_FSR&&!fsr?SH_FILTER_BICUBIC:filter;
    s->alpha=alpha;s->sharpness=sharpness;
    s->rcas=fsr&&sharpness>0&&gl3_program(P_RCAS(alpha));
}
static const locations *filter_setup(const upsample *s,int loupe)
{
    int id=filter_program(s->filter,s->alpha,loupe);
    if(id<0)return NULL;
    const locations *u=use(id);
    bind(0,s->t,s->filter==SH_FILTER_FSR?sampler_nearest:sampler_linear);
    glUniform4f(u->map,s->map[0],s->map[1],s->map[2],s->map[3]);
    glUniform4f(u->size,(float)s->tw,(float)s->th,1.f/s->tw,1.f/s->th);
    glUniform4f(u->clip,s->clip[0],s->clip[1],s->clip[2],s->clip[3]);
    glUniform2f(u->outscale,s->scale[0],s->scale[1]);
    glUniform1f(u->sharp,s->sharpness);glUniform1f(u->dither,s->dither);
    return u;
}
/* The intermediate of UI part p: grows to the part's rectangle plus margins. */
static int part_target(upsample *s,int p,const view *v)
{
    int ox=(int)floorf(s->x0-2),oy=(int)floorf(s->y0-2);
    int w=(int)ceilf(s->x1+2)-ox,h=(int)ceilf(s->y1+2)-oy;
    if(w<1)w=1;if(h<1)h=1;if(w>v->ww+4)w=v->ww+4;if(h>v->wh+4)h=v->wh+4;
    if(w>g.part_w[p]||h>g.part_h[p]){
        delete_texture(&g.part[p],&g.part_fbo[p]);
        g.part_w[p]=w>g.part_w[p]?(w+63)&~63:g.part_w[p];g.part_h[p]=h>g.part_h[p]?(h+31)&~31:g.part_h[p];
        g.part[p]=texture2d(GL_RGBA8,g.part_w[p],g.part_h[p]);g.part_fbo[p]=target_for(g.part[p]);
        g.part_key[p][0]=0;
        if(!g.part_fbo[p]){delete_texture(&g.part[p],NULL);g.part_w[p]=g.part_h[p]=0;return 0;}
    }
    s->mid=g.part[p];s->mid_fbo=g.part_fbo[p];s->ox=ox;s->oy=oy;s->mw=g.part_w[p];s->mh=g.part_h[p];
    return 1;
}
/* EASU into the intermediate (a two-pixel margin, RCAS reads one around);
 * `only`, when given, limits it to a window rectangle (the rest is kept). */
static void upsample_first(const upsample *s,const float *only)
{
    if(!s->rcas)return;
    float x0=s->x0-2-s->ox,y0=s->y0-2-s->oy,x1=s->x1+2-s->ox,y1=s->y1+2-s->oy;
    if(only){
        float a=fmaxf(x0,only[0]-s->ox),b=fmaxf(y0,only[1]-s->oy),c=fminf(x1,only[2]-s->ox),d=fminf(y1,only[3]-s->oy);
        if(a>=c||b>=d)return;
        glBindFramebuffer(GL_FRAMEBUFFER,s->mid_fbo);x0=a;y0=b;x1=c;y1=d;
    }else begin_target(s->mid_fbo);
    glViewport(0,0,s->mw,s->mh);blend_premultiplied(0);
    scissor(x0,y0,x1,y1,s->mh,1);
    upsample local=*s; /* window = local + origin */
    local.map[2]+=s->ox*s->map[0];local.map[3]+=s->oy*s->map[1];
    const locations *u=filter_setup(&local,0);
    if(!u)return;
    glUniform1f(u->dither,0);
    quad(u,x0,y0,x1,y1,s->mw,s->mh,1);
}
/* Into the window: RCAS of what upsample_first left, or the filter itself. */
static void upsample_window(const upsample *s,const view *v)
{
    glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,v->ww,v->wh);blend_premultiplied(s->alpha);
    scissor(s->x0,s->y0,s->x1,s->y1,v->wh,1);
    const locations *u;
    if(s->rcas){
        u=use(P_RCAS(s->alpha));bind(0,s->mid,sampler_nearest);
        glUniform4f(u->size,(float)s->mw,(float)s->mh,1.f/s->mw,1.f/s->mh);
        glUniform2f(u->offset,(float)s->ox,(float)(v->wh-s->oy-s->mh));
        glUniform1f(u->sharp,rcas_con(s->sharpness));glUniform1f(u->dither,s->dither);
    }else if(!(u=filter_setup(s,0)))return;
    quad(u,s->x0,s->y0,s->x1,s->y1,v->ww,v->wh,1);
}

static void draw_glow(const stronghold_frame *f,const view *v,const float map[4])
{
    stronghold_rect h=f->highlight;
    if(h.left>=h.right||h.top>=h.bottom||!gl3_program(P_GLOW))return;
    double ax=f->width*.5,ay=f->height,s=f->ui_scale;
    double left=floor(ax+(h.left-ax)*s),right=ceil(ax+(h.right-ax)*s);
    double top=floor(ay+(h.top-ay)*s),bottom=ceil(ay+(h.bottom-ay)*s);
    if(left<0)left=0;if(top<0)top=0;if(right>f->width)right=f->width;if(bottom>f->height)bottom=f->height;
    if(left>=right||top>=bottom)return;
    double pulse=.8+.2*sin((double)(void_monotonic_ns()%1400000000ull)/1400000000.0*6.283185307179586);
    float x0=(float)(v->ox+left*v->fit),x1=(float)(v->ox+right*v->fit),y0=(float)(v->oy+top*v->fit),y1=(float)(v->oy+bottom*v->fit);
    glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,v->ww,v->wh);scissor(x0,y0,x1,y1,v->wh,1);
    glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR,GL_ONE,GL_ZERO,GL_ONE);
    const locations *u=use(P_GLOW);bind(0,g.pm[0],sampler_linear);
    glUniform4f(u->map,map[0],map[1],map[2],map[3]);
    glUniform4f(u->size,(float)g.fw,(float)g.fh,1.f/g.fw,1.f/g.fh);
    glUniform4f(u->fmap,(float)(1/v->fit),(float)(1/v->fit),(float)(-v->ox/v->fit),(float)(-v->oy/v->fit));
    glUniform4f(u->glow,(float)((left+right)*.5),(float)((top+bottom)*.5),(float)(1/fmax(1,(right-left)*.5)),(float)(1/fmax(1,(bottom-top)*.5)));
    glUniform1f(u->pulse,(float)pulse);
    quad(u,x0,y0,x1,y1,v->ww,v->wh,1);
}

/* The world under the finger at the chosen on-screen scale (with the
 * building ghost, which is part of the world raster), in a gold-rimmed
 * circle placed by stronghold_loupe_place(). */
static void draw_loupe(const upsample *world,const view *v)
{
    float nx,ny;
    if(!stronghold_display_loupe(&nx,&ny))return;
    float fx=nx*v->ww,fy=ny*v->wh,cx,cy,r;
    float size=atomic_load(&loupe_size)/100.f,zoom=atomic_load(&loupe_zoom)/100.f;
    stronghold_loupe_place(fx,fy,v->ww,v->wh,size,&cx,&cy,&r);
    glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,v->ww,v->wh);blend_premultiplied(1);
    scissor(cx-r-2,cy-r-2,cx+r+2,cy+r+2,v->wh,1);
    const locations *u=filter_setup(world,1);
    if(!u)return;
    glUniform2f(u->outscale,world->scale[0]*zoom,world->scale[1]*zoom);glUniform1f(u->dither,0);
    glUniform4f(u->loupe,cx,cy,r,fmaxf(3,r*.065f));
    glUniform4f(u->loupe_src,fx*world->map[0]+world->map[2],fy*world->map[1]+world->map[3],world->map[0]/zoom,world->map[1]/zoom);
    quad(u,cx-r-2,cy-r-2,cx+r+2,cy+r+2,v->ww,v->wh,1);
}

static view fit_view(int w,int h,int ww,int wh)
{
    /* The same fit as display.c's touch inverse and layer_view. */
    view v;v.fit=fmin((double)ww/w,(double)wh/h);v.ox=(ww-w*v.fit)*.5;v.oy=(wh-h*v.fit)*.5;v.ww=ww;v.wh=wh;
    return v;
}
static void clear_window(int ww,int wh)
{
    glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,ww,wh);glDisable(GL_SCISSOR_TEST);
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
}

static void present_layered(const stronghold_frame *f,int ww,int wh)
{
    int world,ui_mode,deband,crisp;float sharpness;
    stronghold_display_filters(&world,&ui_mode,&sharpness,&deband,&crisp);
    if(!frame_textures(f->width,f->height,deband)||!window_textures(ww,wh))return;
    if(f->fresh||g.refresh||g.world_stale){
        upload_source(f->world,f->width,f->raster_width,f->raster_height,deband);g.world_stale=0;
    }
    int refresh=g.refresh;
    for(int i=0;i<SH_UI_LAYERS;i++)if(!update_layer(f,i))return;
    g.refresh=0;
    view v=fit_view(f->width,f->height,ww,wh);
    double kx=v.fit*f->world_scale_x,ky=v.fit*f->world_scale_y;
    upsample w;upsample_init(&w,world,0,sharpness);
    w.t=g.world;w.tw=g.world_w;w.th=g.world_h;
    w.map[0]=(float)(1/kx);w.map[1]=(float)(1/ky);w.map[2]=(float)(-v.ox/kx);w.map[3]=(float)(-v.oy/ky);
    w.clip[2]=(float)f->raster_width;w.clip[3]=(float)f->raster_height;w.scale[0]=(float)kx;w.scale[1]=(float)ky;
    w.x0=(float)v.ox;w.y0=(float)v.oy;w.x1=(float)(v.ox+f->width*v.fit);w.y1=(float)(v.oy+f->height*v.fit);
    /* The opaque bottom HUD hides the world below its top edge; three of
     * its texels stay drawn for the filters' fringe. */
    if(f->hud_opaque&&f->ui_scale>=1)w.y1=(float)fmin(w.y1,v.oy+(f->height-128*f->ui_scale+3*f->ui_scale)*v.fit);
    w.dither=deband?1.f/255:0;
    w.mid=g.out;w.mid_fbo=g.out_fbo;w.mw=ww;w.mh=wh;
    upsample parts[SH_UI_PARTS];int drawn[SH_UI_PARTS]={0};
    float glow_map[4]={0};
    for(int p=0;p<f->parts;p++){
        const stronghold_part *part=&f->part[p];
        int layer=part->layer;
        stronghold_rect b=f->layer[layer].bounds;
        int top=b.top>part->top?b.top:part->top,bottom=b.bottom<part->bottom?b.bottom:part->bottom;
        if(top>=bottom)continue;
        /* Two texels around the content for the filters' fringe. */
        int left=b.left-2>0?b.left-2:0,right=b.right+2<f->width?b.right+2:f->width;
        top=top-2>part->top?top-2:part->top;bottom=bottom+2<part->bottom?bottom+2:part->bottom;
        double u=part->scale>0?part->scale:f->ui_scale,ax=part->anchor_x,ay=part->anchor_y,s=1/(v.fit*u);
        int a_part=!layer&&part->top==0&&part->anchor_x==0&&part->anchor_y==0;
        upsample *l=&parts[p];upsample_init(l,ui_mode,1,sharpness);
        l->t=g.pm[a_part?SH_UI_LAYERS:layer];l->tw=f->width;l->th=a_part?PART_A_ROWS:f->height;
        l->map[0]=l->map[1]=(float)s;l->map[2]=(float)(ax*(1-1/u)-v.ox*s);l->map[3]=(float)(ay*(1-1/u)-v.oy*s);
        l->clip[1]=(float)part->top;l->clip[2]=(float)f->width;l->clip[3]=(float)part->bottom;
        l->scale[0]=l->scale[1]=(float)(v.fit*u);
        l->x0=(float)(v.ox+(ax+(left-ax)*u)*v.fit);l->y0=(float)(v.oy+(ay+(top-ay)*u)*v.fit);
        l->x1=(float)(v.ox+(ax+(right-ax)*u)*v.fit);l->y1=(float)(v.oy+(ay+(bottom-ay)*u)*v.fit);
        /* Keep the enlarged decorative HUD inside Stronghold's fitted frame. */
#ifdef STRONGHOLD_ORIGINAL
        l->x0=fmaxf(l->x0,(float)v.ox);l->y0=fmaxf(l->y0,(float)v.oy);
        l->x1=fminf(l->x1,(float)(v.ox+f->width*v.fit));l->y1=fminf(l->y1,(float)(v.oy+f->height*v.fit));
#else
        /* Scaled HUD bands can be wider than the window: draw what shows. */
        l->x0=fmaxf(l->x0,0);l->y0=fmaxf(l->y0,0);l->x1=fminf(l->x1,(float)ww);l->y1=fminf(l->y1,(float)wh);
#endif
        if(l->x0>=l->x1||l->y0>=l->y1)continue;
        if(!layer&&!a_part)memcpy(glow_map,l->map,sizeof glow_map);
        if(l->rcas&&!part_target(l,p,&v))l->rcas=0;
        drawn[p]=1;
    }
    /* Offscreen first, so the window pass is never interrupted. A UI part's
     * EASU is kept while its mapping holds: only what its layer's changed
     * texels reach (three texels of filter footprint) is computed again. */
    upsample_first(&w,NULL);
    for(int p=0;p<SH_UI_PARTS;p++)if(p>=f->parts||!drawn[p]||!parts[p].rcas)g.part_key[p][0]=0;
    for(int p=0;p<f->parts;p++)if(drawn[p]&&parts[p].rcas){
        const upsample *l=&parts[p];
        float key[9]={1,l->map[0],l->map[2],l->map[3],l->x0,l->y0,l->x1,l->y1,(float)(f->part[p].layer*8+l->ox)};
        if(memcmp(key,g.part_key[p],sizeof key)||refresh){upsample_first(l,NULL);memcpy(g.part_key[p],key,sizeof key);continue;}
        stronghold_rect d=f->layer[f->part[p].layer].dirty;
        if(d.left>=d.right||d.top>=d.bottom)continue;
        double u=f->part[p].scale>0?f->part[p].scale:f->ui_scale,ax=f->part[p].anchor_x,ay=f->part[p].anchor_y;
        float only[4]={(float)(v.ox+(ax+(d.left-3-ax)*u)*v.fit),(float)(v.oy+(ay+(d.top-3-ay)*u)*v.fit),
                       (float)(v.ox+(ax+(d.right+3-ax)*u)*v.fit),(float)(v.oy+(ay+(d.bottom+3-ay)*u)*v.fit)};
        upsample_first(l,only);
    }
    clear_window(ww,wh);
    upsample_window(&w,&v);
    int order=0;
    for(int p=0;p<=f->parts;p++){
        int layer=p<f->parts?f->part[p].layer:-1;
        while(order<SH_UI_LAYERS&&(p==f->parts||SH_UI_ORDER[order]!=layer)){
            int group=SH_UI_ORDER[order++];if(!group)draw_glow(f,&v,glow_map);overlay(group,ww,wh);
        }
        if(p<f->parts&&drawn[p])upsample_window(&parts[p],&v);
    }
    draw_loupe(&w,&v);
    glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);
}

/* Menus, loading screens and videos: the whole frame with the UI filter. */
static void present_frame(const uint8_t *px,int w,int h,int pitch,int bpp,int ww,int wh)
{
    int world,ui_mode,deband,crisp;float sharpness;
    stronghold_display_filters(&world,&ui_mode,&sharpness,&deband,&crisp);
    upsample s;upsample_init(&s,ui_mode,0,sharpness);
    if(bpp==16){
        if(!frame_textures(w,h,deband)||!window_textures(ww,wh))return;
        upload_source((const uint16_t *)px,pitch/2,w,h,0);s.t=g.world;s.tw=g.world_w;s.th=g.world_h;
        g.world_stale=1; /* the next in-game frame uploads its world again */
    }else{
        GLenum format,type;const uint8_t *data=frame_upload(px,w,h,pitch,bpp,&format,&type);
        if(!data||!window_textures(ww,wh))return;
        if(g.rgba_w!=w||g.rgba_h!=h){delete_texture(&g.rgba,NULL);g.rgba=texture2d(GL_RGBA8,w,h);g.rgba_w=w;g.rgba_h=h;}
        upload(g.rgba,data,w,4,GL_RGBA,GL_UNSIGNED_BYTE,0,0,w,h);
        s.t=g.rgba;s.tw=w;s.th=h;
    }
    view v=fit_view(w,h,ww,wh);
    s.map[0]=s.map[1]=(float)(1/v.fit);s.map[2]=(float)(-v.ox/v.fit);s.map[3]=(float)(-v.oy/v.fit);
    s.clip[2]=(float)w;s.clip[3]=(float)h;s.scale[0]=s.scale[1]=(float)v.fit;
    s.x0=(float)v.ox;s.y0=(float)v.oy;s.x1=(float)(v.ox+w*v.fit);s.y1=(float)(v.oy+h*v.fit);
    s.mid=g.out;s.mid_fbo=g.out_fbo;s.mw=ww;s.mh=wh;
    upsample_first(&s,NULL);
    clear_window(ww,wh);
    upsample_window(&s,&v);
    glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);
    overlay(-1,ww,wh);
}

static int initialize_gles3(void)
{
    static const GLfloat unit[]={0,0,1,0,0,1,1,1};
    glGenBuffers(1,&quad_buffer);glBindBuffer(GL_ARRAY_BUFFER,quad_buffer);glBufferData(GL_ARRAY_BUFFER,sizeof unit,unit,GL_STATIC_DRAW);
    glGenSamplers(1,&sampler_nearest);glGenSamplers(1,&sampler_linear);
    for(int i=0;i<2;i++){
        GLuint s=i?sampler_linear:sampler_nearest;
        glSamplerParameteri(s,GL_TEXTURE_MIN_FILTER,i?GL_LINEAR:GL_NEAREST);glSamplerParameteri(s,GL_TEXTURE_MAG_FILTER,i?GL_LINEAR:GL_NEAREST);
        glSamplerParameteri(s,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glSamplerParameteri(s,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    /* The always-needed programs; the other variants compile on first use. */
    if(!gl3_program(P_SOURCE(0))||!gl3_program(P_PREMUL)||filter_program(SH_FILTER_BILINEAR,0,0)<0||
       filter_program(SH_FILTER_BILINEAR,1,0)<0)return 0;
    const char *version=(const char *)glGetString(GL_VERSION);
    half_float_target=version&&(strstr(version,"OpenGL ES 3.2")!=NULL||
        (glGetString(GL_EXTENSIONS)&&strstr((const char *)glGetString(GL_EXTENSIONS),"GL_EXT_color_buffer_half_float")));
    glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);
    return 1;
}

static int follow_window(int initial)
{
    uint32_t serial;void *window=void_android_window_current(&serial,initial);
    if(!initial&&serial==window_serial)return 1;
    if(!eglMakeCurrent(display,pbuffer,pbuffer,context))return 0;
    if(window_surface!=EGL_NO_SURFACE)eglDestroySurface(display,window_surface);
    window_surface=EGL_NO_SURFACE;window_serial=serial;void_android_window_ack(serial);
#ifdef VOID_ANDROID_APP
    if(!window){void_blocking_begin();window=void_android_window_current(&serial,1);void_blocking_end();window_serial=serial;}
#endif
    if(window){
        window_surface=eglCreateWindowSurface(display,config,(EGLNativeWindowType)window,NULL);
        if(window_surface==EGL_NO_SURFACE||!eglMakeCurrent(display,window_surface,window_surface,context)){
            void_log("[ddraw] window EGL surface failed %04X",eglGetError());
            if(window_surface!=EGL_NO_SURFACE)eglDestroySurface(display,window_surface);
            window_surface=EGL_NO_SURFACE;eglMakeCurrent(display,pbuffer,pbuffer,context);
        } else {eglSwapInterval(display,1);void_log("[ddraw] window surface ready");}
    }
    void_android_window_ack(serial);return 1;
}
#ifndef SH_PBUFFER_WIDTH /* the device harness renders without a window */
#define SH_PBUFFER_WIDTH 16
#define SH_PBUFFER_HEIGHT 16
#endif
static int initialize(void)
{
    if(failed)return 0;
    if(context!=EGL_NO_CONTEXT)return 1;
    failed=1;display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if(!eglInitialize(display,NULL,NULL)){void_log("[ddraw] eglInitialize %04X",eglGetError());return 0;}
    const EGLint pbuffer_attrs[]={EGL_WIDTH,SH_PBUFFER_WIDTH,EGL_HEIGHT,SH_PBUFFER_HEIGHT,EGL_NONE};
    /* OpenGL ES 3 for the layered pipeline; ES 2 keeps the single filter. */
    for(int version=3;version>=2&&context==EGL_NO_CONTEXT;version--){
        const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT|EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,version==3?0x40:EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,0,EGL_NONE};
        EGLint n=0;if(!eglChooseConfig(display,attrs,&config,1,&n)||!n)continue;
        const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,version,EGL_NONE};
        context=eglCreateContext(display,config,EGL_NO_CONTEXT,ca);
        if(context==EGL_NO_CONTEXT)continue;
        pbuffer=eglCreatePbufferSurface(display,config,pbuffer_attrs);
        if(pbuffer==EGL_NO_SURFACE||!follow_window(1)){
            if(pbuffer!=EGL_NO_SURFACE)eglDestroySurface(display,pbuffer);
            eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
            eglDestroyContext(display,context);context=EGL_NO_CONTEXT;pbuffer=EGL_NO_SURFACE;continue;
        }
        gles3=version==3&&initialize_gles3();
        if(version==3&&!gles3){
            void_log("[ddraw] OpenGL ES 3 pipeline unavailable; trying ES 2");
            eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
            if(window_surface!=EGL_NO_SURFACE)eglDestroySurface(display,window_surface);
            eglDestroySurface(display,pbuffer);eglDestroyContext(display,context);
            context=EGL_NO_CONTEXT;pbuffer=window_surface=EGL_NO_SURFACE;window_serial=0;
            memset(gl3_programs,0,sizeof gl3_programs);memset(gl3_failed,0,sizeof gl3_failed);
            continue;
        }
        if(!gles3&&!initialize_gles2()){void_log("[ddraw] no usable shaders");return 0;}
    }
    if(context==EGL_NO_CONTEXT){void_log("[ddraw] EGL context/current %04X",eglGetError());return 0;}
    const char *dump=getenv("VOID_DUMP_FRAMES");dump_every=dump?atoi(dump):0;report_ns=void_monotonic_ns();failed=0;
    void_log("[ddraw] %s / %s / %s (%s)",glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION),
             gles3?"layered GPU composition":"CPU composition");
    return 1;
}
static void dump_bmp(const uint8_t *px,int w,int h,int pitch,int bpp)
{
    char path[1200];snprintf(path,sizeof path,"%s/frames",void_data_dir());mkdir(path,0775);
    snprintf(path,sizeof path,"%s/frames/ddraw_%06llu.bmp",void_data_dir(),(unsigned long long)frame_count);
    FILE *f=fopen(path,"wb");if(!f){void_log("[ddraw] cannot write %s",path);return;}
    uint32_t stride=((uint32_t)w*3+3)&~3u,size=54+stride*(uint32_t)h;
    uint8_t head[54]={'B','M'};
#define PUT32(o,v) do {uint32_t x_=(v);memcpy(head+(o),&x_,4);}while(0)
    PUT32(2,size);PUT32(10,54);PUT32(14,40);PUT32(18,(uint32_t)w);PUT32(22,(uint32_t)h);head[26]=1;head[28]=24;PUT32(34,stride*(uint32_t)h);
#undef PUT32
    uint8_t *row=calloc(1,stride);if(!row){fclose(f);return;}int good=fwrite(head,1,54,f)==54;
    for(int y=h-1;y>=0&&good;y--){const uint8_t *src=px+(size_t)y*pitch;
        for(int x=0;x<w;x++){
            if(bpp==16){uint16_t c=((const uint16_t *)src)[x];row[x*3]=(uint8_t)((c&31)*255/31);row[x*3+1]=(uint8_t)(((c>>5)&63)*255/63);row[x*3+2]=(uint8_t)((c>>11)*255/31);}
            else if(bpp==8)row[x*3]=row[x*3+1]=row[x*3+2]=src[x];
            else {row[x*3]=src[x*(bpp/8)];row[x*3+1]=src[x*(bpp/8)+1];row[x*3+2]=src[x*(bpp/8)+2];}
        }good=fwrite(row,1,stride,f)==stride;
    }
    free(row);if(fclose(f))good=0;void_log("[ddraw] frame %llu dump %s (%s)",(unsigned long long)frame_count,path,good?"ok":"write failed");
}

/* Crisp text: once per fresh layered frame (also with the setting off, which
 * clears its queues), give the layers about to be uploaded their text-free
 * pixels and widen each layer's dirty rectangle by what was edited. */
static void hide_layer_text(stronghold_frame *frame)
{
    uint16_t *pixels[SH_UI_LAYERS];uint8_t *coverage[SH_UI_LAYERS];int bounds[SH_UI_LAYERS][4],hidden[SH_UI_LAYERS][4];
    for(int i=0;i<SH_UI_LAYERS;i++){
        pixels[i]=frame->layer[i].used?(uint16_t *)frame->layer[i].pixels:NULL;
        coverage[i]=frame->layer[i].used?(uint8_t *)frame->layer[i].coverage:NULL;
        stronghold_rect r=frame->layer[i].bounds;
        bounds[i][0]=r.left;bounds[i][1]=r.top;bounds[i][2]=r.right;bounds[i][3]=r.bottom;
    }
    if(!stronghold_text_overlay_hide_layers(pixels,coverage,bounds,frame->width,frame->width,frame->height,hidden))return;
    for(int i=0;i<SH_UI_LAYERS;i++){
        if(hidden[i][0]>=hidden[i][2]||hidden[i][1]>=hidden[i][3])continue;
        stronghold_rect *d=&frame->layer[i].dirty;
        if(d->left>=d->right||d->top>=d->bottom){d->left=hidden[i][0];d->top=hidden[i][1];d->right=hidden[i][2];d->bottom=hidden[i][3];continue;}
        if(hidden[i][0]<d->left)d->left=hidden[i][0];
        if(hidden[i][1]<d->top)d->top=hidden[i][1];
        if(hidden[i][2]>d->right)d->right=hidden[i][2];
        if(hidden[i][3]>d->bottom)d->bottom=hidden[i][3];
    }
}

void void_ddraw_present_pixels(const uint8_t *px,int w,int h,int pitch,int bpp)
{
    if(!px||w<=0||h<=0||pitch<w*(bpp/8))return;
    stronghold_frame frame;
    int layered=stronghold_display_frame(&frame,w,h,bpp);
    if(layered<0)return;
    ++frame_count;
    int dump=dump_next||(dump_every>0&&frame_count%(uint64_t)dump_every==0);
    if(!initialize()||!follow_window(0)){g.refresh=1;return;}
    /* Crisp text maps its runs through the window size: set it first. */
    EGLSurface target=window_surface!=EGL_NO_SURFACE?window_surface:pbuffer;EGLint ww=16,hh=16;
    eglQuerySurface(display,target,EGL_WIDTH,&ww);eglQuerySurface(display,target,EGL_HEIGHT,&hh);
    stronghold_display_surface_size(ww,hh);
    if(layered&&gles3&&frame.fresh)hide_layer_text(&frame);
    else if(layered&&!gles3)stronghold_text_overlay_publish();
    /* The CPU composite: the ES 2 picture and the dump oracle. */
    const uint8_t *composed=px;int composed_pitch=pitch;
    if(layered&&(dump||!gles3)){composed=stronghold_display_compose(px,w,h,&composed_pitch,bpp);if(!composed)return;}
    if(dump){dump_next=0;dump_bmp(composed,w,h,composed_pitch,bpp);}
    /* An uncomposed frame (menus) is edited in place for the upload, then restored. */
    int hidden=!layered&&stronghold_text_overlay_hide_frame(bpp==16&&!(pitch&1)?(uint16_t *)px:NULL,pitch/2,w,h);
    if(!gles3)present_gles2(composed,w,h,composed_pitch,bpp,ww,hh,layered);
    else if(layered)present_layered(&frame,ww,hh);
    else present_frame(px,w,h,pitch,bpp,ww,hh);
    if(hidden)stronghold_text_overlay_restore_frame();
    stronghold_perf_frame_end(); /* the swap below may wait for the display */
    if(!eglSwapBuffers(display,target))void_log("[ddraw] swap %04X",eglGetError());
    void_android_frame_presented();
    uint64_t now=void_monotonic_ns();if(now-report_ns>=5000000000ull){
        double elapsed=(double)(now-report_ns)*1e-9;void_log("[ddraw] frame %llu: %.2f fps (%dx%d RGB%d)",(unsigned long long)frame_count,(double)(frame_count-report_frame)/elapsed,w,h,bpp);
        report_frame=frame_count;report_ns=now;void_thunk_stats((uint32_t)frame_count);
    }
}
