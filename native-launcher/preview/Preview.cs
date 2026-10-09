using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using Atmosphere;
using SharpProspero.Application;
using SharpProspero.Graphics;
using SharpProspero.Input;
using SharpProspero.Interop.Pad;
internal static class Preview {
 static readonly BindingFlags Hidden=BindingFlags.Instance|BindingFlags.NonPublic;
 static void Set(object o,string n,object value)=>o.GetType().GetField(n,Hidden)!.SetValue(o,value);
 static object? Call(object o,string n,params object[] args)=>o.GetType().GetMethod(n,Hidden)!.Invoke(o,args);
 static unsafe void Main() {
  if(GlCanvas.ScrollOffset(64,0)!=0||GlCanvas.ScrollOffset(64,2)!=0||GlCanvas.ScrollOffset(64,3)!=32||GlCanvas.ScrollOffset(64,4)!=64||GlCanvas.ScrollOffset(64,5.99)!=64||GlCanvas.ScrollOffset(64,6)!=0||GlCanvas.ScrollOffset(64,7.99)!=0||GlCanvas.ScrollOffset(64,9)!=32||GlCanvas.ScrollOffset(-1,3)!=0)throw new Exception("Scrolling text timing failed");
  Console.WriteLine("PASS: one-way scrolling, two-second end hold, instant reset, two-second start hold and non-overflow behavior");
  byte[] command=EtaHenAccess.CreateRequest(1234);
  if(command.Length!=2576 || !command.AsSpan(0,16).SequenceEqual(new byte[]{239,190,173,222,5,0,0,0,210,4,0,0,199,250,255,255})) throw new Exception("IPC wire layout mismatch");
  for(int i=16;i<command.Length;i++) if(command[i]!=0) throw new Exception("IPC reserved bytes must be zero");
  EtaHenAccess.ValidateReply(command,1234);
  System.Buffers.Binary.BinaryPrimitives.WriteInt32LittleEndian(command.AsSpan(12),0);
  EtaHenAccess.ValidateReply(command,1234);
  static void Reject(byte[] packet,int pid) {try{EtaHenAccess.ValidateReply(packet,pid);}catch(IOException){return;}throw new Exception("Invalid IPC reply accepted");}
  Reject(command[..16],1234);Reject(command,99);
  System.Buffers.Binary.BinaryPrimitives.WriteInt32LittleEndian(command.AsSpan(12),-1);Reject(command,1234);
  command[0]=1;Reject(command,1234);
  Console.WriteLine("etaHEN IPC packet and reply validation: PASS");
  var app=new Launcher();
  Set(app,"_startupScanChecked",true);
  ScePadButton Stick(float x,float y,long time)=>(ScePadButton)Call(app,"StickNavigation",x,y,time)!;
  if(Stick(.2f,0,0)!=0 || Stick(.8f,0,10)!=ScePadButton.Right || Stick(.8f,0,359)!=0 || Stick(.8f,0,360)!=ScePadButton.Right || Stick(.8f,0,489)!=0 || Stick(.8f,0,490)!=ScePadButton.Right)throw new Exception("Stick dead zone or repeat timing failed");
  if(Stick(0,-.9f,500)!=ScePadButton.Up || Stick(0,0,510)!=0 || Stick(-.8f,.7f,520)!=ScePadButton.Left || Stick(0,0,530)!=0)throw new Exception("Stick release or dominant axis failed");
  Console.WriteLine("PASS: left-stick dead zone, repeat, release and direction changes");
  Set(app,"_frameDelta",.07);Set(app,"_modal","copy");Call(app,"AnimateModal");
  if((float)typeof(Launcher).GetField("_modalOpacity",Hidden)!.GetValue(app)! != .5f)throw new Exception("Popup fade-in midpoint failed");
  Call(app,"AnimateModal");Set(app,"_modal","");Set(app,"_frameDelta",.05);Call(app,"AnimateModal");
  if((string)typeof(Launcher).GetField("_visibleModal",Hidden)!.GetValue(app)! != "copy")throw new Exception("Popup removed before fade-out finished");
  Call(app,"AnimateModal");
  if((string)typeof(Launcher).GetField("_visibleModal",Hidden)!.GetValue(app)! != "")throw new Exception("Popup retained after fade-out");
  Console.WriteLine("PASS: popup fades in and remains visible through fade-out");
  nint glLibrary=0;
  var canvas=(GlCanvas)typeof(Launcher).GetField("_canvas",Hidden)!.GetValue(app)!;
  if(Environment.GetEnvironmentVariable("ATMOSPHERE_GL_TEST") is string glPath){glLibrary=NativeLibrary.Load(glPath);canvas.OpenHost(glLibrary);}
  using var doc=JsonDocument.Parse("""
  {"smb":{"activeSourceId":"s1","settings":{"name":"Living Room NAS","server":"192.168.0.113","share":"data2","folder":"ps5"},"sources":[{"id":"s1","enabled":true,"name":"Living Room NAS","protocol":"smb","server":"192.168.0.113","share":"data2","folder":"ps5"},{"id":"s2","enabled":true,"name":"FTP Server","protocol":"ftp","server":"192.168.0.114","folder":"DATA2/ps5"}],"games":[{"id":"1","sourceId":"s1","sourceName":"Living Room NAS","sourceProtocol":"smb","title":"Adventure Collection","titleId":"PPSA00001","minimumFirmware":"12.60","region":"EUR","backportFiles":true,"format":"ffpfsc","size":48000000000,"addedAt":3},{"id":"2","sourceId":"s2","sourceName":"FTP Server","sourceProtocol":"ftp","title":"Racing Collection","titleId":"PPSA00002","format":"folder","size":37000000000,"addedAt":1}],"installed":{"complete":true,"checking":false,"games":[{"titleId":"PPSA00001","location":"USB 0"}]},"job":{"title":"Adventure Collection","status":"copying","phase":"Copying","received":12000000000,"total":48000000000,"speedBytesPerSecond":80000000}},"storage":[{"id":"usb0","label":"USB Drive","freeBytes":900000000000,"external":true}]}
  """);
  Call(app,"UpdateData",doc.RootElement.Clone());
  var startup=new Launcher();Call(startup,"UpdateData",doc.RootElement.Clone());
  if(!(bool)typeof(Launcher).GetField("_refreshPending",Hidden)!.GetValue(startup)!)throw new Exception("Startup scan not requested");
  Set(startup,"_refreshPending",false);Call(startup,"UpdateData",doc.RootElement.Clone());
  if((bool)typeof(Launcher).GetField("_refreshPending",Hidden)!.GetValue(startup)!)throw new Exception("Startup scan repeated on snapshot");
  Console.WriteLine("PASS: automatic startup scan requested once per launch");
  var busyStartup=new Launcher();
  using(var busyDoc=JsonDocument.Parse(doc.RootElement.GetRawText().Replace("\"activeSourceId\":\"s1\"","\"busy\":true,\"activeSourceId\":\"s1\"")))Call(busyStartup,"UpdateData",busyDoc.RootElement.Clone());
  if((bool)typeof(Launcher).GetField("_startupScanChecked",Hidden)!.GetValue(busyStartup)!)throw new Exception("Startup scan consumed while busy");
  Call(busyStartup,"UpdateData",doc.RootElement.Clone());
  if(!(bool)typeof(Launcher).GetField("_refreshPending",Hidden)!.GetValue(busyStartup)!)throw new Exception("Deferred startup scan missing");
  var emptyStartup=new Launcher();
  using(var emptyDoc=JsonDocument.Parse("{\"smb\":{\"sources\":[]}}"))Call(emptyStartup,"UpdateData",emptyDoc.RootElement.Clone());
  if((bool)typeof(Launcher).GetField("_refreshPending",Hidden)!.GetValue(emptyStartup)! || (int)typeof(Launcher).GetField("_tab",Hidden)!.GetValue(emptyStartup)!=0)throw new Exception("Empty startup launched scan or changed tab");
  Console.WriteLine("PASS: startup scan waits for idle and skips unconfigured servers");
  Set(app,"_sourceRow",1);Call(app,"EditSettings");
  if((string)typeof(Launcher).GetField("_editSourceId",Hidden)!.GetValue(app)! != "s2" || !((string[])typeof(Launcher).GetField("_values",Hidden)!.GetValue(app)!).Contains("FTP Server"))throw new Exception("Edit did not target the highlighted second server");
  Set(app,"_sourceRow",0);Set(app,"_modal","");
  Console.WriteLine("PASS: edit form targets highlighted server instead of active server");
  static JsonElement State(string json)=>JsonDocument.Parse(json).RootElement.Clone();
  Call(app,"RefreshLibrary");Call(app,"RefreshLibrary");
  var backend=typeof(Launcher).GetField("_backend",Hidden)!.GetValue(app)!;
  var queue=backend.GetType().GetField("_commands",Hidden)!.GetValue(backend)!;
  if((int)queue.GetType().GetProperty("Count")!.GetValue(queue)!=1)throw new Exception("Duplicate refresh queued");
  Call(app,"UpdateData",State("""{"code":200,"smb":{"activeSourceId":"s1","message":"Scan complete: 2 games found.","busy":false}}"""));
  if(!(bool)typeof(Launcher).GetField("_refreshPending",Hidden)!.GetValue(app)!)throw new Exception("Stale snapshot ended refresh");
  Call(app,"UpdateData",State("""{"code":202,"smb":{"activeSourceId":"s1","message":"Reading folder","busy":true}}"""));
  if(((JsonElement[])typeof(Launcher).GetField("_games",Hidden)!.GetValue(app)!).Length!=2)throw new Exception("Refresh cleared cached games");
  Call(app,"UpdateData",State("""{"code":200,"smb":{"activeSourceId":"s1","message":"SMB connection timed out","busy":false}}"""));
  Call(app,"UpdateData",State("""{"code":200,"smb":{"activeSourceId":"s1","message":"","busy":false}}"""));
  if(!((string)typeof(Launcher).GetField("_status",Hidden)!.GetValue(app)!).Contains("timed out"))throw new Exception("Error disappeared");
  Console.WriteLine("PASS: refresh deduplication, stale snapshot handling, retained games and persistent error");
  Set(app,"_actionError","");Call(app,"UpdateData",doc.RootElement.Clone());
  var covers=(System.Collections.Generic.Dictionary<string,IDisposable>)typeof(Launcher).GetField("_covers",Hidden)!.GetValue(app)!;
  var retainedCover=new MemoryStream();covers["cache-test"]=retainedCover;
  Call(app,"UpdateData",doc.RootElement.Clone());
  if(!covers.ContainsKey("cache-test")||!retainedCover.CanWrite)throw new Exception("Repeated backend snapshot discarded cached covers");
  covers.Remove("cache-test");retainedCover.Dispose();
  var results=(System.Collections.Generic.Queue<(string Key,int Generation,IDisposable? Image)>)typeof(Launcher).GetField("_coverResults",Hidden)!.GetValue(app)!;
  var staleCover=new MemoryStream();results.Enqueue(("stale",-1,staleCover));Call(app,"DrainCoverResults",canvas);
  if(staleCover.CanWrite||covers.ContainsKey("stale"))throw new Exception("Stale cover decode was not discarded");
  Console.WriteLine("PASS: repeated snapshots retain covers; stale background results are released");
  Call(app,"RetryCover","retry-test",1000L);
  if((bool)Call(app,"CanLoadCover","retry-test",2999L)! || !(bool)Call(app,"CanLoadCover","retry-test",3000L)!)throw new Exception("Cover retry deadline is incorrect");
  Call(app,"RetryCover","retry-test",3000L);
  if((bool)Call(app,"CanLoadCover","retry-test",6999L)! || !(bool)Call(app,"CanLoadCover","retry-test",7000L)!)throw new Exception("Cover retry backoff is incorrect");
  Call(app,"ClearCovers");
  if(!(bool)Call(app,"CanLoadCover","retry-test",0L)!)throw new Exception("Source change retained a cover failure");
  Console.WriteLine("PASS: failed covers retry automatically with backoff, reset on source change");
  var context=(FrameContext)Activator.CreateInstance(typeof(FrameContext),true)!;
  uint* pixels=(uint*)NativeMemory.AllocZeroed(1920*1080*4);
  typeof(FrameContext).GetProperty("Surface")!.SetValue(context,new Surface(pixels,1920,1080));
  typeof(FrameContext).GetProperty("DeltaSeconds")!.SetValue(context,1.0/60);
  if(glLibrary!=0){
   canvas.Clear(Color.Black);canvas.SetLibraryClip(true);
   canvas.FillRoundedRect(0,0,1920,1080,0,Color.White);canvas.SetLibraryClip(false);
   var clipReadback=(delegate* unmanaged<void*,int>)NativeLibrary.GetExport(glLibrary,"atmosphere_gl_readback");
   if(clipReadback(pixels)!=0 || (pixels[(1079-100)*1920+100]&0xffffff)!=0 || (pixels[(1079-400)*1920+100]&0xffffff)!=0xffffff || (pixels[(1079-900)*1920+100]&0xffffff)!=0)throw new Exception("Library viewport leaked over header/footer");
   Console.WriteLine("PASS: scrolling library clips cards without covering header/footer");
   uint* sample=stackalloc uint[24];
   for(int row=0;row<4;row++)for(int x=0;x<6;x++)sample[row*6+x]=row<2?0xffff0000u:0xff0000ffu;
   canvas.Clear(Color.Black);canvas.BlitScaled(new Surface(sample,4,4,6),100,100,80,80);
   var readback=(delegate* unmanaged<void*,int>)NativeLibrary.GetExport(glLibrary,"atmosphere_gl_readback");
   if(readback(pixels)!=0 || (pixels[(1079-115)*1920+140]&0xffffff)!=0xff0000 || (pixels[(1079-165)*1920+140]&0xffffff)!=0x0000ff)throw new Exception("Cover texture channels, row stride or orientation incorrect");
   canvas.ClearImages();
   for(int i=0;i<24;i++)sample[i]=0xff00ff00;
   canvas.BlitScaled(new Surface(sample,4,4,6),100,100,80,80);
   if(readback(pixels)!=0 || (pixels[(1079-140)*1920+140]&0xffffff)!=0x00ff00)throw new Exception("Stale cover texture after cache clear");
   canvas.ClearImages();
   Console.WriteLine("PASS: OpenGL cover orientation, BGRA channels, padded rows and texture invalidation");
   uint* art=stackalloc uint[64*32];
   for(int y=0;y<32;y++)for(int x=0;x<64;x++)art[y*64+x]=x<16||x>=48?0xffff0000u:0xff00ff00u;
   canvas.Clear(Color.Black);canvas.BlitScaled(new Surface(art,64,32),100,100,160,160,true,24,.5f);
   if(readback(pixels)!=0)throw new Exception("Artwork readback failed");
   uint center=pixels[(1079-180)*1920+180]&0xffffff,corner=pixels[(1079-101)*1920+101]&0xffffff,edge=pixels[(1079-180)*1920+120]&0xffffff;
   if(center<0x007d00 || center>0x008100 || corner!=0 || edge!=center)throw new Exception("Artwork crop, rounded mask or opacity incorrect");
   canvas.ClearImages();
   Console.WriteLine("PASS: artwork center crop, rounded corners and background opacity");
   using(var stream=File.OpenRead("../../packaging-tools/ps5-native-app-boilerplate/sce_sys/pic0.dds"))
   using(var dds=DdsImage.Load(stream)){
    if(dds.Width!=3840||dds.Height!=2160||dds.ByteLength!=8294400)throw new Exception("DDS dimensions/size invalid");
    canvas.Clear(Color.Black);
    if(!canvas.BlitDds(dds,0,0,1920,1080)||readback(pixels)!=0)throw new Exception("BC7 GPU upload/render failed");
    canvas.ReleaseDds(dds);
   }
   try{DdsImage.Load(new MemoryStream(new byte[148]));throw new Exception("Invalid DDS accepted");}catch(IOException){}
   Console.WriteLine("PASS: 4K BC7 DDS validation, compressed GPU upload and rendering");
  }
  Directory.CreateDirectory("renders");
  using var destinationsDoc=JsonDocument.Parse(doc.RootElement.GetRawText().Replace("\"smb\":{","\"smb\":{\"destinations\":[{\"storageId\":\"usb0\",\"root\":\"/mnt/usb0\",\"hasSelection\":true,\"selected\":\"etaHEN/games\",\"folders\":[{\"folder\":\"homebrew\",\"sourceId\":\"\"},{\"folder\":\"etaHEN/games\",\"sourceId\":\"\"},{\"folder\":\"\",\"sourceId\":\"\"},{\"folder\":\"other-private\",\"sourceId\":\"s2\"}]}],"));
  Call(app,"UpdateData",destinationsDoc.RootElement.Clone());
  Set(app,"_copyGame",destinationsDoc.RootElement.GetProperty("smb").GetProperty("games")[0].Clone());
  var destination=((string Root,string[] Folders,string Selected,bool Legacy))Call(app,"CopyDestination",destinationsDoc.RootElement.GetProperty("storage")[0])!;
  if(destination.Selected!="etaHEN/games"||destination.Folders.Length!=3||destination.Legacy)throw new Exception("Destination preference or source isolation failed");
  if(((string[])typeof(Launcher).GetField("_keys",Hidden)!.GetValue(app)!).Contains("destinationFolder"))throw new Exception("Destination remains in server editor");
  Console.WriteLine("PASS: destination selector restores drive preference and excludes another server's legacy folder");
  foreach(var view in new[]{"library","refreshing","empty","sources","transfers","copy","deleteGame","duplicateSource","settings"}) {
   Call(app,"UpdateData",destinationsDoc.RootElement.Clone());
   Set(app,"_refreshActive",view=="refreshing");Set(app,"_refreshStarted",Environment.TickCount64-8000);
   if(view=="empty")Set(app,"_games",Array.Empty<JsonElement>());
   Set(app,"_tab",view=="sources"?1:view=="transfers"?2:0);Set(app,"_modal",view=="copy"?"copy":view=="deleteGame"?"deleteGame":view=="duplicateSource"?"duplicateSource":"");
   if(view=="deleteGame"){Set(app,"_deleteTitle","Adventure Collection");Set(app,"_deleteGame",State("""{"titleId":"PPSA00001","location":"USB 0","path":"/mnt/usb0/homebrew/Adventure Collection.ffpfsc"}"""));}
   if(view=="duplicateSource")Set(app,"_deleteSourceName","Living Room NAS");
   if(view=="settings") Call(app,"EditSettings");
   Set(app,"_visibleModal",(string)typeof(Launcher).GetField("_modal",Hidden)!.GetValue(app)!);
   Set(app,"_modalOpacity",1f);
   for(int frame=0;frame<30;frame++)Call(app,"OnFrame",context);
   if(glLibrary!=0){
    var readback=(delegate* unmanaged<void*,int>)NativeLibrary.GetExport(glLibrary,"atmosphere_gl_readback");
    if(readback(pixels)!=0)throw new Exception("OpenGL frame readback failed");
    for(int row=0;row<540;row++)for(int x=0;x<1920;x++){uint temp=pixels[row*1920+x];pixels[row*1920+x]=pixels[(1079-row)*1920+x];pixels[(1079-row)*1920+x]=temp;}
   }
   using var file=File.Create("renders/"+view+".bmp");using var writer=new BinaryWriter(file);
   writer.Write((ushort)0x4d42);writer.Write(54+1920*1080*4);writer.Write(0);writer.Write(54);writer.Write(40);writer.Write(1920);writer.Write(-1080);writer.Write((ushort)1);writer.Write((ushort)32);writer.Write(0);writer.Write(1920*1080*4);writer.Write(new byte[16]);writer.Write(new ReadOnlySpan<byte>(pixels,1920*1080*4));
  }
  NativeMemory.Free(pixels);
  canvas.Dispose();
  if(glLibrary!=0){canvas.OpenHost(glLibrary);canvas.Clear(Color.Black);canvas.Present();canvas.Dispose();Console.WriteLine("PASS: OpenGL close and reopen");}
  Console.WriteLine("Rendered native Library, Sources, Transfers, destination and settings screens.");
 }
}
