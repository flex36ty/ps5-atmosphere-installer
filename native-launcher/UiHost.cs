#if ATMOSPHERE_NATIVE_HOST
using System;
using System.Runtime.InteropServices;
namespace Atmosphere;
internal static class UiHost
{
    internal static nint Graphics;
    [UnmanagedCallersOnly(EntryPoint="atmosphere_ui_run")]
    public static int Run(nint graphics,nint imageAllocate,nint imageFree)
    {
        try { SharpProspero.Graphics.ImageMemory.Configure(imageAllocate,imageFree);Graphics=graphics;using var app=new Launcher();app.Run();return 0; }
        catch(Exception e) { Console.Error.WriteLine("Atmosphere UI: "+e.Message);return -1; }
        finally { Graphics=0; }
    }
}
#endif
