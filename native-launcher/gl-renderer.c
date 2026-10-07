/* Atmosphere native OpenGL UI. GPL-3.0-or-later. */
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#ifndef ATMOSPHERE_GL_HOST
#include <ps5_opengl_display_modes.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
static void trace_stage(const char *stage) {
#ifndef ATMOSPHERE_GL_HOST
 int fd=open("/app0/atmosphere-state/opengl-startup.log",O_WRONLY|O_CREAT|O_APPEND,0600);
 if(fd>=0){write(fd,stage,strlen(stage));write(fd,"\n",1);close(fd);}
#else
 fprintf(stderr,"OpenGL: %s\n",stage);
#endif
}
#define STB_TRUETYPE_IMPLEMENTATION
#include "gl-assets/stb_truetype.h"

static EGLDisplay display=EGL_NO_DISPLAY;
static EGLSurface surface=EGL_NO_SURFACE;
static EGLContext context=EGL_NO_CONTEXT;
static GLuint program, vao, vbo, font_texture;
static GLint u_rect,u_radius,u_top,u_bottom,u_kind;
static char error_text[2048];
static stbtt_packedchar glyphs[1248];
typedef struct {float x,y,u,v;} Vertex;
#define MAX_QUADS 32768
static Vertex vertices[6*MAX_QUADS];
typedef struct {float x,y,w,h,r;uint32_t top,bottom;int kind;GLuint texture;} DrawState;
typedef struct {DrawState state;int first,count;} Draw;
static Draw draws[MAX_QUADS];
static DrawState current;
static int count,segment_start,draw_count;
static unsigned submitted_frames;
static void close_renderer(void);
static int fail(const char *what) {snprintf(error_text,sizeof(error_text),"%s (EGL 0x%x / GL 0x%x)",what,eglGetError(),glGetError());return -1;}
static const char *last_error(void){return error_text;}
static GLuint shader(GLenum kind,const char *source) {
 GLuint s=glCreateShader(kind);glShaderSource(s,1,&source,NULL);glCompileShader(s);
 GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
 if(!ok){glGetShaderInfoLog(s,sizeof(error_text),NULL,error_text);glDeleteShader(s);return 0;}return s;
}
static void color(GLint uniform,uint32_t c){glUniform4f(uniform,((c>>16)&255)/255.f,((c>>8)&255)/255.f,(c&255)/255.f,(c>>24)/255.f);}
static void flush(void){
 int n=count-segment_start;if(!n)return;
 if(draw_count && memcmp(&draws[draw_count-1].state,&current,sizeof(current))==0)
  draws[draw_count-1].count+=n;
 else draws[draw_count++]=(Draw){current,segment_start,n};
 segment_start=count;
}
static void submit(void){
 if(!count)return;
 glUseProgram(program);glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);glActiveTexture(GL_TEXTURE0);
 /* One immutable geometry upload for this batch, rather than overwriting the
  * same in-flight buffer for every UI primitive. Driver retains old storage. */
 glBufferData(GL_ARRAY_BUFFER,count*sizeof(Vertex),vertices,GL_STREAM_DRAW);
 for(int i=0;i<draw_count;i++){
  Draw *d=&draws[i];DrawState *s=&d->state;
  glUniform4f(u_rect,s->x,s->y,s->w,s->h);glUniform1f(u_radius,s->r);
  color(u_top,s->top);color(u_bottom,s->bottom);glUniform1i(u_kind,s->kind);
  glBindTexture(GL_TEXTURE_2D,s->texture?s->texture:font_texture);glDrawArrays(GL_TRIANGLES,d->first,d->count);
 }
 count=segment_start=draw_count=0;
}
static void quad(float x,float y,float w,float h,float u0,float v0,float u1,float v1){
 if(count+6>(int)(sizeof(vertices)/sizeof(vertices[0]))){flush();submit();}
 Vertex q[6]={{x,y,u0,v0},{x+w,y,u1,v0},{x,y+h,u0,v1},{x,y+h,u0,v1},{x+w,y,u1,v0},{x+w,y+h,u1,v1}};
 memcpy(vertices+count,q,sizeof(q));count+=6;
}
static GLuint texture(int w,int h,const void *data,int alpha){
 if(w<=0 || h<=0 || w>8192 || h>8192 || !data)return 0;
 GLuint t=0;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
 glPixelStorei(GL_UNPACK_ALIGNMENT,1);
 if(alpha==2 || alpha==3){
  GLint major=0,minor=0,extensions=0;glGetIntegerv(GL_MAJOR_VERSION,&major);glGetIntegerv(GL_MINOR_VERSION,&minor);
  int found=major>4||(major==4&&minor>=2);
  glGetIntegerv(GL_NUM_EXTENSIONS,&extensions);
  for(int i=0;!found&&i<extensions;i++){const char *ext=(const char*)glGetStringi(GL_EXTENSIONS,i);if(ext&&!strcmp(ext,"GL_ARB_texture_compression_bptc"))found=1;}
  GLenum format=alpha==3?GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM:GL_COMPRESSED_RGBA_BPTC_UNORM;
  if(!found){glDeleteTextures(1,&t);return 0;}
  glCompressedTexImage2D(GL_TEXTURE_2D,0,format,w,h,0,((w+3)/4)*((h+3)/4)*16,data);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
 }else{
 glTexImage2D(GL_TEXTURE_2D,0,alpha?GL_R8:GL_RGBA8,w,h,0,alpha?GL_RED:GL_BGRA,GL_UNSIGNED_BYTE,data);
 if(!alpha){glGenerateMipmap(GL_TEXTURE_2D);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);}
 }
 if(glGetError()!=GL_NO_ERROR){glDeleteTextures(1,&t);return 0;}return t;
}
static int load_font(void){
#ifdef ATMOSPHERE_GL_HOST
 FILE *f=fopen("gl-assets/Montserrat-Medium.ttf","rb");
#else
 FILE *f=fopen("/app0/ui/Montserrat-Medium.ttf","rb");
#endif
 if(!f)return fail("Open Montserrat font");
 if(fseek(f,0,SEEK_END)){fclose(f);return fail("Seek font");}long n=ftell(f);rewind(f);
 if(n<12 || n>8*1024*1024){fclose(f);return fail("Invalid font size");}
 unsigned char *ttf=malloc(n),*bitmap=calloc(4096,4096);int ok=0;
 if(ttf && bitmap && fread(ttf,1,n,f)==(size_t)n){
  stbtt_pack_context pack;
  if(stbtt_PackBegin(&pack,bitmap,4096,4096,0,2,NULL)){
   stbtt_PackSetOversampling(&pack,1,1);
   ok=stbtt_PackFontRange(&pack,ttf,0,96,32,1248,glyphs);stbtt_PackEnd(&pack);
   if(ok)font_texture=texture(4096,4096,bitmap,1);
  }
 }
 fclose(f);free(ttf);free(bitmap);return ok&&font_texture?0:fail("Build font atlas");
}
#ifdef ATMOSPHERE_GL_SMOKE
/* Isolate native submission from the UI, texture decoder and backend. */
static int smoke_test(void){
 trace_stage("SMOKE: clear begin");
 glViewport(0,0,1920,1080);glClearColor(.03f,.06f,.10f,1);glClear(GL_COLOR_BUFFER_BIT);
 glFinish();trace_stage("SMOKE: clear completed");
 if(!eglSwapBuffers(display,surface))return fail("Smoke clear presentation");
 trace_stage("SMOKE: clear presented");
 const char *vs="#version 330 core\nlayout(location=0) in vec2 pos;void main(){gl_Position=vec4(pos,0,1);}";
 const char *fs="#version 330 core\nout vec4 frag;void main(){frag=vec4(.25,.85,.70,1);}";
 GLuint v=shader(GL_VERTEX_SHADER,vs),f=shader(GL_FRAGMENT_SHADER,fs);
 if(!v||!f){if(v)glDeleteShader(v);if(f)glDeleteShader(f);return -1;}
 program=glCreateProgram();glAttachShader(program,v);glAttachShader(program,f);glLinkProgram(program);
 glDeleteShader(v);glDeleteShader(f);GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);
 if(!linked)return fail("Smoke shader link");
 const float triangle[]={-.5f,-.5f,.5f,-.5f,0,.5f};
 glUseProgram(program);glGenVertexArrays(1,&vao);glBindVertexArray(vao);
 glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
 glBufferData(GL_ARRAY_BUFFER,sizeof(triangle),triangle,GL_STATIC_DRAW);
 glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,2*sizeof(float),0);
 trace_stage("SMOKE: triangle draw begin");
 glDrawArrays(GL_TRIANGLES,0,3);trace_stage("SMOKE: triangle queued; waiting for GPU");
 glFinish();trace_stage("SMOKE: triangle completed");
 if(glGetError()!=GL_NO_ERROR)return fail("Smoke triangle GL error");
 if(!eglSwapBuffers(display,surface))return fail("Smoke triangle presentation");
 glFinish();trace_stage("SMOKE: triangle presented; test passed");
 return 0;
}
#endif
static int open_renderer(void){
 if(display!=EGL_NO_DISPLAY)return -1;
#ifndef ATMOSPHERE_GL_HOST
 /* Driver failures use stderr; preserve them before the process can exit. */
 FILE *log=freopen("/app0/atmosphere-state/opengl-driver.log","a",stderr);
 if(log)setvbuf(log,NULL,_IONBF,0);
 log=freopen("/app0/atmosphere-state/opengl-driver.log","a",stdout);
 if(log)setvbuf(log,NULL,_IONBF,0);
#endif
 trace_stage("Starting renderer");
 error_text[0]=0;EGLint major,minor,n;EGLConfig config;
#ifdef ATMOSPHERE_GL_HOST
 const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
 const EGLint ctx[]={EGL_CONTEXT_MAJOR_VERSION_KHR,3,EGL_CONTEXT_MINOR_VERSION_KHR,3,EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,EGL_NONE};
 display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
 trace_stage("EGL display acquired; initializing native display");
 if(display==EGL_NO_DISPLAY || !eglInitialize(display,&major,&minor) || !eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(display,attrs,&config,1,&n) || n!=1){fail("Initialize host EGL");goto bad;}
 const EGLint pbuffer[]={EGL_WIDTH,1920,EGL_HEIGHT,1080,EGL_NONE};
 surface=eglCreatePbufferSurface(display,config,pbuffer);context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);
