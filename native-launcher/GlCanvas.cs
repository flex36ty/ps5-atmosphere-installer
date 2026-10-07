using System;
using System.Linq;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using SharpProspero.Graphics;
using SharpProspero.Interop.Kernel;

namespace Atmosphere;

// The PRX owns all EGL/GL objects. Calls stay on the frame thread.
internal sealed unsafe class GlCanvas : IDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Api {
        public uint Version, Size;
        public nint Open, Close, Begin, Present, Rect, Text, Measure, Texture, Image, DeleteTexture, Error, Clip, Artwork;
    }
    private Api _api;
    public void SetLibraryClip(bool enabled){if(_opened)((delegate* unmanaged<int,void>)_api.Clip)(enabled?1:0);}
    private bool _opened;
    private bool _borrowed;
    private Surface _fallback;
    private readonly Dictionary<nint,uint> _textures=new();
    public bool IsOpen=>_opened;
    public float Opacity {get;set;}=1;
    private Color Tint(Color color)=>color.WithAlpha((byte)((color.Value>>24)*Math.Clamp(Opacity,0,1)));
    public void Adopt(nint address) {
        if(address==0 || _opened)throw new IOException("Invalid native graphics handoff");
        Api api=*(Api*)address;
        if(api.Clip==0||api.Artwork==0)throw new IOException("Native graphics artwork unavailable");
        if(api.Version!=1||api.Size!=sizeof(Api)||api.Begin==0||api.Present==0||api.Rect==0||api.Text==0||api.Measure==0||api.Texture==0||api.Image==0||api.DeleteTexture==0||api.Error==0)
            throw new IOException("Native graphics interface mismatch");
        _api=api;_borrowed=true;_opened=true;
    }
    public void SetFallback(Surface surface)=>_fallback=surface;
    private string Error()=>Marshal.PtrToStringUTF8(((delegate* unmanaged<nint>)_api.Error)())??"OpenGL failed";
#if ATMOSPHERE_GL_PREVIEW
    public void OpenHost(nint library) {
        Api api=new(){Version=1,Size=(uint)sizeof(Api)};
        var start=(delegate* unmanaged<nuint,void*,int>)NativeLibrary.GetExport(library,"atmosphere_gl_start");
        if(start((nuint)sizeof(Api),&api)!=0)throw new IOException("Host GL handoff failed");
        _api=api;
        if(((delegate* unmanaged<int>)api.Open)()!=0)throw new IOException(Error());
        _opened=true;
    }
