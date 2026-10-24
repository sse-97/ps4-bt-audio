#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include "ui.h"

/* Bluetooth Audio 2.8.0 home-screen app. It renders the screens in ui.h and
 * drives the audio service entirely through the JSON control API (docs/API.md):
 * GET /api/status for state, POST /api/... for actions. It never talks to the
 * Bluetooth driver itself. */

/* Enter-button assignment (console region setting): 0 = Circle confirms (Japan
 * and parts of Asia), 1 = Cross confirms. Declared here because not every SDK
 * header exposes it; the symbol lives in libSceSystemService. */
#define ENTER_BUTTON_PARAM 1000
extern int sceSystemServiceParamGetInt(int paramId, int *value);

static int pad=-1,pad_connected;
static uint64_t pad_retry;
static struct ui_ctx ctx;
static char pin_buf[5]="0000";
static int online;

/* All file I/O uses sceKernel* (never stdio): the app runs in a restricted
 * sandbox where a blocked libc syscall raises SIGSYS. app.log confirms these
 * kernel calls succeed. Each line carries the UTC time of day, like service.log,
 * so the two line up. */
#define APP_LOG "/data/bta_audio/app.log"
static void logline(const char*s){
 OrbisKernelTimeval tv;unsigned t=0;
 if(!sceKernelGettimeofday(&tv)&&tv.tv_sec>0)t=(unsigned)(tv.tv_sec%86400);
 char line[256];int n=snprintf(line,sizeof(line),"[%02u:%02u:%02u] %s\n",t/3600,t/60%60,t%60,s);
 if(n<0)return;if(n>=(int)sizeof(line)){n=sizeof(line)-1;line[n-1]='\n';}
 int fd=sceKernelOpen(APP_LOG,0x209,0644);
 if(fd>=0){sceKernelWrite(fd,line,(size_t)n);sceKernelClose(fd);}
}
/* A few lines per launch: past 64 KB the log becomes app-previous.log, so the two
 * files never hold more than about 128 KB. */
static void rotate_log(void){
 int fd=sceKernelOpen(APP_LOG,0,0);if(fd<0)return;
 off_t size=sceKernelLseek(fd,0,2);sceKernelClose(fd);
 if(size>65536)sceKernelRename(APP_LOG,"/data/bta_audio/app-previous.log");
}
static unsigned controller(uint64_t now){
 OrbisPadData data;memset(&data,0,sizeof(data));
 if(pad>=0){int rc=scePadReadState(pad,&data);if(rc>=0&&data.connected){pad_connected=1;return data.buttons;}pad_connected=0;}
 if(now<pad_retry)return 0;pad_retry=now+2000000;
 if(pad>=0){scePadClose(pad);pad=-1;}
 OrbisUserServiceLoginUserIdList users;for(int i=0;i<4;i++)users.userId[i]=-1;
 int rc=sceUserServiceGetLoginUserIdList(&users);
 if(rc<0)sceUserServiceGetInitialUser(&users.userId[0]);
 for(int i=0;i<4;i++)if(users.userId[i]>=0){
  int h=scePadGetHandle(users.userId[i],ORBIS_PAD_PORT_TYPE_STANDARD,0);
  if(h<0)h=scePadOpen(users.userId[i],ORBIS_PAD_PORT_TYPE_STANDARD,0,0);
  if(h<0)continue;memset(&data,0,sizeof(data));
  if(scePadReadState(h,&data)>=0&&data.connected){pad=h;pad_connected=1;logline("Controller acquired");return data.buttons;}
  scePadClose(h);
 }
 return 0;
}

/* ---- HTTP client: the audio service owns Bluetooth; this app only displays and requests ---- */
static int local_socket(int port){
 int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
 struct timeval t={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));
 int one=1;setsockopt(fd,SOL_SOCKET,0x800,&one,sizeof(one));
 struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_len=sizeof(a);a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(0x7f000001);
 if(connect(fd,(void*)&a,sizeof(a))<0){close(fd);return -1;}return fd;
}
static int write_all(int fd,const void*p,size_t n){const char*b=p;while(n){ssize_t k=send(fd,b,n,0);if(k<=0)return -1;b+=k;n-=k;}return 0;}
/* Returns the HTTP status code (e.g. 200, 409) or -1 on transport error. Copies
 * the response body (NUL-terminated) into out when provided. Every path closes fd. */