#else
 const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
 const EGLint ctx[]={EGL_CONTEXT_MAJOR_VERSION_KHR,4,EGL_CONTEXT_MINOR_VERSION_KHR,6,EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,EGL_NONE};
 display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
 if(display==EGL_NO_DISPLAY || !eglSetDisplayModePS5(display,3840,2160) || !eglSetDisplayRefreshPS5(display,60) || !eglInitialize(display,&major,&minor) || !eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(display,attrs,&config,1,&n) || n!=1){fail("Initialize OpenGL");goto bad;}
 surface=eglCreateWindowSurface(display,config,0,NULL);context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);
 trace_stage("Native window and context created");
#endif
 if(surface==EGL_NO_SURFACE || context==EGL_NO_CONTEXT || !eglMakeCurrent(display,surface,surface,context) || !eglSwapInterval(display,1)){fail("Create OpenGL display");goto bad;}
#ifdef ATMOSPHERE_GL_SMOKE
 if(smoke_test()==0)snprintf(error_text,sizeof(error_text),"GPU smoke test passed; software mode restored");
 close_renderer();return -1;
#endif
 trace_stage("Context current; compiling UI shaders");
 const char *vs="#version 330 core\nlayout(location=0) in vec2 pos;layout(location=1) in vec2 uv;out vec2 p;out vec2 t;void main(){p=pos;t=uv;gl_Position=vec4(pos.x/960.0-1.0,1.0-pos.y/540.0,0,1);}";
 const char *fs="#version 330 core\nin vec2 p;in vec2 t;out vec4 frag;uniform vec4 rect,top,bottom;uniform float radius;uniform int kind;uniform sampler2D image;void main(){vec4 c=mix(top,bottom,clamp((p.y-rect.y)/max(rect.w,1.0),0.0,1.0));if(kind==1)c*=texture(image,t);if(kind==2)c.a*=texture(image,t).r;if(kind==3){vec4 b=vec4(0);vec2 stepUV=6.0/vec2(textureSize(image,0));float weights[9]=float[9](1.0,8.0,28.0,56.0,70.0,56.0,28.0,8.0,1.0);for(int j=-4;j<=4;j++)for(int i=-4;i<=4;i++){b+=textureLod(image,t+vec2(i,j)*stepUV,2.0)*weights[i+4]*weights[j+4];}c*=b/65536.0;}if(radius>0.0){vec2 q=abs(p-(rect.xy+rect.zw*.5))-rect.zw*.5+radius;float d=length(max(q,0.0))+min(max(q.x,q.y),0.0)-radius;c.a*=1.0-smoothstep(-.75,.75,d);}frag=c;}";
 GLuint v=shader(GL_VERTEX_SHADER,vs),f=shader(GL_FRAGMENT_SHADER,fs);
 if(!v||!f){if(v)glDeleteShader(v);if(f)glDeleteShader(f);goto bad;}
 program=glCreateProgram();glAttachShader(program,v);glAttachShader(program,f);glLinkProgram(program);glDeleteShader(v);glDeleteShader(f);
 GLint linked;glGetProgramiv(program,GL_LINK_STATUS,&linked);if(!linked){glGetProgramInfoLog(program,sizeof(error_text),NULL,error_text);goto bad;}
 glUseProgram(program);u_rect=glGetUniformLocation(program,"rect");u_radius=glGetUniformLocation(program,"radius");u_top=glGetUniformLocation(program,"top");u_bottom=glGetUniformLocation(program,"bottom");u_kind=glGetUniformLocation(program,"kind");glUniform1i(glGetUniformLocation(program,"image"),0);
 glGenVertexArrays(1,&vao);glBindVertexArray(vao);glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
 glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),NULL,GL_STREAM_DRAW);
 glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)0);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)8);
 #ifdef ATMOSPHERE_GL_HOST