#endif
    public void Open() {
        byte[] path=Encoding.UTF8.GetBytes("/app0/sce_module/atmosphere_gl.prx\0");
        Api api=new(){Version=1,Size=(uint)sizeof(Api)}; int result=int.MinValue,handle;
        fixed(byte* p=path) handle=KernelModule.sceKernelLoadStartModule(p,(nuint)sizeof(Api),&api,0,null,&result);
        if(handle<0||result!=0)throw new IOException($"OpenGL module 0x{handle:X8}; init 0x{result:X8}");
        if(api.Version!=1||api.Size!=sizeof(Api)||api.Open==0||api.Close==0||api.Begin==0||api.Present==0||api.Rect==0||api.Text==0||api.Measure==0||api.Texture==0||api.Image==0||api.DeleteTexture==0||api.Error==0)throw new IOException("OpenGL interface mismatch");
        _api=api;
        if(((delegate* unmanaged<int>)api.Open)()!=0)throw new IOException(Error());
        _opened=true;
    }
    public void Present(){if(_opened && ((delegate* unmanaged<int>)_api.Present)()!=0)throw new IOException(Error());}
    public void Clear(Color color){if(_opened)((delegate* unmanaged<uint,void>)_api.Begin)(color.Value);else _fallback.Clear(color);}
    public void FillRoundedRect(int x,int y,int w,int h,int radius,Color color)=>Rect(x,y,w,h,radius,color,color);
    public void FillVerticalGradient(int x,int y,int w,int h,Color top,Color bottom)=>Rect(x,y,w,h,0,top,bottom);
    public void FillRoundedGradient(int x,int y,int w,int h,int radius,Color top,Color bottom)=>Rect(x,y,w,h,radius,top,bottom);
    public void StrokeRoundedRect(int x,int y,int w,int h,int radius,int thickness,Color color){
        int r=Math.Min(radius,Math.Min(w,h)/2),stroke=Math.Min(thickness,r);
        FillRoundedRect(x+r,y,w-2*r,stroke,0,color);
        FillRoundedRect(x+r,y+h-stroke,w-2*r,stroke,0,color);
        FillRoundedRect(x,y+r,stroke,h-2*r,0,color);
        FillRoundedRect(x+w-stroke,y+r,stroke,h-2*r,0,color);
        // Join all four edges with an annular quarter-circle, leaving the center clear.
        for(int row=0;row<r;row++){
            float dy=r-row-.5f;
            float outer=(float)Math.Sqrt(r*r-dy*dy);
            int innerRadius=r-stroke;
            float inner=dy<innerRadius?(float)Math.Sqrt(innerRadius*innerRadius-dy*dy):0;
            float width=outer-inner;
            Rect(x+r-outer,y+row,width,1,0,color,color);
            Rect(x+w-r+inner,y+row,width,1,0,color,color);
            Rect(x+r-outer,y+h-1-row,width,1,0,color,color);
            Rect(x+w-r+inner,y+h-1-row,width,1,0,color,color);
        }
    }
    private void Rect(float x,float y,float w,float h,float radius,Color top,Color bottom){
        top=Tint(top);bottom=Tint(bottom);
        if(_opened)((delegate* unmanaged<float,float,float,float,float,uint,uint,void>)_api.Rect)(x,y,w,h,radius,top.Value,bottom.Value);
        else if(top.Value==bottom.Value)_fallback.FillRoundedRect((int)x,(int)y,(int)w,(int)h,(int)radius,top);
        else _fallback.FillVerticalGradient((int)x,(int)y,(int)w,(int)h,top,bottom);
    }
    private static float FontSize(int scale)=>scale*8+2;
    private static bool IsButton(char c)=>"□△×○☰✚".Contains(c);
    public int TextWidth(string text,int scale) {
        int width=0,start=0;
        for(int i=0;i<=text.Length;i++)if(i==text.Length || IsButton(text[i])) {
            string part=text.Substring(start,i-start);
            if(!_opened)width+=Surface.MeasureText(part.AsSpan(),scale);
            else {byte[] b=Encoding.UTF8.GetBytes(part+"\0");fixed(byte* p=b)width+=(int)Math.Ceiling(((delegate* unmanaged<byte*,float,float>)_api.Measure)(p,FontSize(scale)));}
            if(i<text.Length)width+=12*scale;
            start=i+1;
        }
        return width;
    }
    private void Button(char button,int x,int y,int scale) {
        int size=8*scale,thickness=Math.Max(2,scale);
        Color color=button switch {'□'=>Color.FromRgb(238,149,202),'△'=>Color.FromRgb(111,220,179),'×'=>Color.FromRgb(127,183,255),'○'=>Color.FromRgb(246,149,150),_=>Color.White};
        void Line(int ax,int ay,int bx,int by) {
            int steps=Math.Max(Math.Abs(bx-ax),Math.Abs(by-ay));
            for(int i=0;i<=steps;i++){int px=ax+(bx-ax)*i/Math.Max(1,steps),py=ay+(by-ay)*i/Math.Max(1,steps);FillRoundedRect(x+px,y+py,thickness,thickness,0,color);}
        }
        switch(button) {
            case '□':Line(0,0,size,0);Line(size,0,size,size);Line(size,size,0,size);Line(0,size,0,0);break;
            case '△':Line(size/2,0,size,size);Line(size,size,0,size);Line(0,size,size/2,0);break;
            case '×':Line(0,0,size,size);Line(0,size,size,0);break;
            case '○':for(int i=0;i<40;i++){double a=i*Math.PI/20,b=(i+1)*Math.PI/20;Line((int)(size/2.0*(1+Math.Cos(a))),(int)(size/2.0*(1+Math.Sin(a))),(int)(size/2.0*(1+Math.Cos(b))),(int)(size/2.0*(1+Math.Sin(b))));}break;
            case '☰':Line(0,0,size,0);Line(0,size/2,size,size/2);Line(0,size,size,size);break;
            case '✚':Line(size/2,0,size/2,size);Line(0,size/2,size,size/2);break;
        }
    }
    public void DrawText(string text,int x,int y,int scale,Color color)=>DrawTextClipped(text,x,y,scale,color,1920-x);
    public void DrawTextClipped(string text,int x,int y,int scale,Color color,int width){
        if(text.Any(IsButton)) {
            int cursor=x,start=0;
            for(int i=0;i<=text.Length;i++)if(i==text.Length || IsButton(text[i])) {
                string part=text.Substring(start,i-start);
                int remaining=x+width-cursor;if(remaining<=0)return;
                if(part.Length>0)DrawTextClipped(part,cursor,y,scale,color,remaining);
                cursor+=TextWidth(part,scale);
                if(i<text.Length){if(cursor+10*scale>x+width)return;Button(text[i],cursor,y,scale);cursor+=12*scale;}
                start=i+1;
            }
            return;
        }
        color=Tint(color);
        if(!_opened){_fallback.DrawTextClipped(text,x,y,scale,color,width);return;}
        byte[] utf8=Encoding.UTF8.GetBytes(text+"\0");
        fixed(byte* p=utf8)((delegate* unmanaged<byte*,float,float,float,uint,float,void>)_api.Text)(p,x,y,FontSize(scale),color.Value,width);
    }
    public void DrawTextCentered(string text,int y,int scale,Color color){
        if(text.Any(IsButton)){DrawText(text,(1920-TextWidth(text,scale))/2,y,scale,color);return;}
        if(!_opened){_fallback.DrawTextCentered(text,y,scale,color);return;}
        byte[] utf8=Encoding.UTF8.GetBytes(text+"\0");float w;
        fixed(byte* p=utf8)w=((delegate* unmanaged<byte*,float,float>)_api.Measure)(p,FontSize(scale));
        DrawText(text,(int)((1920-w)/2),y,scale,color);
    }
    public void BlitScaled(Surface image,int x,int y,int w,int h,bool crop=false,int radius=16,float opacity=1,bool frosted=false){
        if(!_opened){_fallback.BlitScaled(image,x,y,w,h);return;}
        nint key=(nint)image.Pixels;
        if(!_textures.TryGetValue(key,out uint id)){
            // Decoders may pad their rows. Upload a tightly packed copy in that case.
            if(image.Stride==image.Width)id=((delegate* unmanaged<int,int,void*,int,uint>)_api.Texture)(image.Width,image.Height,image.Pixels,0);
            else {
                uint[] packed=new uint[checked(image.Width*image.Height)];
                for(int row=0;row<image.Height;row++)new ReadOnlySpan<uint>(image.Pixels+row*image.Stride,image.Width).CopyTo(packed.AsSpan(row*image.Width));
                fixed(uint* pixels=packed)id=((delegate* unmanaged<int,int,void*,int,uint>)_api.Texture)(image.Width,image.Height,pixels,0);
            }
            if(id==0)throw new IOException("OpenGL cover upload failed");
            _textures.Add(key,id);
        }
        if(crop)((delegate* unmanaged<uint,float,float,float,float,float,float,float,int,void>)_api.Artwork)(id,x,y,w,h,(float)image.Width/image.Height,radius,Math.Clamp(opacity*Opacity,0,1),frosted?1:0);
        else ((delegate* unmanaged<uint,float,float,float,float,void>)_api.Image)(id,x,y,w,h);
    }
    public void ClearImages(){if(_opened)foreach(uint id in _textures.Values)((delegate* unmanaged<uint,void>)_api.DeleteTexture)(id);_textures.Clear();}
    public bool BlitDds(DdsImage image,int x,int y,int w,int h,float opacity=1,int radius=0,bool frosted=false){
        if(!_opened)return false;
        if(!_textures.TryGetValue(image.Data,out uint id)){
            id=((delegate* unmanaged<int,int,void*,int,uint>)_api.Texture)(image.Width,image.Height,(void*)image.Data,image.Srgb?3:2);
            _textures.Add(image.Data,id);
        }
        if(id==0)return false;
        ((delegate* unmanaged<uint,float,float,float,float,float,float,float,int,void>)_api.Artwork)(id,x,y,w,h,(float)image.Width/image.Height,radius,opacity*Opacity,frosted?1:0);
        return true;
    }
    public void ReleaseDds(DdsImage image){if(_textures.Remove(image.Data,out uint id)&&_opened)((delegate* unmanaged<uint,void>)_api.DeleteTexture)(id);}
    public void ReleaseImage(Surface surface) {
        nint key=(nint)surface.Pixels;
        if(_textures.Remove(key,out uint texture) && _opened)((delegate* unmanaged<uint,void>)_api.DeleteTexture)(texture);
    }
    public void Dispose(){if(!_opened)return;ClearImages();if(!_borrowed)((delegate* unmanaged<void>)_api.Close)();_opened=false;_borrowed=false;}
}