static int http(const char*method,const char*path,void*out,size_t cap){
 int fd=local_socket(8097);if(fd<0)return -1;
 char request[256];int len=snprintf(request,sizeof(request),"%s %s HTTP/1.1\r\nHost: 127.0.0.1:8097\r\nX-BTA-Control: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",method,path);
 if(len<0||len>=(int)sizeof(request)||write_all(fd,request,(size_t)len)){close(fd);return -1;}
 char response[16384];size_t used=0;
 while(used+1<sizeof(response)){ssize_t n=recv(fd,response+used,sizeof(response)-used-1,0);if(n==0)break;if(n<0){close(fd);return -1;}used+=(size_t)n;}
 close(fd);response[used]=0;
 if(used<12||strncmp(response,"HTTP/1.1 ",9))return -1;
 int code=atoi(response+9);
 char*b=strstr(response,"\r\n\r\n");
 if(out&&cap){size_t bytes=0;if(b){b+=4;bytes=used-(size_t)(b-response);if(bytes>cap-1)bytes=cap-1;memcpy(out,b,bytes);}((char*)out)[bytes]=0;}
 return code>0?code:-1;
}
/* GET /api/status and parse the JSON into `state` (model.h). Sets `online`. */
static char status_buf[16384];
static int refresh(void){
 int fd=local_socket(8097);if(fd<0){online=0;return -1;}
 const char*req="GET /api/status HTTP/1.1\r\nHost: 127.0.0.1:8097\r\nX-BTA-Control: 1\r\nConnection: close\r\n\r\n";
 if(write_all(fd,req,strlen(req))){close(fd);online=0;return -1;}
 size_t used=0;
 while(used+1<sizeof(status_buf)){ssize_t n=recv(fd,status_buf+used,sizeof(status_buf)-used-1,0);if(n==0)break;if(n<0){close(fd);online=0;return -1;}used+=(size_t)n;}
 close(fd);status_buf[used]=0;
 if(used<12||strncmp(status_buf,"HTTP/1.1 200",12)){online=0;return -1;}
 char*body=strstr(status_buf,"\r\n\r\n");if(!body){online=0;return -1;}body+=4;
 struct ui_state next;
 if(!model_parse(body,&next)){online=0;return -1;}
 state=next;online=1;return 0;
}
static void notice_for(const char*s,uint64_t us){snprintf(notice,sizeof(notice),"%s",s);notice_until=g_now+us;}
static void set_notice(const char*s){notice_for(s,6000000);}
/* Why the service didn't start stays in the Start dialog until the next try or
 * until the service answers (the main loop clears it then). `starting` puts the
 * busy indicator beside "Starting audio service..." meanwhile. */
static int starting;
static void start_notice(const char*s){notice_for(s,600000000);starting=0;}
/* The audio service normally starts at boot (GoldHEN AutoRun). If it is not running
 * when the app opens, send the bundled payload to GoldHEN's local BinLoader (port 9090)
 * once. The payload's launcher refuses to start twice, and we only try while port 8097
 * is not answering. The payload goes out on a single connection: never probe 9090. */