glViewport(0,0,1920,1080);
#else
glViewport(0,0,3840,2160);
#endif
glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
 trace_stage("Loading font atlas");
 if(load_font())goto bad;
 trace_stage("Renderer ready");return 0;
bad:close_renderer();return -1;
}
static void begin(uint32_t c){flush();submit();glClearColor(((c>>16)&255)/255.f,((c>>8)&255)/255.f,(c&255)/255.f,1);glClear(GL_COLOR_BUFFER_BIT);glUseProgram(program);glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);glActiveTexture(GL_TEXTURE0);}
static int present(void){flush();if(!submitted_frames)trace_stage("Submitting first frame");submit();if(!eglSwapBuffers(display,surface))return fail("Present OpenGL frame");if(!submitted_frames)trace_stage("First frame presented");if(++submitted_frames==120)trace_stage("120 UI frames presented");return 0;}
static void rect(float x,float y,float w,float h,float r,uint32_t top,uint32_t bottom){
 if(w<=0||h<=0)return;memset(&current,0,sizeof(current));
 if(r>0 || top!=bottom){current.x=x;current.y=y;current.w=w;current.h=h;}
 current.r=r;current.top=top;current.bottom=bottom;quad(x,y,w,h,0,0,1,1);flush();
}
static unsigned codepoint(const unsigned char **s){unsigned c=*(*s)++;if(c<128)return c;int n=c>=240?3:c>=224?2:c>=192?1:0;c&=n==3?7:n==2?15:31;for(int i=0;i<n;i++){if((**s&192)!=128)return '?';c=(c<<6)|(*(*s)++&63);}return n?c:'?';}
static int glyph_index(unsigned c){return c>=32&&c<1280?(int)c-32:'?'-32;}
static float measure(const char *text,float size){float w=0;const unsigned char *s=(const unsigned char*)text;while(*s)w+=glyphs[glyph_index(codepoint(&s))].xadvance;return w*size/96.f;}
static void text(const char *text,float x,float y,float size,uint32_t tint,float width){
 if(!text||width<=0)return;float scale=size/96.f,pen=x,baseline=y+size*.82f;
 memset(&current,0,sizeof(current));current.kind=2;current.top=current.bottom=tint;current.texture=font_texture;
 const unsigned char *s=(const unsigned char*)text;
 while(*s){int i=glyph_index(codepoint(&s));stbtt_packedchar *g=&glyphs[i];float next=pen+g->xadvance*scale;
  if(next>x+width)break;
  quad(pen+g->xoff*scale,baseline+g->yoff*scale,(g->xoff2-g->xoff)*scale,(g->yoff2-g->yoff)*scale,g->x0/4096.f,g->y0/4096.f,g->x1/4096.f,g->y1/4096.f);pen=next;
 }flush();
}
static void image(GLuint id,float x,float y,float w,float h){
 memset(&current,0,sizeof(current));current.x=x;current.y=y;current.w=w;current.h=h;current.r=8;
 current.kind=1;current.top=current.bottom=0xffffffff;current.texture=id;
 quad(x,y,w,h,0,0,1,1);flush();
}
static void artwork(GLuint id,float x,float y,float w,float h,float aspect,float radius,float opacity,int frosted){
 memset(&current,0,sizeof(current));current.x=x;current.y=y;current.w=w;current.h=h;current.r=radius;
 current.kind=frosted?3:1;current.top=current.bottom=((uint32_t)(255*opacity)<<24)|0xffffff;current.texture=id;
 float u=1,v=1,target=w/h;
 if(aspect>target)u=target/aspect;else v=aspect/target;
 quad(x,y,w,h,(1-u)*.5f,(1-v)*.5f,(1+u)*.5f,(1+v)*.5f);flush();
}
static void delete_texture(GLuint id){if(id){flush();submit();glDeleteTextures(1,&id);}}
static void close_renderer(void){
 if(display==EGL_NO_DISPLAY)return;
 if(context!=EGL_NO_CONTEXT){if(font_texture)glDeleteTextures(1,&font_texture);if(vbo)glDeleteBuffers(1,&vbo);if(vao)glDeleteVertexArrays(1,&vao);if(program)glDeleteProgram(program);eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);}
 if(surface!=EGL_NO_SURFACE)eglDestroySurface(display,surface);eglTerminate(display);
 display=EGL_NO_DISPLAY;surface=EGL_NO_SURFACE;context=EGL_NO_CONTEXT;font_texture=vbo=vao=program=0;count=segment_start=draw_count=0;submitted_frames=0;
}
static void library_clip(int enabled){
 flush();submit();
 if(!enabled){glDisable(GL_SCISSOR_TEST);return;}
 GLint viewport[4];glGetIntegerv(GL_VIEWPORT,viewport);
 glEnable(GL_SCISSOR_TEST);
 glScissor(0,viewport[3]*(1080-824)/1080,viewport[2],viewport[3]*(824-240)/1080);
}
typedef struct {uint32_t version,size;void *open,*close,*begin,*present,*rect,*text,*measure,*texture,*image,*delete_texture,*error,*clip,*artwork;} Api;
int atmosphere_gl_start(size_t size,void *args){
 if(!args||size!=sizeof(Api))return -1;Api *a=args;if(a->version!=1||a->size!=sizeof(Api))return -1;
 *a=(Api){1,sizeof(Api),open_renderer,close_renderer,begin,present,rect,text,measure,texture,image,delete_texture,last_error,library_clip,artwork};return 0;
}
#ifdef ATMOSPHERE_GL_HOST
int atmosphere_gl_readback(void *pixels){flush();submit();glFinish();glReadPixels(0,0,1920,1080,GL_BGRA,GL_UNSIGNED_BYTE,pixels);return glGetError()==GL_NO_ERROR?0:-1;}
#endif
