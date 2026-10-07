using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using SharpProspero.Application;
using SharpProspero.Platform;

namespace Atmosphere;
internal static unsafe partial class EtaHenAccess
{
    internal const int PacketSize=0xA10;
    internal static byte[] CreateRequest(int pid) {
        if(pid<=0) throw new ArgumentOutOfRangeException(nameof(pid));
        byte[] packet=new byte[PacketSize];
        BinaryPrimitives.WriteUInt32LittleEndian(packet,0xDEADBEEF);
        BinaryPrimitives.WriteInt32LittleEndian(packet.AsSpan(4),5);
        BinaryPrimitives.WriteInt32LittleEndian(packet.AsSpan(8),pid);
        BinaryPrimitives.WriteInt32LittleEndian(packet.AsSpan(12),-1337);
        return packet;
    }
    internal static void ValidateReply(ReadOnlySpan<byte> reply,int pid) {
        if(reply.Length!=PacketSize) throw new IOException("etaHEN returned an incomplete access reply.");
        uint magic=BinaryPrimitives.ReadUInt32LittleEndian(reply);
        if((magic!=0 && magic!=0xDEADBEEF) || BinaryPrimitives.ReadInt32LittleEndian(reply.Slice(4))!=5 || BinaryPrimitives.ReadInt32LittleEndian(reply.Slice(8))!=pid)
            throw new IOException("etaHEN returned an unexpected access reply.");
        int result=BinaryPrimitives.ReadInt32LittleEndian(reply.Slice(12));
        // Both values are accepted by etaHEN's documented client. Neither is
        // proof of access: verify the filesystem after the reply below.
        if(result!=0 && result!=-1337) throw new IOException($"etaHEN refused access: {result}");
    }
    [LibraryImport("libkernel",EntryPoint="access")]
    private static partial int Access(byte* path,int mode);
    private static bool CanAccess(ReadOnlySpan<byte> path,int mode) {
        fixed(byte* p=path) return Access(p,mode)==0;
    }
    public static void Request() {
        int pid=ProcessInfo.Id;
        try {
            using var connection=TcpConnection.Connect(SocketAddress.Loopback(9028));
            connection.SetReceiveTimeout(3_000_000);
            connection.SendAll(CreateRequest(pid));
            byte[] reply=new byte[PacketSize]; int received=0;
            var timer=Stopwatch.StartNew();
            while(received<reply.Length) {
                int remaining=5000-(int)timer.ElapsedMilliseconds;
                if(remaining<=0) throw new IOException("etaHEN access reply timed out.");
                connection.SetReceiveTimeout((uint)remaining*1000);
                int count=connection.Receive(reply.AsSpan(received));
                if(count==0) break;
                received+=count;
            }
            ValidateReply(reply.AsSpan(0,received),pid);
        } catch(Exception error) {
            throw new IOException("etaHEN access request failed. Enable Network and Legacy CMD server in etaHEN Toolbox. "+error.Message,error);
        }
        if(!CanAccess("/data\0"u8,2) || !CanAccess("/mnt\0"u8,4))
            throw new IOException("etaHEN replied, but access to /data or /mnt is still denied.");
    }
}
