using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading;
using SharpProspero.Interop.Kernel;
using SharpProspero.Interop.Net;
using SharpProspero.Interop.Sysmodule;

namespace Atmosphere;

internal sealed unsafe class NativeBackend
{
    private int _handle = -1;
    private nint _request, _free, _stage, _stop, _run;
    [StructLayout(LayoutKind.Sequential)]
    private struct Api { public uint Version, Size; public nint Run, Request, Free, Stage, Stop; }
    private bool _started;
    private volatile bool _exited, _closing;
    private int _result;
    public bool IsStopped => !_started || _exited;
    private readonly Queue<string> _commands = new();
    public void Enqueue(string command) { lock (_commands) _commands.Enqueue(command); }
    public void Run(Action<JsonElement> snapshot, Action<string> status)
    {
        try {
            Sysmodule.sceSysmoduleLoadModuleInternal(0x80000009);
            // The PS5 network library is needed for SMB, not for a local web server.
            NetPool.sceNetInit();
            if(_handle<0) {
                byte[] path=Encoding.UTF8.GetBytes("/app0/sce_module/atmosphere_backend.prx\0");
                int init=int.MinValue; int handle;
                Api api=new() {Version=1,Size=(uint)sizeof(Api)};
                fixed(byte* p=path) handle=KernelModule.sceKernelLoadStartModule(p,(nuint)sizeof(Api),&api,0,null,&init);
                if(handle<0) throw new IOException($"Module load 0x{handle:X8}; init 0x{init:X8}");
                _handle=handle;
                if(init!=0 || api.Version!=1 || api.Size!=(uint)sizeof(Api) || api.Run==0 || api.Request==0 || api.Free==0 || api.Stage==0 || api.Stop==0)
                    throw new IOException($"Backend interface handoff failed; init 0x{init:X8}. Close and reopen the app.");
                _run=api.Run; _request=api.Request; _free=api.Free; _stage=api.Stage; _stop=api.Stop;
            }
            if(!_started) {
                // Settings live in the title's own writable folder. External
                // destination permissions are checked by the transfer worker.
                status("Opening app storage...");
                if(_closing) return;
                new Thread(()=>{ _result=((delegate* unmanaged<int,nint,int>)_run)(0,0); _exited=true; },4*1024*1024)
                    {IsBackground=true,Name="Atmosphere SMB"}.Start();
                _started=true;
            }
            var timer=Stopwatch.StartNew();
            while(!_closing && ((delegate* unmanaged<int>)_stage)()<9) {
                if(_exited) throw new IOException(ExitError());
                int stage=((delegate* unmanaged<int>)_stage)();
                status("Starting: "+StageName(stage));
                if(timer.Elapsed.TotalSeconds>30) throw new IOException("Startup stalled at "+StageName(stage));
                Thread.Sleep(100);
            }
            while(!_closing) {
                if(_exited) throw new IOException(ExitError());
                string command="{}";
                lock(_commands) if(_commands.Count>0) command=_commands.Dequeue();
                byte[] bytes=Encoding.UTF8.GetBytes(command+"\0"); nint result;
                fixed(byte* p=bytes) result=((delegate* unmanaged<byte*,nint>)_request)(p);
                if(result==0) throw new IOException("The backend could not allocate its response.");
                try {
                    string text=Marshal.PtrToStringUTF8(result) ?? "{}";
                    using var doc=JsonDocument.Parse(text);
                    snapshot(doc.RootElement.Clone());
                } finally { ((delegate* unmanaged<nint,void>)_free)(result); }
                for(int i=0;i<5 && !_closing;i++) {
                    lock(_commands) if(_commands.Count>0) break;
                    Thread.Sleep(50);
                }
            }
        } catch(Exception e) { status("Startup failed: "+e.Message); }
    }
    private static string StageName(int stage)=>stage switch {
        0=>"backend thread",1=>"standard I/O",2=>"catalogue",3=>"storage",
        4=>"cached library",5=>"TLS and settings",6=>"random generator",7=>"SMB worker",_=>"native interface"
    };
    private string ExitError() {
        if(_result!=0 && _request!=0) {
            nint response=((delegate* unmanaged<byte*,nint>)_request)(null);
            if(response!=0) try {
                using var doc=JsonDocument.Parse(Marshal.PtrToStringUTF8(response)??"{}");
                if(doc.RootElement.TryGetProperty("error",out var error) && error.GetString() is string text && text.Length>0) return text;
            } finally {((delegate* unmanaged<nint,void>)_free)(response);}
        }
        return $"Backend exited: {_result} at {StageName(((delegate* unmanaged<int>)_stage)())}";
    }
    public void Stop() { _closing=true; if(_stop!=0 && !_exited) ((delegate* unmanaged<void>)_stop)(); }
    public static string Command(string action, params (string Name, object Value)[] fields)
    {
        using var memory=new MemoryStream();
        using(var writer=new Utf8JsonWriter(memory)) {
            writer.WriteStartObject(); writer.WriteString("action",action);
            foreach(var field in fields) {
                if(field.Value is bool b) writer.WriteBoolean(field.Name,b);
                else writer.WriteString(field.Name,field.Value.ToString());
            }
            writer.WriteEndObject();
        }
        return Encoding.UTF8.GetString(memory.ToArray());
    }
}
