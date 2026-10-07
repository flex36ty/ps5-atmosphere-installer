using System;
using System.Runtime.InteropServices;
using System.Text;
using SharpProspero.Interop.SystemService;

namespace Atmosphere;
internal static unsafe partial class NativeExit
{
#if ATMOSPHERE_NATIVE_HOST
    private const string ExpectedTitle="PPSA99005";
#else
    private const string ExpectedTitle="FAKE34177";
#endif
    // PS5 ABI, as used by ps5-payload-dev/shsrv bundles/hbldr/hbldr.c.
    [LibraryImport("libSceSystemService", EntryPoint="sceSystemServiceKillApp")]
    private static partial int KillApp(int appId, int how, int reason, int coreDump);
    [LibraryImport("libkernel", EntryPoint="getpid")]
    private static partial int GetPid();
    [LibraryImport("libkernel", EntryPoint="sceKernelGetAppInfo")]
    private static partial int GetAppInfo(int pid, byte* info);

    public static int FindOwnApp()
    {
        // Both published app_info_t layouts are 96 bytes. ShadowMountPlus
        // places title_id at 16; the payload SDK/etaHEN definition uses 20.
        // Query only our PID, independent of shell title mappings.
        byte* info=stackalloc byte[96];
        new Span<byte>(info,96).Clear();
        int pid=GetPid();
        if(pid<=0)throw new InvalidOperationException("Cannot read Atmosphere process ID.");
        int rc=GetAppInfo(pid,info);
        if(rc!=0)throw new InvalidOperationException($"Cannot read own application info: 0x{rc:X8}");
        int id=*(int*)info;
        bool matches16=info[25]==0 && Encoding.ASCII.GetString(new ReadOnlySpan<byte>(info+16,9))==ExpectedTitle;
        bool matches20=info[29]==0 && Encoding.ASCII.GetString(new ReadOnlySpan<byte>(info+20,9))==ExpectedTitle;
        if(id<=0 || (!matches16 && !matches20))
            throw new InvalidOperationException($"Close identity mismatch: pid={pid} app=0x{id:X8} data={Convert.ToHexString(new ReadOnlySpan<byte>(info+16,14))}");
        return id;
    }
    public static int Close(int expectedId)
    {
        if(FindOwnApp()!=expectedId) throw new InvalidOperationException("The running application changed before close.");
        return KillApp(expectedId,-1,0,0);
    }
}