static int launch_attempted;
static void start_service(void){
 if(online||launch_attempted)return;launch_attempted=1;
 static char payload[131072];
 int f=sceKernelOpen("/app0/assets/BTA-Audio.bin",0,0);
 if(f<0){logline("start: bundled payload not found");start_notice("The audio service file is missing. Reinstall the app.");return;}
 size_t used=0;
 while(used<sizeof(payload)){long n=sceKernelRead(f,payload+used,sizeof(payload)-used);if(n<=0)break;used+=(size_t)n;}
 sceKernelClose(f);
 if(used<1024||used>=sizeof(payload)){logline("start: bundled payload size invalid");start_notice("The audio service file is damaged. Reinstall the app.");return;}
 int fd=local_socket(9090);
 if(fd<0){logline("start: PayLoader (9090) not reachable; turn it on in GoldHEN's Servers Settings");start_notice("Turn on GoldHEN's PayLoader Server, then press Start.");return;}
 int rc=write_all(fd,payload,used);close(fd);
 logline(rc?"start: send failed":"start: payload sent to BinLoader");
 if(rc)start_notice("Couldn't reach GoldHEN. Press Start to try again.");
 else{notice_for("Starting audio service...",30000000);starting=1;}
}
/* POST an action, then refresh. 200 clears the notice; an error shows the
 * service's plain-text reason (error bodies are text, success bodies are JSON). */
static void action(const char*path){
 char body[160];int code=http("POST",path,body,sizeof(body));
 if(code==200)notice[0]=0;
 else if(code<0)set_notice("No response from the audio service.");
 else{for(int i=0;body[i];i++)if(body[i]=='\n'||body[i]=='\r')body[i]=0;set_notice(body[0]?body:"That didn't work.");}
 if(code!=200){ /* the PIN is cut from its path */
  char m[224];snprintf(m,sizeof(m),"action failed: %s -> %d %s",strncmp(path,"/api/pin/",9)?path:"/api/pin",code,code<0?"":body);logline(m);
 }
 refresh();
}
/* Connect to state.devices[i]. */
static void select_index(int i){
 if(i<0||i>=state.count)return;
 char path[64];snprintf(path,sizeof(path),"/api/select/%012lX",state.devices[i].address);action(path);
}
static void volume_change(int amount){
 if(!online)return;int value=state.volume+amount;if(value<0)value=0;if(value>100)value=100;
 char path[40];snprintf(path,sizeof(path),"/api/volume/%d",value);action(path);
}

/* ---- focus helpers: lists stop at their ends, like the system's ---- */
static void scroll_to(int visible){
 if(ctx.focus<ctx.top)ctx.top=ctx.focus;if(ctx.focus>=ctx.top+visible)ctx.top=ctx.focus-visible+1;if(ctx.top<0)ctx.top=0;
}
static void clamp_home_focus(void){
 int n=home_rows(&ctx),last=n-HOME_VISIBLE;if(ctx.focus<0)ctx.focus=0;if(ctx.focus>=n)ctx.focus=n-1;
 scroll_to(HOME_VISIBLE);if(last<0)last=0;if(ctx.top>last)ctx.top=last;
}
/* Home index of one of the fixed rows (ROW_*), below however many devices Home lists. */
static int fixed_row(int row){return home_count(&ctx)+row;}
static void clamp_scan_focus(void){
 int n=state.count;if(n<=0){ctx.focus=0;ctx.top=0;return;}
 if(ctx.focus<0)ctx.focus=0;if(ctx.focus>=n)ctx.focus=n-1;scroll_to(SCAN_VISIBLE);
}
static void go_home(int row){ctx.screen=SCREEN_HOME;ctx.menu=0;ctx.focus=row;clamp_home_focus();}
/* Home row of the device the service has selected (the Add Device row if none). */
static int selected_row(void){
 int n=home_count(&ctx);
 for(int k=0;k<n;k++)if(state.devices[home_dev(&ctx,k)].address==state.selected)return k;
 return fixed_row(ROW_ADD);
}
static void step(int*value,unsigned pressed,int count){
 if(pressed&ORBIS_PAD_BUTTON_DOWN)(*value)++;if(pressed&ORBIS_PAD_BUTTON_UP)(*value)--;
 if(*value<0)*value=0;if(*value>=count)*value=count-1;
}

