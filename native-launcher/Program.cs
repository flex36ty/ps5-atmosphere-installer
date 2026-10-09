using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Threading;
using SharpProspero.Application;
using SharpProspero.Graphics;
using SharpProspero.Interop.Dialog;
using SharpProspero.Interop.Pad;
using SharpProspero.Interop.Sysmodule;
using SharpProspero.Modules;
using SharpProspero.Platform;

namespace Atmosphere;

internal sealed class Launcher : ProsperoApp
{
    private readonly GlCanvas _canvas=new();
    private float _focusX=70,_focusY=244,_tabX=748;
    private float _libraryScroll;
    private JpegImage? _fixedBackground;
    private double _frameDelta;
    private float[] _cardLift=System.Array.Empty<float>();
    private string _motionKey="";
    private double _textSeconds;
    private float _driveY=440,_sourceY=260,_fieldY=301,_transferProgress;
    private float Ease(float current,float target)=>current+(target-current)*(float)(1-Math.Exp(-14*_frameDelta));
    private void FocusRow(GlCanvas s,int x,int y,int width,int height) {
        s.FillRoundedRect(x-3,y-3,width+6,height+6,12,Blue.WithAlpha(22));
        s.FillRoundedRect(x,y,width,height,10,Blue.WithAlpha(45));
        s.FillRoundedRect(x,y+7,4,height-14,2,Blue);
    }
    private string _graphicsError="";
    protected override bool OpenExternalDisplay() {
#if ATMOSPHERE_NATIVE_HOST
        _canvas.Adopt(UiHost.Graphics);return true;
#elif !ATMOSPHERE_GL_PREVIEW
        // A one-launch opt-in: consume the marker before touching GPU code.
        // A crash or power cycle therefore returns to the software renderer.
        const string request="/app0/atmosphere-state/opengl.once";
        if(!SharpProspero.Storage.FileSystem.Exists(request))return false;
        try {
            SharpProspero.Storage.FileSystem.DeleteFile(request);
            _canvas.Open();return true;
        }
        catch(Exception e) { _graphicsError="OpenGL unavailable: "+e.Message;return false; }
#else
        return false;
#endif
    }
    protected override void PresentExternalDisplay()=>_canvas.Present();
    protected override void CloseExternalDisplay()=>_canvas.Dispose();
    private readonly NativeBackend _backend=new();
    private JsonElement _data;
    private JsonElement[] _games=System.Array.Empty<JsonElement>();
    private JsonElement[] _installedGames=System.Array.Empty<JsonElement>();
    private readonly int[] _librarySelections=new int[2];
    private readonly Dictionary<string,(long RetryAt,int Attempts)> _coverFailures=new();
    private string _status="Starting Atmosphere", _source="", _modal="";
    private string _visibleModal="";
    private float _modalOpacity;
    private void AnimateModal() {
        if(_modal.Length>0) {
            if(_visibleModal!=_modal){_visibleModal=_modal;_modalOpacity=0;}
            _modalOpacity=Math.Min(1,_modalOpacity+(float)(_frameDelta/.14));
        } else {
            _modalOpacity=Math.Max(0,_modalOpacity-(float)(_frameDelta/.10));
            if(_modalOpacity==0)_visibleModal="";
        }
    }
    private int _tab, _selected, _sourceRow, _drive, _field, _sort;
    private bool _ready, _remember, _passwordEdited;
    private bool _refreshPending, _refreshActive, _startupScanChecked;
    private string _actionError="", _lastRefresh="Not refreshed this session";
    private long _refreshStarted;
    private int _closingApp=-1;
    private long _closeDeadline;
    private string _copyId="", _copyTitle="";
    private JsonElement _copyGame;
    private string _deleteSourceId="",_deleteSourceName="";
    private JsonElement _deleteGame;
    private string _deleteTitle="",_deleteRequest="";
    private double _deletePreviousJob;
    private int _deleteSerial;
    private string _backgroundDeleteMessage="",_backgroundDeleteId="";
    private JsonElement DeleteTarget(JsonElement game)=>Array(Get(Get(Smb,"installed"),"deleteTargets")).FirstOrDefault(g=>Text(g,"titleId").Equals(Text(game,"titleId"),StringComparison.OrdinalIgnoreCase));
    private readonly string[] _keys={"name","server","share","folder","username","password","domain","protocol","port"};
    private readonly string[] _labels={"Server name","Server / IP","Share (SMB only)","Folder (from server root)","Username","Password","Domain (SMB only)","Protocol","Port"};
    private string[] _values=new string[9];
    private TextInputDialog? _input;
    private readonly Dictionary<string,IDisposable> _covers=new();
    private readonly Dictionary<string,long> _coverUse=new();
    private long _coverBytes,_coverTouch;
    private readonly HashSet<string> _coverPending=new();
    private readonly object _coverLoadLock=new();
    private readonly Queue<(string Key,int Generation,IDisposable? Image)> _coverResults=new();
    private readonly Queue<(string Key,string Cover,bool CachedPng,int Generation)> _coverRequests=new();
    private Thread? _coverWorker;
    private byte[] _coverReadBuffer=System.Array.Empty<byte>();
    private int _coverGeneration,_coverActive;
    private bool _coverStopped;
    private SystemModule? _pngModule, _jpegModule;
    private static readonly Color Ink=Color.FromRgb(9,14,25), Panel=Color.FromRgb(23,33,49),
        Blue=Color.FromRgb(112,224,207), Muted=Color.FromRgb(151,169,192);
    protected override void OnLoad()
    {
        try { _pngModule=SystemModule.Load(SystemModuleId.PngDec); _jpegModule=SystemModule.Load(SystemModuleId.JpegDec); } catch { }
        try {
            using var file=SharpProspero.Storage.FileSystem.OpenRead("/app0/ui/background.jpg");
            if(file.Length<=0 || file.Length>4*1024*1024)throw new System.IO.IOException("Invalid bundled background size");
            byte[] bytes=new byte[(int)file.Length];file.ReadExactly(bytes);
            _fixedBackground=JpegImage.Decode(bytes);
        } catch(Exception e) { ReportCoverFailure("bundled background",e); }
        new Thread(()=>_backend.Run(
            data=>Dispatcher.Post(()=>UpdateData(data)),
            text=>Dispatcher.Post(()=>_status=text))) {IsBackground=true,Name="Atmosphere UI bridge"}.Start();
    }
    private static JsonElement Get(JsonElement value,string key)=>value.ValueKind==JsonValueKind.Object && value.TryGetProperty(key,out var x)?x:default;
    private static string Text(JsonElement value,string key)=>Get(value,key).ValueKind==JsonValueKind.String?Get(value,key).GetString()??"":"";
    private static double Number(JsonElement value,string key)=>Get(value,key).TryGetDoubleSafe();
    private static bool Flag(JsonElement value,string key)=>Get(value,key).ValueKind==JsonValueKind.True;
    private static JsonElement[] Array(JsonElement value)=>value.ValueKind==JsonValueKind.Array?value.EnumerateArray().ToArray():System.Array.Empty<JsonElement>();
    private JsonElement Smb=>Get(_data,"smb");
    private string InstalledLocation(JsonElement game) {
        string id=Text(game,"titleId");if(id.Length==0)return "";
        return string.Join(" / ",Array(Get(Get(Smb,"installed"),"games"))
            .Where(g=>string.Equals(Text(g,"titleId"),id,StringComparison.OrdinalIgnoreCase))
            .Select(g=>Text(g,"location").Length>0?Text(g,"location"):"PS5 storage").Distinct());
    }
    private string InstallStatus(JsonElement game) {
        string location=InstalledLocation(game);if(location.Length>0)return "Installed · "+location;
        string id=Text(game,"titleId");
        if(id.Length!=9 || !(id.StartsWith("PPSA",StringComparison.OrdinalIgnoreCase)||id.StartsWith("CUSA",StringComparison.OrdinalIgnoreCase)) || !id.Skip(4).All(c=>c>='0'&&c<='9'))return "Install status unknown";
        var index=Get(Smb,"installed");return Flag(index,"checking")?"Checking installation…":Flag(index,"complete")?"Not installed":"Install status unknown";
    }
    // Tag fills blended 30% toward the panel background; text keeps its contrast.
    private static readonly Color TagId=Color.FromRgb(41,60,64),TagFirmware=Color.FromRgb(64,62,56),
        TagFormat=Color.FromRgb(55,55,74),TagBackport=Color.FromRgb(66,56,55),TagSize=Color.FromRgb(42,62,75),
        TagRegion=Color.FromRgb(48,59,78),TagNeutral=Color.FromRgb(43,55,69),TagInstalled=Color.FromRgb(44,63,58);
    private static string RegionLabel(JsonElement game)=>Text(game,"region") is "US" or "EUR" or "JPN" or "ASIA" or "KOR" ? Text(game,"region") : "Unknown";
    private static void MetadataTag(GlCanvas s,int x,int y,int width,string text,Color fill,int scale=2) {
        // A faint translucent surface and top edge keep tags soft on either panel.
        s.FillRoundedRect(x,y,width,33,9,Color.White.WithAlpha(6));
        s.FillRoundedRect(x,y,width,33,9,fill.WithAlpha(18));
        s.FillRoundedRect(x+9,y,width-18,1,0,Color.White.WithAlpha(11));
        s.DrawTextClipped(text,x+10,y+(scale==1?12:8),scale,Color.FromRgb(184,196,209),width-20);
    }
    private void StartCopy(bool allowDuplicate) {
        var drives=Array(Get(_data,"storage"));if(_drive<0||_drive>=drives.Length)return;
        var destination=CopyDestination(drives[_drive]);if(destination.Folders.Length==0){_actionError="No ShadowMountPlus copy folders are configured for this drive.";return;}
        Send("copy",("gameId",_copyId),("sourceId",Text(_copyGame,"sourceId")),("storageId",Text(drives[_drive],"id")),("destinationFolder",destination.Selected),("allowDuplicate",allowDuplicate));_modal="";SelectTab(3);
    }
    private int _exportServer,_exportLocation;
    private bool _exportSkipVerification;
    private float _exportY=442;
    private JsonElement[] ExportServers()=>Array(Get(Smb,"sources")).Where(x=>Text(x,"protocol") is "" or "smb" or "ftp").ToArray();
    private JsonElement[] ExportLocations()=>Array(Get(Get(Smb,"installed"),"games")).Where(x=>Text(x,"titleId")==Text(_copyGame,"titleId")&&Flag(x,"canExport")).ToArray();
    private bool CanExport(JsonElement game)=>Array(Get(Get(Smb,"installed"),"games")).Any(x=>Text(x,"titleId")==Text(game,"titleId")&&Flag(x,"canExport"));
    private void OpenExport(JsonElement game){
        if(Flag(Smb,"busy")){_actionError="Wait for the current transfer or scan to finish.";return;}
        _copyGame=game;_copyTitle=Text(game,"title");_exportServer=_exportLocation=0;
        _exportSkipVerification=true;
        if(ExportLocations().Length==0){_actionError=Flag(Get(Smb,"installed"),"sourceChecking")?"Still locating local game files. Try again when the installed scan finishes.":"The installed title's source could not be read. Refresh Installed Games; disconnected drives or registered packages cannot be copied.";return;}
        _modal="export";
    }
    private void DrawExport(GlCanvas s){
        Box(s,"COPY INSTALLED GAME TO SERVER");
        s.DrawTextClipped(_copyTitle,355,337,3,Color.White,1190);
        var locations=ExportLocations();var servers=ExportServers();
        _exportLocation=Math.Clamp(_exportLocation,0,Math.Max(0,locations.Length-1));
        _exportServer=Math.Clamp(_exportServer,0,Math.Max(0,servers.Length-1));
        s.DrawTextClipped(locations.Length>0?"□  From: "+Text(locations[_exportLocation],"location"):"Local source unavailable",355,394,2,Blue,1190);
        _exportY=Ease(_exportY,442+_exportServer*32);if(servers.Length>0)FocusRow(s,335,(int)_exportY,1240,32);
        for(int i=0;i<servers.Length;i++){
            int y=447+i*32;
            s.DrawTextClipped(Text(servers[i],"name")+"  ·  "+(Text(servers[i],"protocol")=="ftp"?"FTP":"SMB")+"  ·  /"+Text(servers[i],"folder"),355,y,2,i==_exportServer?Color.White:Muted,1190);
        }
        if(servers.Length==0)s.DrawText("Add an SMB or FTP server in Servers first.",355,460,2,Muted);
        s.DrawText(_exportSkipVerification?"△  Verify copy: OFF - file size only":"△  Verify copy: ON - full SHA-256 readback",355,690,2,Blue);
        s.DrawText(_exportSkipVerification?"Faster finish; file contents will not be verified.":"Reads the uploaded files back to check their contents.",355,724,2,Muted);
        s.DrawText("Local files are kept. Interrupted copies stay hidden on the server.",355,758,2,Muted);
        s.DrawText("×  Confirm copy     ○  Back",355,802,2,Blue);
    }
    private readonly Dictionary<string,string> _copyFolders=new();
    private (string Root,string[] Folders,string Selected,bool Legacy) CopyDestination(JsonElement drive){
        var d=Array(Get(Smb,"destinations")).FirstOrDefault(x=>Text(x,"storageId")==Text(drive,"id"));
        string root=Text(d,"root"),source=Text(_copyGame,"sourceId");
        var folders=Array(Get(d,"folders")).Where(x=>Text(x,"sourceId")==""||Text(x,"sourceId")==source).Select(x=>Text(x,"folder")).Distinct().ToArray();
        string selected=_copyFolders.TryGetValue(root,out var saved)?saved:Flag(d,"hasSelection")?Text(d,"selected"):"homebrew";
        if(!folders.Contains(selected))selected=folders.FirstOrDefault()??"";
        bool legacy=!Array(Get(d,"folders")).Any(x=>Text(x,"folder")==selected&&Text(x,"sourceId")=="");
        return (root,folders,selected,legacy);
    }
    private JsonElement[] Games()=>_tab==1?_installedGames:_games;
    private void SelectTab(int tab){
        if(_tab<2)_librarySelections[_tab]=_selected;
        _tab=tab;
        if(_tab<2){_selected=Math.Clamp(_librarySelections[_tab],0,Math.Max(0,Games().Length-1));_libraryScroll=Math.Max(0,_selected/3-1)*302;_focusX=70+(_selected%3)*590;_focusY=244+(_selected/3)*302;}
    }
    private void SortGames() {
        var games=_games;
        _games=_sort==0?games.OrderBy(x=>Text(x,"title"),StringComparer.OrdinalIgnoreCase).ToArray():games.OrderByDescending(x=>Number(x,"addedAt")).ToArray();
        _installedGames=_sort==0?_installedGames.OrderBy(x=>Text(x,"title"),StringComparer.OrdinalIgnoreCase).ToArray():_installedGames.OrderByDescending(x=>Number(x,"addedAt")).ToArray();
    }
    private void UpdateData(JsonElement data)
    {
        var selectedGames=Games();
        string selectedPath=selectedGames.Length>0?Text(selectedGames[Math.Clamp(_selected,0,selectedGames.Length-1)],"path"):"";
        string selectedSource=selectedGames.Length>0?Text(selectedGames[Math.Clamp(_selected,0,selectedGames.Length-1)],"sourceId"):"";
        _data=data; _ready=true;
        var background=Get(data,"deleteResult");
        string backgroundId=Text(background,"requestId")+Text(background,"state");
        if(backgroundId.Length>0&&backgroundId!=_backgroundDeleteId){_backgroundDeleteId=backgroundId;_backgroundDeleteMessage=Text(background,"message");}
        if(Flag(data,"closeForDelete")){
            try {_closingApp=NativeExit.FindOwnApp();_closeDeadline=Environment.TickCount64+15000;_backend.Stop();}
            catch(Exception e){_actionError="Close failed; deletion will cancel: "+e.Message;}
        }
        string source=Text(Smb,"activeSourceId");
        bool changed=source!=_source;
        if(changed) _source=source;
        if(Get(Smb,"games").ValueKind==JsonValueKind.Array) {
            var incoming=Array(Get(Smb,"games"));
            var installed=Get(Smb,"installedGames").ValueKind==JsonValueKind.Array?Array(Get(Smb,"installedGames")):_installedGames;
            // Transfer-progress snapshots repeat the library. Preserve its textures.
            var previousCovers=new Dictionary<string,string>();
            foreach(var game in _games.Concat(_installedGames))previousCovers[Text(game,"id")]=Text(game,"cover");
            bool coversChanged=incoming.Length+installed.Length!=_games.Length+_installedGames.Length || incoming.Concat(installed).Any(g=>!previousCovers.TryGetValue(Text(g,"id"),out string? cover) || cover!=Text(g,"cover"));
            _games=incoming;_installedGames=installed; SortGames(); if(coversChanged)ClearCovers();
            int retained=System.Array.FindIndex(Games(),g=>Text(g,"path")==selectedPath&&Text(g,"sourceId")==selectedSource);
            if(retained>=0)_selected=retained;
        }
        string error=Text(data,"error");
        string message=Text(Smb,"message");
        if(error.Length>0) {_actionError=error;_refreshPending=false;}
        if(_refreshPending && Flag(Smb,"busy")) {_refreshPending=false;_refreshActive=true;}
        if((_refreshActive || (_refreshPending && Number(data,"code")==202)) && !Flag(Smb,"busy") && (message.StartsWith("Scan complete",StringComparison.Ordinal) || _refreshActive)) {
            _refreshActive=false;_refreshPending=false;
            if(message.StartsWith("Scan complete",StringComparison.Ordinal)) _lastRefresh="Updated just now";
            else _actionError=message;
        }
        _status=_actionError.Length>0?_actionError:message;
        if(_deleteRequest.Length>0 && _actionError.Length==0){
            var index=Get(Smb,"installed");var action=Get(index,"action");var job=Get(index,"storageJob");
            if(Text(action,"id")==_deleteRequest){
                _status=Text(action,"message");
                if(Text(action,"state")=="error")_actionError=_status;
                if(Text(action,"state")=="complete" && Number(job,"id")!=_deletePreviousJob && Text(job,"operation")=="delete" && Text(job,"titleId")==Text(_deleteGame,"titleId"))
                    _status=Flag(job,"active")?"Deleting installed game…":Number(job,"result")==0?"Installed game deleted.":"Delete failed: "+Text(job,"error");
            }
        }
        if(_status.Length==0) _status="Ready";
        if(_backgroundDeleteMessage.Length>0 && _actionError.Length==0)_status=_backgroundDeleteMessage;
        _selected=Math.Clamp(_selected,0,Math.Max(0,Games().Length-1));
        if(!_startupScanChecked && Get(Smb,"sources").ValueKind==JsonValueKind.Array && !Flag(Smb,"busy")) {
            _startupScanChecked=true;
            if(Array(Get(Smb,"sources")).Any(x=>Flag(x,"enabled")&&Text(x,"server").Length>0))RefreshLibrary();
        }
    }
    private void Send(string action, params (string Name,object Value)[] fields) { _backgroundDeleteMessage="";if(action!="deleteInstalled")_deleteRequest="";_actionError="";_backend.Enqueue(NativeBackend.Command(action,fields)); _status="Working..."; }
    private void RefreshLibrary() {
        if(_refreshPending || _refreshActive || Flag(Smb,"busy")) return;
        if(_tab==1){Send("refreshInstalled");_status="Refreshing installed games...";return;}
        if(Array(Get(Smb,"sources")).Length==0){SelectTab(2);_status="Press □ to add a server.";return;}
        if(!Array(Get(Smb,"sources")).Any(x=>Flag(x,"enabled")&&Text(x,"server").Length>0)){SelectTab(2);_status="Configure and activate a server to refresh.";return;}
        _refreshPending=true;_refreshStarted=Environment.TickCount64;Send("scan");_status="Refresh requested...";
    }
    protected override void OnFrame(FrameContext context)
    {
        _canvas.SetFallback(context.Surface);
        DrainCoverResults(_canvas);
        var s=_canvas;
        _frameDelta=Math.Clamp(context.DeltaSeconds,0,0.1);
        string motionKey=$"{_tab}:{_selected}:{_sourceRow}:{_modal}:{_field}";
        if(motionKey!=_motionKey){_motionKey=motionKey;_textSeconds=0;}else _textSeconds+=_frameDelta;
        s.TextSeconds=_textSeconds;s.AnimateText=true;s.ResetTransform();
        s.Clear(Ink);
        DrawBackdrop(s);
        s.FillVerticalGradient(0,0,1920,240,Color.FromRgb(28,48,83).WithAlpha(200),Ink.WithAlpha(90));
        s.DrawText("ATMOSPHERE",76,54,5,Color.White);
        s.DrawText("YOUR PERSONAL GAME LIBRARY",78,110,2,Muted);
        string[] tabs={"LIBRARY","INSTALLED GAMES","SERVERS","TRANSFERS"};
        _tabX+=(748+_tab*272-_tabX)*(float)(1-Math.Exp(-18*_frameDelta));
        s.FillRoundedRect((int)_tabX,62,250,52,12,Panel);
        for(int i=0;i<4;i++) {
            int x=748+i*272+(250-s.TextWidth(tabs[i],2))/2;
            s.DrawText(tabs[i],x,79,2,_tab==i?Blue:Muted);
        }
        s.FillRoundedRect(76,925,1760,55,12,Panel);
        s.DrawTextClipped(_status,98,943,2,_actionError.Length>0?Color.FromRgb(255,166,145):Muted,1710);
        s.DrawText("L1 / R1  Tabs     ✚  Navigate     ×  Select     ○  Back     ☰  Exit",76,1000,2,Color.White);
        if(_graphicsError.Length>0)s.DrawTextClipped(_graphicsError,76,1036,2,Color.FromRgb(255,166,145),1760);
        if(_closingApp>=0) {
            s.DrawTextCentered("Closing Atmosphere",420,4,Color.White);
            if(_backend.IsStopped) {
                int appId=_closingApp; _closingApp=-1;
                try {int rc=NativeExit.Close(appId);_status=rc<0?$"Close failed: 0x{rc:X8}":"Close requested. Waiting for the system.";}
                catch(Exception e){_status=e.Message;}
            } else if(Environment.TickCount64>_closeDeadline) {
                _closingApp=-1;_status="Transfers are still stopping. Press OPTIONS to try closing again.";
            }
            return;
        }
        if(!_ready) { s.DrawTextCentered("Loading the native library",420,4,Color.White); HandleExit(context); return; }
        if(_tab<2) DrawLibrary(s); else if(_tab==2) DrawSources(s); else DrawTransfer(s);
        AnimateModal();
        s.Opacity=_modalOpacity*_modalOpacity*(3-2*_modalOpacity);
        if(_visibleModal=="copy") DrawCopy(s);
        else if(_visibleModal=="export") DrawExport(s);
        else if(_visibleModal=="settings") DrawSettings(s);
        else if(_visibleModal=="duplicateSource") {
            Box(s,"DUPLICATE SERVER?");
            s.DrawTextClipped(_deleteSourceName,355,350,4,Color.White,1190);
            s.DrawText("Copies connection settings and saved login preferences.",355,445,2,Muted);
            s.DrawText("The new entry starts inactive so you can edit it first.",355,495,2,Muted);
            s.DrawText("×  Duplicate server     ○  Cancel",355,700,3,Blue);
        }
        else if(_visibleModal=="deleteSource") {
            Box(s,"DELETE SERVER?");
            s.DrawTextClipped(_deleteSourceName,355,350,4,Color.White,1190);
            s.DrawText("Removes this connection and its library from Atmosphere.",355,445,2,Muted);
            s.DrawText("Game files on your server and storage are kept.",355,495,2,Muted);
            s.DrawText("×  Delete server     ○  Keep server",355,700,3,Blue);
        }
        else if(_visibleModal=="deleteGame") {
            Box(s,"DELETE INSTALLED GAME?");
            s.DrawTextClipped(_deleteTitle,355,345,4,Color.White,1190);
            s.DrawTextClipped(Text(_deleteGame,"location"),355,425,3,Blue,1190);
            s.DrawTextClipped(Text(_deleteGame,"path"),355,475,2,Muted,1190);
            s.DrawText("Permanently deletes this installed folder or image.",355,545,2,Muted);
            s.DrawText("Atmosphere will close, then delete. Your server copy is kept.",355,590,2,Muted);
            s.DrawText("×  Close and delete     ○  Cancel",355,700,3,Blue);
        }
        else if(_visibleModal=="duplicate") {
            Box(s,"ALREADY INSTALLED");
            s.DrawTextClipped(_copyTitle,355,350,4,Color.White,1190);
            s.DrawTextClipped(InstallStatus(_copyGame),355,440,3,Blue,1190);
            s.DrawText("The same title ID is already present. Copy another instance?",355,510,2,Muted);
            s.DrawText("Existing game files will not be overwritten.",355,555,2,Muted);
            s.DrawText("×  Copy anyway     ○  Back",355,700,3,Blue);
        }
        else if(_visibleModal=="cancel") { Box(s,"Cancel this transfer?"); s.DrawTextCentered("×  Confirm     ○ Keep transferring",520,3,Color.White); }
        s.Opacity=1;
        if(_input!=null) {
            if(_input.Update()==TextInputState.Finished) {
                if(_input.EndStatus==ImeDialogEndStatus.Ok) { _values[_field]=_input.Text; if(_field==5) _passwordEdited=true; }
                _input.Dispose(); _input=null;
            }
            return;
        }
        HandleInput(context);
    }
    private void DrawLibrary(GlCanvas s)
    {
        var games=Games(); var cfg=Get(Smb,"settings");
        bool refreshing=_tab==1?Flag(Get(Smb,"installed"),"sourceChecking"):_refreshPending||_refreshActive;
        s.DrawTextClipped(_tab==1?"Installed Games":"All active servers",76,178,3,Color.White,700);
        s.DrawText($"{games.Length} GAMES  /  {(_sort==0?"A - Z":"NEWEST FIRST")}",830,184,2,Muted);
        s.FillRoundedRect(1370,158,466,62,16,refreshing?Panel:Color.FromRgb(32,73,75));
        s.DrawText(refreshing?"REFRESH IN PROGRESS":Flag(Smb,"busy")?"TRANSFER ACTIVE":"□  REFRESH LIBRARY",1396,180,2,Blue);
        if(games.Length==0) {
            s.FillRoundedRect(370,310,1180,380,24,Panel);
            s.DrawTextCentered(refreshing?"Finding your games":_tab==1?"No installed games found":"Your library starts here",390,4,Color.White);
            s.DrawTextCentered(_tab==1?"Scanning internal storage and connected drives.":refreshing?"Reading your servers. Games will appear shortly.":"Add a server, then press □ to refresh.",474,2,Muted);
            s.DrawTextCentered(refreshing?"You can keep navigating while the scan runs.":"No games are removed from your server.",530,2,Blue);
        }
        int row=_selected/3;
        float targetScroll=Math.Max(0,row-1)*302;
        _libraryScroll+=(targetScroll-_libraryScroll)*(float)(1-Math.Exp(-9*_frameDelta));
        if(Math.Abs(targetScroll-_libraryScroll)<.5f)_libraryScroll=targetScroll;
        int first=Math.Max(0,(int)(_libraryScroll/302))*3;
        float targetX=70+(_selected%3)*590,targetY=244+row*302;
        float ease=(float)(1-Math.Exp(-14*_frameDelta));
        _focusX+=(targetX-_focusX)*ease;_focusY+=(targetY-_focusY)*ease;
        s.SetLibraryClip(true);
        if(_cardLift.Length!=games.Length)_cardLift=new float[games.Length];
        for(int i=0;i<_cardLift.Length;i++)_cardLift[i]=Ease(_cardLift[i],i==_selected?1:0);
        if(games.Length>0){
            int fx=(int)_focusX,fy=(int)(_focusY-_libraryScroll);
            s.CenterX=fx+279;s.CenterY=fy+140;s.Zoom=1+.025f*_cardLift[_selected];s.Lift=5*_cardLift[_selected];
            var edge=Color.FromRgb(112,211,224);
            // Keep the moving frame outside the card so its tint cannot hide it.
            s.FillRoundedRect(fx-3,fy-3,564,286,22,edge.WithAlpha(24));
            s.StrokeRoundedRect(fx,fy,558,280,22,4,edge);
            s.ResetTransform();
        }
        int end=Math.Min(first+12,games.Length);
        // Draw the selected card last so its raised surface stays in front.
        for(int draw=first;draw<=end;draw++) {
            int i=draw==end?_selected:draw;
            if(i<first||i>=end||(draw!=end&&i==_selected))continue;
            int x=76+(i%3)*590,y=250+(i/3)*302-(int)_libraryScroll;
            if(y>=824 || y+280<=240)continue;
            bool selected=i==_selected;
            s.CenterX=x+273;s.CenterY=y+134;s.Zoom=1+.025f*_cardLift[i];s.Lift=5*_cardLift[i];s.AnimateText=selected;
            s.FillRoundedRect(x-3,y+10,552,270,20,Color.Black.WithAlpha((byte)(20+40*_cardLift[i])));
            s.FillRoundedRect(x,y+5,546,268,18,Color.Black.WithAlpha(25));
            // A tinted base keeps the wallpaper subdued beneath the frosted sheen.
            s.FillRoundedRect(x,y,546,268,18,selected?Color.FromRgb(37,66,79).WithAlpha(235):Panel.WithAlpha(180));
            s.FillRoundedGradient(x,y,546,268,18,
                Color.FromRgb(204,217,229).WithAlpha(selected?(byte)52:(byte)27),
                Color.FromRgb(130,151,174).WithAlpha(selected?(byte)20:(byte)9));
            s.FillRoundedRect(x+18,y+1,510,1,0,Color.White.WithAlpha(selected?(byte)80:(byte)31));
            DrawCover(s,games[i],x+14,y+24);
            string format=Text(games[i],"format").ToUpperInvariant();
            if(format.Length>0) {
                int badgeWidth=Math.Min(200,s.TextWidth(format,2)+22);
                s.FillRoundedRect(x+24,y+34,badgeWidth,32,8,TagFormat);
                s.DrawTextClipped(format,x+35,y+42,2,Color.White,badgeWidth-22);
            }
            s.DrawTextClipped(Text(games[i],"title"),x+250,y+30,3,Color.White,280);
            MetadataTag(s,x+250,y+66,132,Text(games[i],"titleId"),TagId);
            MetadataTag(s,x+390,y+66,144,"Min FW "+(Text(games[i],"minimumFirmware").Length>0?Text(games[i],"minimumFirmware"):"?"),TagFirmware);
            MetadataTag(s,x+390,y+108,144,Flag(games[i],"localOnly")&&Get(games[i],"backportFiles").ValueKind==JsonValueKind.Undefined?"Backport ?":Flag(games[i],"backportFiles")?"Backported":"Not Backported",Flag(games[i],"backportFiles")?TagBackport:TagNeutral);
            MetadataTag(s,x+250,y+108,132,Flag(games[i],"localOnly")&&Number(games[i],"size")==0?"Size unknown":Size(Number(games[i],"size")),TagSize);
            MetadataTag(s,x+390,y+150,144,RegionLabel(games[i]),TagRegion);
            bool installed=InstalledLocation(games[i]).Length>0;
            MetadataTag(s,x+250,y+150,132,installed?"Installed":InstallStatus(games[i]),installed?TagInstalled:TagNeutral);
            MetadataTag(s,x+250,y+192,284,Flag(games[i],"localOnly")?"Local: "+Text(games[i],"sourceName"):"Server: "+Text(games[i],"sourceProtocol").ToUpperInvariant()+" · "+Text(games[i],"sourceName"),TagNeutral);
            if(selected) { if(!Flag(games[i],"localOnly")||CanExport(games[i]))s.DrawText("× COPY",x+250,y+237,2,Blue); if(DeleteTarget(games[i]).ValueKind==JsonValueKind.Object)s.DrawText("R2 DELETE",x+390,y+237,2,Color.FromRgb(242,105,115)); }
            s.ResetTransform();s.AnimateText=true;
        }
        s.SetLibraryClip(false);
        // Visible rows take priority; warm only the next two rows, using the same
        // bounded worker queue and persistent PNG paths already in smb-state.
        for(int i=first+6;i<Math.Min(first+12,games.Length);i++)QueueCover(games[i]);
        s.DrawText(_tab==1?(refreshing?"SCANNING LOCAL STORAGE":"INTERNAL + CONNECTED STORAGE"):refreshing?$"SCANNING  /  {Math.Max(0,(Environment.TickCount64-_refreshStarted)/1000)}s":_lastRefresh,76,850,2,Blue);
        if(games.Length>0)s.DrawText($"GAME {_selected+1} / {games.Length}",1560,850,2,Muted);
        if(refreshing) {
            s.FillRoundedRect(76,827,1760,4,2,Panel);
            int offset=(int)((Environment.TickCount64/5)%1540);
            s.FillRoundedRect(76+offset,827,220,4,2,Blue);
        }
        s.DrawText("□  Refresh     △  Sort     L2  Servers     ×  Copy",76,890,2,Muted);
        if(games.Length>0&&CanExport(games[Math.Clamp(_selected,0,games.Length-1)]))s.DrawText("R3  Copy to server",1100,890,2,Blue);
    }
    private void DrawBackdrop(GlCanvas s) {
        if(_fixedBackground!=null)s.BlitScaled(_fixedBackground.AsSurface(),0,0,1920,1080,true,0,1,true);
        s.FillVerticalGradient(0,0,1920,1080,Ink.WithAlpha(100),Ink.WithAlpha(205));
    }
    private void DrawCover(GlCanvas s,JsonElement game,int x,int y,int width=220,int height=220)
    {
        QueueCover(game);
        string key=Text(game,"id");
        if(_covers.TryGetValue(key,out var item)) {
            _coverUse[key]=++_coverTouch;
            Surface image=item is PngImage p?p.AsSurface():((JpegImage)item).AsSurface();
            s.BlitScaled(image,x,y,width,height,true,20);
        } else {
            s.FillRoundedRect(x,y,width,height,20,Color.FromRgb(42,61,91));
            s.DrawText("PS5",x+width/2-49,y+height/2-24,5,Blue);
        }
    }
    private void QueueCover(JsonElement game) {
        string key=Text(game,"id"), cover=Text(game,"cover");
        bool cachedPng=cover.StartsWith("/app0/atmosphere-state/cover-",StringComparison.Ordinal) && cover.EndsWith(".png",StringComparison.Ordinal) && !cover.Contains("..") && cover.LastIndexOf('/')==22;
        if(!_covers.ContainsKey(key) && CanLoadCover(key,Environment.TickCount64) && !_coverPending.Contains(key) && (cachedPng || cover.StartsWith("data:image/",StringComparison.Ordinal)) && cover.Length<6*1024*1024) {
            lock(_coverLoadLock) {
                if(!_coverStopped && _coverActive<2) {
                    if(_coverWorker==null) {
                        var worker=new Thread(CoverWorker) {IsBackground=true,Name="Atmosphere covers"};
                        worker.Start();_coverWorker=worker;
                    }
                    _coverActive++;_coverPending.Add(key);
                    _coverRequests.Enqueue((key,cover,cachedPng,_coverGeneration));
                    Monitor.Pulse(_coverLoadLock);
                }
            }
        }
    }
    private void CoverWorker() {
        while(true) {
            (string Key,string Cover,bool CachedPng,int Generation) request;
            lock(_coverLoadLock) {
                while(!_coverStopped && _coverRequests.Count==0)Monitor.Wait(_coverLoadLock);
                if(_coverStopped)return;
                request=_coverRequests.Dequeue();
            }
            LoadCover(request.Key,request.Cover,request.CachedPng,request.Generation);
        }
    }
    private void LoadCover(string key,string cover,bool cachedPng,int generation) {
        IDisposable? decoded=null;
        string stage="open cached file";
        try {
            byte[] bytes;int length;
            if(cachedPng) {
                using var file=SharpProspero.Storage.FileSystem.OpenRead(cover);
                long size=file.Length;
                if(size<8 || size>4*1024*1024)throw new System.IO.IOException("Invalid cover size");
                length=(int)size;stage="allocate compressed buffer";
                if(_coverReadBuffer.Length<length)_coverReadBuffer=new byte[Math.Min(16*1024*1024,Math.Max(length,_coverReadBuffer.Length*2))];
                bytes=_coverReadBuffer;stage="read cached file";file.ReadExactly(bytes.AsSpan(0,length));
            } else {stage="decode embedded data";bytes=Convert.FromBase64String(cover[(cover.IndexOf(',')+1)..]);length=bytes.Length;}
            stage="decode image pixels";
            if(cachedPng) {
                var header=bytes.AsSpan(0,length);
                if(header.Length<24)throw new System.IO.IOException("Truncated artwork header");
                uint width=System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(header.Slice(16,4));
                uint height=System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(header.Slice(20,4));
                if(width==0||height==0||(ulong)width*height>3840UL*2160)throw new System.IO.IOException("Artwork exceeds 4K pixel budget");
            }
            decoded=cachedPng || cover.StartsWith("data:image/png",StringComparison.Ordinal)?PngImage.Decode(bytes.AsSpan(0,length)):JpegImage.Decode(bytes.AsSpan(0,length));
        } catch(Exception e) { ReportCoverFailure(key+" ["+stage+"]",e); }
        lock(_coverLoadLock) {
            _coverActive--;
            if(_coverStopped){decoded?.Dispose();return;}
            _coverResults.Enqueue((key,generation,decoded));
        }
    }
    private static unsafe void ReportCoverFailure(string key,Exception error) {
        // Console.Error lazily opens managed standard streams, which can itself
        // throw on this host. Diagnostics must never escape the cover worker.
        try {
            byte[] message=System.Text.Encoding.UTF8.GetBytes("Cover load failed: "+key+": "+error.Message+"\n");
            byte[] path=System.Text.Encoding.UTF8.GetBytes("/app0/atmosphere-state/cover-errors.log\0");
            int fd;fixed(byte* p=path)fd=SharpProspero.Interop.Kernel.KernelFile.sceKernelOpen(p,0x209,0x180);
            if(fd>=0)try {fixed(byte* p=message)SharpProspero.Interop.Kernel.KernelFile.sceKernelWrite(fd,p,(nuint)message.Length);}
            finally {SharpProspero.Interop.Kernel.KernelFile.sceKernelClose(fd);}
        } catch { }
    }
    private bool CanLoadCover(string key,long now)=>!_coverFailures.TryGetValue(key,out var failure)||now>=failure.RetryAt;
    private void RetryCover(string key,long now) {
        int attempts=_coverFailures.TryGetValue(key,out var previous)?Math.Min(previous.Attempts+1,5):1;
        _coverFailures[key]=(now+Math.Min(30000,1000L<<attempts),attempts);
    }
    private void DrainCoverResults(GlCanvas canvas) {
        while(true) {
            (string Key,int Generation,IDisposable? Image) result;
            lock(_coverLoadLock) {if(_coverResults.Count==0)return;result=_coverResults.Dequeue();}
            if(result.Generation!=_coverGeneration){result.Image?.Dispose();continue;}
            _coverPending.Remove(result.Key);
            if(result.Image==null){RetryCover(result.Key,Environment.TickCount64);continue;}
            _coverFailures.Remove(result.Key);
            long bytes=CoverBytes(result.Image);
            if(bytes>32L*1024*1024){result.Image.Dispose();RetryCover(result.Key,Environment.TickCount64);continue;}
            while(_covers.Count>0 && (_covers.Count>=16 || _coverBytes+bytes>32L*1024*1024)){
                string oldest=_covers.Keys.OrderBy(k=>_coverUse.GetValueOrDefault(k)).First();var image=_covers[oldest];
                if(image is DdsImage dds)canvas.ReleaseDds(dds);else canvas.ReleaseImage(image is PngImage png?png.AsSurface():((JpegImage)image).AsSurface());
                _coverBytes-=CoverBytes(image);image.Dispose();_covers.Remove(oldest);_coverUse.Remove(oldest);
            }
            _covers[result.Key]=result.Image;_coverBytes+=bytes;_coverUse[result.Key]=++_coverTouch;
        }
    }
    private static long CoverBytes(IDisposable image)=>image is DdsImage dds?dds.ByteLength:image is PngImage p?(long)p.Width*p.Height*4:(long)((JpegImage)image).Width*((JpegImage)image).Height*4;
    private static void ServerAttribute(GlCanvas s,string label,string value,int x,int y,int width,Color border)
    {
        s.FillRoundedRect(x,y,width,82,10,border.WithAlpha(185));
        s.FillRoundedRect(x+2,y+2,width-4,78,8,Panel);
        s.DrawText(label,x+14,y+12,2,Muted);
        s.DrawTextClipped(value,x+14,y+44,2,label=="STATUS"?border:Color.White,width-28);
    }
    private void DrawSources(GlCanvas s)
    {
        var sources=Array(Get(Smb,"sources"));
        _sourceRow=Math.Clamp(_sourceRow,0,Math.Max(0,sources.Length-1));
        s.DrawText("SERVERS",76,184,4,Color.White);
        int first=(_sourceRow/5)*5;
        _sourceY=Ease(_sourceY,260+(_sourceRow-first)*116);
        for(int i=first;i<Math.Min(first+5,sources.Length);i++)s.FillRoundedRect(76,260+(i-first)*116,1760,104,12,Panel);
        if(sources.Length>0)FocusRow(s,76,(int)_sourceY,1760,104);
        for(int i=first;i<Math.Min(first+5,sources.Length);i++) {
            int y=271+(i-first)*116;
            var source=sources[i];
            bool active=Flag(source,"enabled");
            string name=Text(source,"name"),protocol=Text(source,"protocol") switch {"ftp"=>"FTP","webdav"=>"WebDAV","webdavs"=>"WebDAV TLS",_=>"SMB"};
            string path=string.Join("/",new[]{Text(source,"share").Trim('/'),Text(source,"folder").Trim('/')}.Where(v=>v.Length>0));
            ServerAttribute(s,"STATUS",active?"ACTIVE":"NOT ACTIVE",90,y,160,active?Color.FromRgb(91,216,140):Color.FromRgb(242,105,115));
            ServerAttribute(s,"NAME",name.Length>0?name:Text(source,"server"),262,y,320,Blue);
            ServerAttribute(s,"PROTOCOL",protocol,594,y,140,Color.FromRgb(83,155,255));
            ServerAttribute(s,"IP ADDRESS",Text(source,"server"),746,y,320,Blue);
            ServerAttribute(s,"PATH",path.Length>0?path:"/",1078,y,744,Blue);
        }
        if(sources.Length==0)s.DrawText("No servers saved. Press □ to add a server.",100,300,3,Muted);
        s.DrawText("×  Activate / Deactivate   □  Add   △  Edit   L2  Duplicate   R2  Delete",76,890,2,Muted);
    }
    private void DrawTransfer(GlCanvas s)
    {
        var job=Get(Smb,"job"); s.DrawText("TRANSFER",76,184,4,Color.White);
        s.FillRoundedRect(76,270,1760,440,20,Panel);
        string title=Text(job,"title"); s.DrawTextClipped(title.Length>0?title:"No transfers yet",120,320,4,Color.White,1660);
        s.DrawText(Text(job,"status")+"    "+Text(job,"phase"),120,410,3,Blue);
        double total=Number(job,"total"), received=Number(job,"received");
        s.FillRoundedRect(120,488,1640,26,10,Ink);
        float progress=total>0?(float)Math.Clamp(received/total,0,1):0;
        _transferProgress=progress<_transferProgress?progress:Ease(_transferProgress,progress);
        if(_transferProgress>0) s.FillRoundedRect(120,488,(int)(1640*_transferProgress),26,10,Blue);
        s.DrawText(Size(received)+" / "+Size(total)+"    "+(Number(job,"speedBytesPerSecond")/1048576).ToString("F1")+" MB/s",120,550,3,Color.White);
        s.DrawTextClipped(Text(job,"error"),120,620,2,Muted,1640);
        s.DrawText(Text(job,"direction")=="upload"?"□  Cancel server backup":"×  Pause / Resume     □  Cancel transfer",76,890,2,Muted);
        if(Text(job,"direction")=="upload")s.DrawTextClipped("Server: /"+Text(job,"remotePath"),100,790,2,Muted,1690);
    }
    private static string Size(double value)=>value>=1073741824?(value/1073741824).ToString("F1")+" GB":(value/1048576).ToString("F1")+" MB";
    private void Box(GlCanvas s,string title) { s.FillRoundedRect(0,0,1920,1080,0,Color.Black.WithAlpha(125));s.FillRoundedRect(292,195,1336,680,28,Color.Black.WithAlpha(45));FrostPanel(s,300,195,1320,665,24); s.DrawTextClipped(title,350,235,4,Color.White,1210); }
    private void FrostPanel(GlCanvas s,int x,int y,int width,int height,int radius) {
        s.FillRoundedRect(x,y,width,height,radius,Ink.WithAlpha(235));
        var game=(_visibleModal=="copy"||_visibleModal=="duplicate"||_visibleModal=="export") && _copyGame.ValueKind==JsonValueKind.Object?_copyGame:Games().ElementAtOrDefault(_selected);
        if(_covers.TryGetValue(Text(game,"id"),out var item)) {
            Surface poster=item is PngImage png?png.AsSurface():((JpegImage)item).AsSurface();
            s.BlitScaled(poster,x,y,width,height,true,radius,.65f,true);
        } else if(_fixedBackground!=null)s.BlitScaled(_fixedBackground.AsSurface(),x,y,width,height,true,radius,.65f,true);
        s.FillRoundedRect(x,y,width,height,radius,Color.FromRgb(23,33,49).WithAlpha(180));
        s.FillRoundedRect(x+radius,y+1,width-2*radius,1,0,Color.White.WithAlpha(35));
    }
    private void DrawCopy(GlCanvas s)
    {
        s.FillRoundedRect(0,0,1920,1080,0,Color.Black.WithAlpha(125));
        s.FillRoundedRect(218,186,1484,730,28,Blue.WithAlpha(28));
        FrostPanel(s,220,188,1480,726,26);
        s.FillRoundedRect(244,212,388,676,20,Panel.WithAlpha(220));
        var game=_copyGame.ValueKind==JsonValueKind.Object?_copyGame:Games().ElementAtOrDefault(_selected);
        DrawCover(s,game,272,300,332,332);
        s.DrawText("READY TO COPY",280,776,2,Blue);
        s.DrawTextClipped(Text(game,"titleId"),280,818,3,Muted,320);
        s.DrawText("COPY TO STORAGE",674,242,2,Blue);
        s.DrawTextClipped(Text(game,"title").Length>0?Text(game,"title"):_copyTitle,674,284,4,Color.White,974);
        MetadataTag(s,674,332,160,Text(game,"format").ToUpperInvariant(),TagFormat);
        MetadataTag(s,844,332,160,Size(Number(game,"size")),TagSize);
        MetadataTag(s,1014,332,170,"Min FW "+(Text(game,"minimumFirmware").Length>0?Text(game,"minimumFirmware"):"?"),TagFirmware);
        MetadataTag(s,1194,332,150,RegionLabel(game),TagRegion);
        MetadataTag(s,1354,332,180,Flag(game,"backportFiles")?"Backported":"Not Backported",Flag(game,"backportFiles")?TagBackport:TagNeutral);
        MetadataTag(s,674,372,470,InstallStatus(game),InstalledLocation(game).Length>0?TagInstalled:TagNeutral);
        MetadataTag(s,1154,372,490,"Server: "+Text(game,"sourceProtocol").ToUpperInvariant()+" · "+Text(game,"sourceName"),TagNeutral);
        s.DrawText("CHOOSE A DESTINATION",674,410,2,Muted);
        var drives=Array(Get(_data,"storage"));
        _drive=Math.Clamp(_drive,0,Math.Max(0,drives.Length-1));
        _driveY=Ease(_driveY,440+(_drive%6)*54);
        for(int i=(_drive/6)*6;i<drives.Length && i<(_drive/6)*6+6;i++)
            s.FillRoundedRect(664,440+(i%6)*54,990,48,10,Panel.WithAlpha(140));
        if(drives.Length>0)FocusRow(s,664,(int)_driveY,990,48);
        for(int i=(_drive/6)*6;i<drives.Length && i<(_drive/6)*6+6;i++) {
            int y=450+(i%6)*54;
            bool known=Flag(drives[i],"capacityKnown");
            s.DrawTextClipped(Text(drives[i],"label"),688,y,2,Color.White,580);
            s.DrawTextClipped(known?Size(Number(drives[i],"freeBytes"))+" FREE":"FREE SPACE UNKNOWN",1280,y,2,Muted,350);
        }
        if(drives.Length==0)s.DrawText("No writable storage found",688,460,3,Muted);
        if(drives.Length>0){
            var destination=CopyDestination(drives[_drive]);
            string path=destination.Root+(destination.Selected.Length>0?"/"+destination.Selected:"");
            s.DrawText(destination.Legacy?"□  Destination folder  ·  saved custom":"□  Destination folder  ·  cycle",680,775,2,Blue);
            s.DrawTextClipped(destination.Folders.Length>0?path:"No configured scan folder on this drive",680,807,2,Muted,945);
        }
        s.FillRoundedRect(666,842,986,48,12,Color.FromRgb(32,73,75));
        s.DrawText("×  Start copy",690,855,2,Color.White);
        s.DrawText("○  Back",1430,855,2,Muted);
    }
    private string _editSourceId="";
    private void EditSettings()
    {
        var sources=Array(Get(Smb,"sources"));
        if(sources.Length==0){_actionError="Add a server before editing it.";return;}
        var settings=sources[Math.Clamp(_sourceRow,0,sources.Length-1)];
        _editSourceId=Text(settings,"id");
        _values=_keys.Select(k=>k=="password"?"":Text(settings,k)).ToArray();
        _values[7]=_values[7] is "ftp" or "webdav" or "webdavs"?_values[7]:"smb";
        _remember=Flag(settings,"remember"); _passwordEdited=false; _field=0; _modal="settings";
    }
    private void DrawSettings(GlCanvas s)
    {
        Box(s,"EDIT SERVER");
        _fieldY=Ease(_fieldY,301+_field*38);
        FocusRow(s,335,(int)_fieldY,1240,40);
        for(int i=0;i<11;i++) {
            int y=308+i*38;
            string value=i<9?_labels[i]+": "+(i==5?(_passwordEdited?"(updated)":"(unchanged)"):_values[i]):i==9?"Remember password: "+(_remember?"YES":"NO"):"SAVE SERVER";
            s.DrawTextClipped(value,355,y,2,i==_field?Color.White:Muted,1190);
        }
        s.DrawText("×  Edit / Select     ○  Back",355,802,2,Blue);
    }
    private void SaveSettings()
    {
        var fields=new List<(string Name,object Value)>();
        for(int i=0;i<9;i++) if(i!=5 || _passwordEdited) fields.Add((_keys[i],_values[i]));
        fields.Add(("remember",_remember));fields.Add(("sourceId",_editSourceId)); Send("configure",fields.ToArray()); _modal="";
    }
    private void HandleExit(FrameContext c) {
        if(!c.Pressed(ScePadButton.Options)) return;
        try {_closingApp=NativeExit.FindOwnApp();_closeDeadline=Environment.TickCount64+15000;_backend.Stop();_status="Stopping transfers before closing...";}
        catch(Exception e){_status=e.Message;}
    }
    private ScePadButton _stickDirection;
    private long _stickRepeatAt;
    private ScePadButton StickNavigation(float x,float y,long now) {
        float strength=Math.Max(Math.Abs(x),Math.Abs(y));
        float threshold=_stickDirection==0?.55f:.35f;
        ScePadButton direction=strength<threshold?0:Math.Abs(x)>Math.Abs(y)
            ?(x<0?ScePadButton.Left:ScePadButton.Right)
            :(y<0?ScePadButton.Up:ScePadButton.Down);
        if(direction==0){_stickDirection=0;return 0;}
        if(direction!=_stickDirection){_stickDirection=direction;_stickRepeatAt=now+350;return direction;}
        if(now<_stickRepeatAt)return 0;
        _stickRepeatAt=now+130;return direction;
    }
    private void HandleInput(FrameContext c)
    {
        if(_modal.Length==0 && _visibleModal.Length>0)return;
        var axes=c.Input.LeftStick;
        var stick=StickNavigation(c.Input.IsConnected?axes.X:0,c.Input.IsConnected?axes.Y:0,Environment.TickCount64);
        bool Press(ScePadButton button)=>c.Pressed(button)||(stick!=0 && stick==button);
        if(Press(ScePadButton.Circle)) { _modal=_modal=="duplicate"?"copy":""; return; }
        if(_modal=="duplicate") {if(Press(ScePadButton.Cross))StartCopy(true);return;}
        if(_modal=="export"){
            var servers=ExportServers();var locations=ExportLocations();
            if(Press(ScePadButton.Up))_exportServer=Math.Max(0,_exportServer-1);
            if(Press(ScePadButton.Down))_exportServer=Math.Min(Math.Max(0,servers.Length-1),_exportServer+1);
            if(Press(ScePadButton.Square)&&locations.Length>0)_exportLocation=(_exportLocation+1)%locations.Length;
            if(Press(ScePadButton.Triangle))_exportSkipVerification=!_exportSkipVerification;
            if(Press(ScePadButton.Cross)&&servers.Length>0&&locations.Length>0){
                _exportServer=Math.Clamp(_exportServer,0,servers.Length-1);_exportLocation=Math.Clamp(_exportLocation,0,locations.Length-1);
                Send("export",("localId",Text(locations[_exportLocation],"localId")),("sourceId",Text(servers[_exportServer],"id")),("confirmed",true),("skipVerification",_exportSkipVerification));_modal="";SelectTab(3);
            }return;
        }
        if(_modal=="deleteGame") {
            if(Press(ScePadButton.Cross)){
                _deleteRequest="delete-"+Environment.TickCount64.ToString()+"-"+(++_deleteSerial).ToString();
                _deletePreviousJob=Number(Get(Get(Smb,"installed"),"storageJob"),"id");
                Send("deleteInstalled",("titleId",Text(_deleteGame,"titleId")),("sourceKey",Text(_deleteGame,"sourceKey")),("requestId",_deleteRequest),("confirmed",true));
                _modal="";
            }return;
        }
        if(_modal=="deleteSource") {
            if(Press(ScePadButton.Cross)) {Send("deleteSource",("sourceId",_deleteSourceId));_modal="";}return;
        }
        if(_modal=="duplicateSource") {
            if(Press(ScePadButton.Cross)){Send("duplicateSource",("sourceId",_deleteSourceId),("confirmed",true));_modal="";}return;
        }
        if(_modal=="cancel") { if(Press(ScePadButton.Cross)) {Send("cancel");_modal="";} return; }
        if(_modal=="copy") {
            var drives=Array(Get(_data,"storage"));
            if(Press(ScePadButton.Up)) _drive=Math.Max(0,_drive-1);
            if(Press(ScePadButton.Down)) _drive=Math.Min(Math.Max(0,drives.Length-1),_drive+1);
            if(Press(ScePadButton.Square)&&drives.Length>0){var d=CopyDestination(drives[_drive]);if(d.Folders.Length>0)_copyFolders[d.Root]=d.Folders[(System.Array.IndexOf(d.Folders,d.Selected)+1)%d.Folders.Length];}
            if(Press(ScePadButton.Cross) && drives.Length>0) {if(InstalledLocation(_copyGame).Length>0)_modal="duplicate";else StartCopy(false);} return;
        }
        if(_modal=="settings") {
            if(Press(ScePadButton.Up)) _field=Math.Max(0,_field-1);
            if(Press(ScePadButton.Down)) _field=Math.Min(10,_field+1);
            if(Press(ScePadButton.Cross)) {
                if(_field==10) SaveSettings(); else if(_field==9) _remember=!_remember;
                else if(_field==7){_values[7]=_values[7] switch {"smb"=>"ftp","ftp"=>"webdav","webdav"=>"webdavs",_=>"smb"};_values[8]="";}
                else try {_actionError="";_input=TextInputDialog.Open(_labels[_field],maxLength:240,type:_field==5?ImeType.BasicLatin:ImeType.Default,initialText:_values[_field],options:_field==5?ImeOption.Password:ImeOption.None);}catch(Exception e){_actionError=_status=e.Message;}
            } return;
        }
        HandleExit(c);
        if(Press(ScePadButton.L1)) SelectTab((_tab+3)%4);
        if(Press(ScePadButton.R1)) SelectTab((_tab+1)%4);
        if(_tab<2) {
            var games=Games();
            if(Press(ScePadButton.Left)) _selected--;
            if(Press(ScePadButton.Right)) _selected++;
            if(Press(ScePadButton.Up)) _selected-=3;
            if(Press(ScePadButton.Down)) _selected+=3;
            _selected=Math.Clamp(_selected,0,Math.Max(0,games.Length-1));
            if(Press(ScePadButton.Square)) RefreshLibrary();
            if(Press(ScePadButton.Triangle)) {_sort=1-_sort;SortGames();_selected=0;}
            if(!Flag(Smb,"busy") && !_refreshPending && Press(ScePadButton.L2)) {
                SelectTab(2);
            }
            if(Press(ScePadButton.R2)&&games.Length>0 && InstalledLocation(games[_selected]).Length>0){
                var target=DeleteTarget(games[_selected]);
                if(Flag(Smb,"busy"))_actionError="Wait for the current transfer or scan to finish.";
                else if(target.ValueKind!=JsonValueKind.Object)_actionError="This installation cannot be deleted through ShadowMount. Refresh and try again.";
                else {_deleteGame=target;_deleteTitle=Text(games[_selected],"title");_modal="deleteGame";}
            }
            if(Press(ScePadButton.R3)&&games.Length>0&&InstalledLocation(games[_selected]).Length>0)OpenExport(games[_selected]);
            if(Press(ScePadButton.Cross)&&games.Length>0) {if(Flag(games[_selected],"localOnly"))OpenExport(games[_selected]);else{_copyGame=games[_selected];_copyId=Text(_copyGame,"id");_copyTitle=Text(games[_selected],"title");_drive=0;_modal="copy";}}
        } else if(_tab==2) {
            var sources=Array(Get(Smb,"sources"));
            if(Press(ScePadButton.Up)) _sourceRow=Math.Max(0,_sourceRow-1);
            if(Press(ScePadButton.Down)) _sourceRow=Math.Min(Math.Max(0,sources.Length-1),_sourceRow+1);
            if(Press(ScePadButton.Cross)&&sources.Length>0) Send(Flag(sources[_sourceRow],"enabled")?"deactivateSource":"selectSource",("sourceId",Text(sources[_sourceRow],"id")));
            if(Press(ScePadButton.Square)) Send("addSource");
            if(Press(ScePadButton.Triangle)) EditSettings();
            if((Press(ScePadButton.R2)||Press(ScePadButton.L2))&&sources.Length>0) {
                if(Flag(Smb,"busy")){_status="Wait for the server operation to finish before removing a source.";return;}
                var source=sources[Math.Clamp(_sourceRow,0,sources.Length-1)];
                _deleteSourceId=Text(source,"id");_deleteSourceName=Text(source,"name");
                if(_deleteSourceName.Length==0)_deleteSourceName=Text(source,"server")+" / "+Text(source,"share");
                _modal=Press(ScePadButton.L2)?"duplicateSource":"deleteSource";
            }
        } else {
            if(Press(ScePadButton.Cross)&&Text(Get(Smb,"job"),"direction")!="upload") Send(Text(Get(Smb,"job"),"status")=="copying"?"pause":"resume");
            if(Press(ScePadButton.Square)) _modal="cancel";
        }
    }
    private void ClearCovers() {_coverGeneration++;_coverPending.Clear();_canvas.ClearImages();foreach(var image in _covers.Values) image.Dispose();_covers.Clear();_coverUse.Clear();_coverBytes=0;_coverFailures.Clear();}
    protected override void OnUnload() {
        // Do not unload decoder modules underneath outstanding image workers.
        lock(_coverLoadLock){_coverStopped=true;_coverActive-=_coverRequests.Count;_coverRequests.Clear();Monitor.PulseAll(_coverLoadLock);while(_coverResults.Count>0)_coverResults.Dequeue().Image?.Dispose();}
        _backend.Stop();_input?.Dispose();ClearCovers();_fixedBackground?.Dispose();_fixedBackground=null;lock(_coverLoadLock){if(_coverActive==0){_pngModule?.Dispose();_jpegModule?.Dispose();}}}
}
internal static class JsonValueExtensions { public static double TryGetDoubleSafe(this JsonElement value)=>value.ValueKind==JsonValueKind.Number&&value.TryGetDouble(out double n)?n:0; }
internal static class Program { private static void Main() {using(var app=new Launcher()) app.Run();ProcessExit.Exit();} }