static unsigned char snaprow[WIDTH*3];
static void snapshot(void){
 int fd=sceKernelOpen("/data/bta_audio/app-frame.ppm",0x601,0644);
 if(fd<0){logline("snapshot: open failed");return;}
 char hdr[32];int hl=snprintf(hdr,sizeof(hdr),"P6\n%d %d\n255\n",WIDTH,HEIGHT);sceKernelWrite(fd,hdr,(size_t)hl);
 for(int y=0;y<HEIGHT;y++){
  for(int x=0;x<WIDTH;x++){uint32_t v=back[y*WIDTH+x];snaprow[x*3]=(unsigned char)(v>>16);snaprow[x*3+1]=(unsigned char)(v>>8);snaprow[x*3+2]=(unsigned char)v;}
  sceKernelWrite(fd,snaprow,sizeof(snaprow));
 }
 sceKernelClose(fd);logline("snapshot: written");
}

/* The system's view of this app, from sceSystemServiceGetStatus. OpenOrbis v0.5.4
 * declares that call without a prototype; this is the layout of the SDK names the
 * GPCS4, fpPS4 and shadPS4 headers agree on. */
typedef struct { int32_t eventNum; uint8_t overlaid,background,cpu7,live_streaming,out_of_vr_area; uint8_t reserved[125]; } SystemStatus;
#define PAD_INTERCEPTED 0x80000000u /* SCE_PAD_BUTTON_INTERCEPTED: the system UI took this input */
/* True while the app shouldn't act on the controller: the system UI is on top, another
 * app is in front, the pad data is marked intercepted, or the app was just resumed
 * (the wall clock jumped while this process stood still). A status flag only counts
 * once it has been seen clear, so one that is stuck at 1 can't lock the app out.
 * Each change goes to app.log, which shows what the console actually reports. */
static int system_away(unsigned buttons,int*behind){
 static uint64_t wall_last;static int seen_front,last=-1;
 SystemStatus st;memset(&st,0,sizeof(st));
 int32_t(*get)(SystemStatus*)=(int32_t(*)(SystemStatus*))(void*)sceSystemServiceGetStatus;
 int rc=get(&st),ok=rc>=0;
 if(ok&&!st.overlaid&&!st.background)seen_front=1;
 int over=ok&&seen_front&&st.overlaid,bg=ok&&seen_front&&st.background,icpt=(buttons&PAD_INTERCEPTED)!=0;
 OrbisKernelTimeval tv;uint64_t wall=0;
 if(!sceKernelGettimeofday(&tv)&&tv.tv_sec>0)wall=(uint64_t)tv.tv_sec*1000000+(uint64_t)tv.tv_usec;
 int resumed=wall_last&&wall>wall_last+1000000;wall_last=wall;
 int code=over|bg<<1|icpt<<2;
 if(code!=last||resumed){
  char m[112];snprintf(m,sizeof(m),"focus: overlaid=%d background=%d intercepted=%d%s (status rc=0x%x raw=%d/%d)",over,bg,icpt,resumed?", resumed":"",rc,st.overlaid,st.background);
  logline(m);last=code;
 }
 *behind=bg;
 return over||bg||icpt||resumed;
}

/* The output mode the app asks for: none in particular. Left unset, it gets the
 * system's default picture, so opening it over an HDR game switched the TV out of
 * HDR and back (KLog: set_mode RGB2020_PQ -> CERGB, Hdmi Init, seconds of black
 * screen each way). "Any" may let the console keep the game's mode and convert
 * this picture, as it does for the home screen. OpenOrbis v0.5.4 declares both
 * calls without prototypes; the mode layout is the one shadPS4 uses. */
typedef struct { uint32_t size; uint8_t encoding,range,colorimetry,depth; uint64_t refresh_rate,resolution; uint8_t reserved[8]; } VideoOutMode;
static void any_output_mode(int video){
 VideoOutMode mode;memset(&mode,0,sizeof(mode));
 ((void(*)(VideoOutMode*,uint32_t))(void*)sceVideoOutModeSetAny_)(&mode,sizeof(mode));
 int32_t(*configure)(int32_t,uint32_t,const VideoOutMode*,const void*,uint32_t,uint32_t)=
  (int32_t(*)(int32_t,uint32_t,const VideoOutMode*,const void*,uint32_t,uint32_t))(void*)sceVideoOutConfigureOutputMode_;
 int rc=configure(video,0,&mode,NULL,sizeof(mode),0);
 char m[64];snprintf(m,sizeof(m),"Video output mode: any, rc=0x%x",rc);logline(m);
}

/* Back on the app's main screen points to the PS button, which leaves the app open.
 * Not sceSystemServiceNavigateToGoHome: on 13.52 that crashed the system's home
 * screen (ShellUI) each time, an unhandled 0x80960009 while switching the user. */
static void to_home_screen(void){
 set_notice("Press the PS button to go to the home screen.");
}

/* Returning from main ends in a raw exit system call, which the app sandbox
 * refuses with SIGSYS: the system then reports a crash (CE-34878-0). Ask the
 * system to close the app instead, as other homebrew apps (game or mini app) do,
 * and wait for it. Should that call fail, keep waiting rather than return: the
 * app can still be closed from the PS button's Quick Menu. */
static int close_app(void){
 int rc=sceSystemServiceLoadExec("exit",NULL);
 char m[48];snprintf(m,sizeof(m),"exit: system close rc=0x%x",rc);logline(m);
 for(;;)sceKernelUsleep(100000);
 return 0;
}

int main(void){
 sceKernelMkdir("/data/bta_audio",0777);sceKernelChmod("/data/bta_audio",0777);
 rotate_log();logline("Bluetooth Audio 2.8.0 native app starting (times UTC)");
 /* Return codes go to app.log: a mini app (CATEGORY gde) shares the console with a
  * running game, and these are the calls that would show it. */
 char diag[128];
 int video=sceVideoOutOpen(0xff,0,0,0);
 if(video<0){snprintf(diag,sizeof(diag),"VideoOutOpen failed rc=0x%x",video);logline(diag);return close_app();}
 any_output_mode(video);
 off_t memory;size_t fbbytes=(size_t)WIDTH*HEIGHT*4;size_t size=(2*fbbytes+0x1FFFFF)&~(size_t)0x1FFFFF;void*base=0;
 size_t dmem=sceKernelGetDirectMemorySize();
 int arc=sceKernelAllocateDirectMemory(0,dmem,size,0x200000,3,&memory),mrc=arc<0?0:sceKernelMapDirectMemory(&base,size,0x33,0,memory,0x200000);
 snprintf(diag,sizeof(diag),"Video memory: %lu MB direct, alloc=0x%x map=0x%x",(unsigned long)(dmem>>20),arc,mrc);logline(diag);
 if(arc<0||mrc<0)return close_app();
 void*buffers[2]={base,(char*)base+fbbytes};OrbisVideoOutBufferAttribute attr;
 sceVideoOutSetBufferAttribute(&attr,0x80000000,1,0,WIDTH,HEIGHT,WIDTH);
 int brc=sceVideoOutRegisterBuffers(video,0,buffers,2,&attr);
 if(brc<0){snprintf(diag,sizeof(diag),"Register buffers failed rc=0x%x",brc);logline(diag);return close_app();}
 sceVideoOutSetFlipRate(video,1);
 OrbisUserServiceInitializeParams userparams={ORBIS_KERNEL_PRIO_FIFO_LOWEST};
 int user_rc=sceUserServiceInitialize(&userparams);int pad_rc=scePadInit();
 snprintf(diag,sizeof(diag),"User init=0x%x Pad init=0x%x",user_rc,pad_rc);logline(diag);
 /* Region enter-button setting: confirm with Circle where the console does. */
 int enter=1;if(sceSystemServiceParamGetInt(ENTER_BUTTON_PARAM,&enter)<0)enter=1;ctx.confirm_circle=(enter==0);
 ctx.pin=pin_buf;
 sceSystemServiceHideSplashScreen();
 g_now=sceKernelGetProcessTime();
 logline("Checking audio service");refresh();start_service();
 logline("Starting display loop");
 unsigned previous=0;int buffer=0,armed=0;long flip=0;uint64_t poll=0,repeat=0,snap_cd=0,start_at=0;
 int pinignore=0,scan_connecting=0,held=0;unsigned long scan_addr=0;
 for(;;){
  uint64_t now=sceKernelGetProcessTime();g_now=now;
  int l=online?(state.link>=LINK_IDLE&&state.link<=LINK_SEARCHING?state.link:LINK_IDLE):LINK_OFFLINE;
  if(now>=poll){
   /* Keep focus on the same Home row across a refresh: the same device, or the
    * same fixed row when the device list above it grows or shrinks. */
   int home=ctx.screen==SCREEN_HOME,nd=home_count(&ctx),on_device=home&&ctx.focus>=0&&ctx.focus<nd,fixed=ctx.focus-nd;
   unsigned long faddr=on_device?state.devices[home_dev(&ctx,ctx.focus)].address:0;
   refresh();poll=now+(!online?2000000:(l==LINK_STREAMING?300000:500000));
   l=online?(state.link>=LINK_IDLE&&state.link<=LINK_SEARCHING?state.link:LINK_IDLE):LINK_OFFLINE;
   if(on_device){int n=home_count(&ctx);for(int k=0;k<n;k++)if(state.devices[home_dev(&ctx,k)].address==faddr){ctx.focus=k;break;}}
   else if(home)ctx.focus=fixed_row(fixed);
  }
  /* Start shows "Starting audio service..." for a moment before it tries, so a
   * try that fails at once still visibly happened before its reason returns. */
  if(start_at&&now>=start_at){start_at=0;launch_attempted=0;refresh();start_service();}
  /* The Start dialog opens on its Start button each time the service goes away;
   * once the service answers, the dialog's start message has done its job. */
  if(ctx.online&&!online)ctx.dialog_focus=0;
  if(!ctx.online&&online){notice[0]=0;starting=0;}
  ctx.online=online;
  ctx.busy=starting;
  /* Keep the cursor valid even when the device list changes between polls. */
  if(ctx.screen==SCREEN_HOME)clamp_home_focus();
  else if(ctx.screen==SCREEN_SCAN)clamp_scan_focus();
  /* The Options menu belongs to the connected device; it closes when that link ends. */
  if(ctx.menu&&!focus_is_active(&ctx))ctx.menu=0;
  /* The connect-and-wait only lives while we're on the search screen. */
  if(ctx.screen!=SCREEN_SCAN)scan_connecting=0;
  /* Move on to the test tone once the device the user chose here is actually
   * playing (not some other device already streaming). */
  if(scan_connecting&&l==LINK_STREAMING&&scan_addr&&state.selected==scan_addr){
   ctx.screen=SCREEN_TEST;ctx.focus=TEST_YES;scan_connecting=0;logline("action: test (add)");action("/api/test");
  }
  /* PIN prompt rises and falls with the service flag. */
  if(!state.pin_pending)pinignore=0;if(state.pin_pending&&!ctx.pinmode&&!pinignore){ctx.pinmode=1;ctx.digit=0;memcpy(pin_buf,"0000",5);}

  unsigned buttons=controller(now);
  if(!armed&&pad_connected){previous=buttons;armed=1;}
  unsigned pressed=buttons&~previous;
  if(buttons==previous&&now>=repeat){pressed=buttons&(ORBIS_PAD_BUTTON_UP|ORBIS_PAD_BUTTON_DOWN|ORBIS_PAD_BUTTON_LEFT|ORBIS_PAD_BUTTON_RIGHT);repeat=now+150000;}
  else if(buttons!=previous)repeat=now+450000;
  previous=buttons;
  if(!armed)pressed=0;
  /* While the system's UI covers the app or another app is in front, the buttons
   * aren't meant for it; and the press that brings it back (X on its tile or in the
   * Quick Menu) may still be held. So no input then, and afterwards none until every
   * button has been let go. */
  int behind=0,away=system_away(buttons,&behind);
  if(away)held=1;
  if(held){pressed=0;if(!away&&!(buttons&~PAD_INTERCEPTED))held=0;}

  unsigned CONFIRM=ctx.confirm_circle?ORBIS_PAD_BUTTON_CIRCLE:ORBIS_PAD_BUTTON_CROSS;
  unsigned CANCEL =ctx.confirm_circle?ORBIS_PAD_BUTTON_CROSS :ORBIS_PAD_BUTTON_CIRCLE;
  int d=ctx.digit;

  if(ctx.pinmode){
   if(pressed&ORBIS_PAD_BUTTON_LEFT)ctx.digit=(d+3)%4;if(pressed&ORBIS_PAD_BUTTON_RIGHT)ctx.digit=(d+1)%4;
   if(pressed&ORBIS_PAD_BUTTON_UP)pin_buf[d]=(char)('0'+(pin_buf[d]-'0'+1)%10);if(pressed&ORBIS_PAD_BUTTON_DOWN)pin_buf[d]=(char)('0'+(pin_buf[d]-'0'+9)%10);
   if(pressed&CANCEL){ctx.pinmode=0;pinignore=1;}
   if(pressed&CONFIRM){char path[40];snprintf(path,sizeof(path),"/api/pin/%s",pin_buf);logline("action: pin");action(path);ctx.pinmode=0;pinignore=1;}
  }else if(!online){ /* "The audio service isn't running." Start / Close */
   if(pressed&ORBIS_PAD_BUTTON_LEFT)ctx.dialog_focus=0;if(pressed&ORBIS_PAD_BUTTON_RIGHT)ctx.dialog_focus=1;
   if(pressed&CONFIRM){
    if(ctx.dialog_focus)break;
    /* Not again while a start is under way: a second payload would only report
     * that the service is already running. */
    if(!start_at&&!(starting&&notice[0])){logline("action: start service");start_at=now+800000;starting=1;notice_for("Starting audio service...",30000000);}
   }
   if(pressed&CANCEL){logline("action: home screen");to_home_screen();} /* Close closes; Back never does */
  }else if(ctx.dialog==DLG_STOP){ /* "Stop the audio service?" Stop / Cancel */
   if(pressed&ORBIS_PAD_BUTTON_LEFT)ctx.dialog_focus=0;if(pressed&ORBIS_PAD_BUTTON_RIGHT)ctx.dialog_focus=1;
   if(pressed&CONFIRM){ctx.dialog=DLG_NONE;if(!ctx.dialog_focus){logline("action: quit");action("/api/quit");}}
   if(pressed&CANCEL)ctx.dialog=DLG_NONE;
  }else if(ctx.menu){
   step(&ctx.menu_focus,pressed,MENU_COUNT);
   if(pressed&CONFIRM){
    ctx.menu=0;
    if(ctx.menu_focus==MENU_TEST){logline("action: test");action("/api/test");}
    else{logline("action: disconnect");action("/api/stop");}
   }
   if(pressed&(CANCEL|ORBIS_PAD_BUTTON_OPTIONS))ctx.menu=0;
  }else if(ctx.screen==SCREEN_SCAN){
   step(&ctx.focus,pressed,state.count);clamp_scan_focus();
   if(pressed&CONFIRM){if(state.count>0&&ctx.focus>=0&&ctx.focus<state.count){scan_addr=state.devices[ctx.focus].address;scan_connecting=1;logline("action: select (add)");select_index(ctx.focus);}}
   if(pressed&ORBIS_PAD_BUTTON_TRIANGLE&&!state.scanning&&l!=LINK_CONNECTING){logline("action: scan again");action("/api/scan");}
   /* Leaving tells the service, which then reconnects the device Add Device paused. */
   if(pressed&CANCEL){logline("action: leave add");action("/api/scan-done");go_home(fixed_row(ROW_ADD));}
  }else if(ctx.screen==SCREEN_TEST){
   step(&ctx.focus,pressed,TEST_COUNT);
   if(pressed&CONFIRM){
    if(ctx.focus==TEST_AGAIN){logline("action: test again");action("/api/test");}
    else if(ctx.focus==TEST_DISCONNECT){logline("action: disconnect (add)");action("/api/stop");go_home(fixed_row(ROW_ADD));}
    else go_home(selected_row());
   }
   if(pressed&CANCEL)go_home(selected_row());
  }else if(ctx.screen==SCREEN_LATENCY){
   step(&ctx.focus,pressed,3);
   if(pressed&CONFIRM){char path[32];snprintf(path,sizeof(path),"/api/latency/%d",ctx.focus);logline("action: latency");action(path);go_home(fixed_row(ROW_LATENCY));}
   if(pressed&CANCEL)go_home(fixed_row(ROW_LATENCY));
  }else{ /* SCREEN_HOME */
   step(&ctx.focus,pressed,home_rows(&ctx));clamp_home_focus();
   int row=ctx.focus-home_count(&ctx);
   if(row==ROW_VOLUME){if(pressed&ORBIS_PAD_BUTTON_LEFT)volume_change(-5);if(pressed&ORBIS_PAD_BUTTON_RIGHT)volume_change(5);}
   if(pressed&ORBIS_PAD_BUTTON_OPTIONS&&focus_is_active(&ctx)){ctx.menu=1;ctx.menu_focus=MENU_TEST;}
   if(pressed&CONFIRM){
    if(row<0){
     /* The connected device opens its Options menu; any other device connects. */
     if(focus_is_active(&ctx)){ctx.menu=1;ctx.menu_focus=MENU_TEST;}
     else{logline("action: connect");select_index(home_dev(&ctx,ctx.focus));}
    }else if(row==ROW_ADD){ctx.screen=SCREEN_SCAN;ctx.focus=0;ctx.top=0;logline("action: scan (add)");action("/api/scan");}
    else if(row==ROW_TV){logline("action: tv");action(state.tv?"/api/tv-off":"/api/tv-on");}
    else if(row==ROW_LATENCY){ctx.screen=SCREEN_LATENCY;ctx.focus=lat_mode();}
    else if(row==ROW_PHONE){logline("action: phone control");action(state.lan?"/api/lan-off":"/api/lan-on");}
    else if(row==ROW_STOP){ctx.dialog=DLG_STOP;ctx.dialog_focus=1;}
   }
   /* Back goes to the home screen and the app stays open, like the system's own
    * apps, so returning to it doesn't start it over. It closes from the Quick Menu
    * (Close Application). */
   if(pressed&CANCEL){logline("action: home screen");to_home_screen();}
  }

  /* Sent, but no service ever answered (e.g. one still running holds the port
   * yet can't be reached): say so rather than leave the dialog empty. */
  if(starting&&!online&&notice[0]&&g_now>=notice_until)start_notice("The audio service didn't answer. Restart the PS4, then press Start.");
  if(notice[0]&&g_now>=notice_until)notice[0]=0;
  /* Behind another app nothing shows: spare the console the drawing. */
  if(behind){sceKernelUsleep(100000);continue;}
  draw(&ctx);
  memcpy(buffers[buffer],back,sizeof(back));
  /* L3+R3 saves the screen to app-frame.ppm (6 MB), only when asked for. */
  if((buttons&(ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_R3))==(ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_R3)&&now>=snap_cd){snapshot();snap_cd=now+2000000;}
  if(sceVideoOutSubmitFlip(video,buffer,1,flip)<0){sceKernelUsleep(33000);continue;}
  for(int tries=0;tries<1000;tries++){OrbisVideoOutFlipStatus status;if(!sceVideoOutGetFlipStatus(video,&status)&&status.flipArg==flip)break;sceKernelUsleep(1000);}
  buffer^=1;flip++;
 }
 logline(online?"Native app closed; audio service remains running":"Native app closed");
 if(pad>=0)scePadClose(pad);sceVideoOutClose(video);
 return close_app();
}
