/* Screen drawing for the home-screen app. Pure C with no console dependencies, so
 * the PC harness (native/host_ui.c) runs this exact code and renders it to a PPM
 * for the CI render check and for eyeballing a change. Included by main.c.
 *
 * Output is 1920x1080, laid out like the PS4's own Settings screens: a light title
 * over a full-width rule, one centred list whose focused row gets the white focus
 * frame, and the button legend under a second rule at the bottom. Actions for the
 * connected device live in the Options side menu; questions open centred dialogs.
 * While phone control is on, a panel on the left holds the QR code, address and
 * pairing code, and the list moves right to make room.
 * The app is a client of the control API (docs/API.md): all state comes from
 * GET /api/status via model.h, never from a private binary layout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "model.h"
#include "ui_font.h"
#include "ui_icons.h"
#include "qr.h"

#define WIDTH 1920
#define HEIGHT 1080

/* Screens, the Home list's fixed rows (they follow the remembered devices), and
 * the choices on the Options menu and the test-tone screen. */
enum { SCREEN_HOME = 0, SCREEN_SCAN = 1, SCREEN_TEST = 2, SCREEN_LATENCY = 3 };
enum { ROW_ADD = 0, ROW_VOLUME, ROW_TV, ROW_LATENCY, ROW_PHONE, ROW_STOP, ROW_FIXED };
enum { MENU_TEST = 0, MENU_DISCONNECT, MENU_COUNT };
enum { TEST_YES = 0, TEST_AGAIN, TEST_DISCONNECT, TEST_COUNT };
enum { DLG_NONE = 0, DLG_STOP = 1 };
#define HOME_VISIBLE 8   /* list rows that fit above the info line and the bottom rule */
#define SCAN_VISIBLE 7   /* the scan list starts lower, under its status line */

/* Everything input-dependent the renderer needs, filled by main.c each frame
 * (and by the host harness from a scene file). State itself lives in `state`. */
struct ui_ctx {
    int screen;         /* SCREEN_* */
    int focus;          /* focused row on the current screen */
    int top;            /* first visible row of a scrolling list */
    int confirm_circle; /* region: 1 => the Circle button confirms, Cross cancels */
    int online;         /* service is answering; when not, the Start dialog shows */
    int busy;           /* the Start dialog is starting the service (spinner) */
    int menu;           /* Options side menu open (Home, on the connected device) */
    int menu_focus;     /* MENU_* */
    int dialog;         /* DLG_* over Home */
    int dialog_focus;   /* focused dialog button: 0 left, 1 right */
    int pinmode;        /* PIN dialog active */
    const char *pin;    /* four PIN digits */
    int digit;          /* selected PIN digit */
};

/* PS4 theme colours: the wallpaper gradient (top left, middle, bottom right) and
 * the ink cut into the white icon discs of Settings rows. The console's theme
 * isn't exposed through the system parameters this app reads, so ui_theme stays
 * at gray on the console; the harness can switch it per scene. */
struct theme { uint32_t top, mid, bot, ink; };
static const struct theme THEMES[3] = {
    {0x7f8398, 0x676a7f, 0x4e5165, 0x62657a},   /* gray */
    {0x2b66da, 0x1848b8, 0x0b2b88, 0x1a4cbc},   /* default blue */
    {0x34363d, 0x1c1d22, 0x0b0b0d, 0x2a2c33}};  /* black */
static int ui_theme;
#define WHITE 0xffffff
/* The face-button symbols keep their pad colours, so the legend matches the pad. */
#define GLYPH_X 0x9cc0ff
#define GLYPH_O 0xff7a66
#define GLYPH_T 0x3fdcb4

static uint32_t back[WIDTH*HEIGHT]; /* cached off-screen target; the video buffers are slow to read back */
static uint32_t wall[WIDTH*HEIGHT]; /* the theme wallpaper, built once and copied in every frame */
static int wall_theme = -1;
static struct ui_state state;
static uint64_t g_now, notice_until;
static char notice[160];

/* ---- drawing primitives. The PC previewer mirrors these exactly. ---- */
static int cx0, cy0, cx1 = WIDTH, cy1 = HEIGHT;
static void clip(int x0,int y0,int x1,int y1){cx0=x0<0?0:x0;cy0=y0<0?0:y0;cx1=x1>WIDTH?WIDTH:x1;cy1=y1>HEIGHT?HEIGHT:y1;}
static uint32_t mixc(uint32_t a,uint32_t b,unsigned t){
 return (((((a>>16)&255)*(256-t)+((b>>16)&255)*t)>>8)<<16)|(((((a>>8)&255)*(256-t)+((b>>8)&255)*t)>>8)<<8)|(((a&255)*(256-t)+(b&255)*t)>>8);
}
static void blend(int x,int y,uint32_t rgb,unsigned a){
 if(!a||x<cx0||x>=cx1||y<cy0||y>=cy1)return;
 uint32_t*p=&back[y*WIDTH+x];
 if(a>=255){*p=0xff000000u|rgb;return;}
 uint32_t bg=*p;unsigned ia=255-a;
 unsigned r=(((rgb>>16)&255)*a+((bg>>16)&255)*ia+127)/255;
 unsigned g=(((rgb>>8)&255)*a+((bg>>8)&255)*ia+127)/255;
 unsigned b=((rgb&255)*a+(bg&255)*ia+127)/255;
 *p=0xff000000u|(r<<16)|(g<<8)|b;
}
static void fill(int x,int y,int w,int h,uint32_t rgb){
 int x0=x>cx0?x:cx0,y0=y>cy0?y:cy0,x1=x+w<cx1?x+w:cx1,y1=y+h<cy1?y+h:cy1;
 if(x1<=x0||y1<=y0)return;
 uint32_t v=0xff000000u|rgb;
 for(int j=y0;j<y1;j++){uint32_t*row=back+j*WIDTH;for(int i=x0;i<x1;i++)row[i]=v;}
}
static void fill_a(int x,int y,int w,int h,uint32_t rgb,unsigned a){
 if(a>=255){fill(x,y,w,h,rgb);return;}
 int x0=x>cx0?x:cx0,y0=y>cy0?y:cy0,x1=x+w<cx1?x+w:cx1,y1=y+h<cy1?y+h:cy1;
 for(int j=y0;j<y1;j++)for(int i=x0;i<x1;i++)blend(i,j,rgb,a);
}
/* A rectangle's border, t px thick, drawn inside x,y,w,h. */
static void outline(int x,int y,int w,int h,int t,uint32_t rgb,unsigned a){
 fill_a(x,y,w,t,rgb,a);fill_a(x,y+h-t,w,t,rgb,a);fill_a(x,y+t,t,h-2*t,rgb,a);fill_a(x+w-t,y+t,t,h-2*t,rgb,a);
}
/* Coverage (0..255) of pixel (cx,cy) in a top-left corner of radius r: 4x4 supersampling. */
static unsigned corner_cov(int r,int cx,int cy){
 int R8=r*8,n=0;
 for(int t=1;t<8;t+=2)for(int s=1;s<8;s+=2){int dx=8*cx+s-R8,dy=8*cy+t-R8;if(dx*dx+dy*dy<=R8*R8)n++;}
 return (unsigned)((n*255+8)/16);
}
static void rrect(int x,int y,int w,int h,int r,uint32_t rgb){
 if(w<=0||h<=0)return;
 if(r>w/2)r=w/2;if(r>h/2)r=h/2;
 if(r<=0){fill(x,y,w,h,rgb);return;}
 fill(x+r,y,w-2*r,h,rgb);fill(x,y+r,r,h-2*r,rgb);fill(x+w-r,y+r,r,h-2*r,rgb);
 for(int cy=0;cy<r;cy++)for(int cx=0;cx<r;cx++){
  unsigned a=corner_cov(r,cx,cy);if(!a)continue;
  blend(x+cx,y+cy,rgb,a);blend(x+w-1-cx,y+cy,rgb,a);blend(x+cx,y+h-1-cy,rgb,a);blend(x+w-1-cx,y+h-1-cy,rgb,a);
 }
}
static void disc(int cx,int cy,int r,uint32_t rgb,unsigned alpha){
 if(alpha>=255){rrect(cx-r,cy-r,2*r,2*r,r,rgb);return;}
 for(int j=0;j<2*r;j++)for(int i=0;i<2*r;i++){
  int qx=i<r?i:2*r-1-i,qy=j<r?j:2*r-1-j;
  blend(cx-r+i,cy-r+j,rgb,corner_cov(r,qx,qy)*alpha/255);
 }
}
/* The ring between radii ri and ro (in 1/8 px) around the pixel corner (cx,cy);
 * ri<0 fills the whole disc. 4x4 supersampled like corner_cov. */
static void ring(int cx,int cy,int ro,int ri,uint32_t rgb,unsigned a){
 int R=ro/8+2;
 for(int y=-R;y<R;y++)for(int x=-R;x<R;x++){
  int n=0;
  for(int t=1;t<8;t+=2)for(int s=1;s<8;s+=2){int dx=8*x+s,dy=8*y+t,d=dx*dx+dy*dy;if(d<=ro*ro&&(ri<0||d>ri*ri))n++;}
  if(n)blend(cx+x,cy+y,rgb,(unsigned)(n*(int)a+8)/16);
 }
}
/* Is the point (px,py) inside a w x h rounded rectangle of corner radius r? All in 1/8 px. */
static int in_rr(int px,int py,int w,int h,int r){
 if(px<0||py<0||px>=w||py>=h)return 0;
 int qx=px<r?r-px:px>w-r?px-(w-r):0,qy=py<r?r-py:py>h-r?py-(h-r):0;
 return qx*qx+qy*qy<=r*r;
}
/* A rounded rectangle's border, t px thick (used for the small OPTIONS tag). */
static void rr_outline(int x,int y,int w,int h,int r,int t,uint32_t rgb,unsigned a){
 for(int j=0;j<h;j++)for(int i=0;i<w;i++){
  int n=0;
  for(int v=1;v<8;v+=2)for(int u=1;u<8;u+=2){int px=8*i+u,py=8*j+v;
   if(in_rr(px,py,8*w,8*h,8*r)&&!in_rr(px-8*t,py-8*t,8*(w-2*t),8*(h-2*t),8*(r-t)))n++;}
  if(n)blend(x+i,y+j,rgb,(unsigned)(n*(int)a+8)/16);
 }
}
static void icon(int id,int x,int y,uint32_t rgb,unsigned scale){
 const struct icon*ic=&ICONS[id];
 for(int j=0;j<ic->h;j++)for(int i=0;i<ic->w;i++){unsigned a=ic->a[j*ic->w+i];if(a)blend(x+i,y+j,rgb,a*scale/255);}
}
static void icon_c(int id,int cx,int cy,uint32_t rgb,unsigned scale){icon(id,cx-ICONS[id].w/2,cy-ICONS[id].h/2,rgb,scale);}
static void dim_frame(unsigned a){
 unsigned ia=255-a;
 for(int i=0;i<WIDTH*HEIGHT;i++){uint32_t v=back[i];back[i]=0xff000000u|((((v>>16)&255)*ia+127)/255<<16)|((((v>>8)&255)*ia+127)/255<<8)|(((v&255)*ia+127)/255);}
}
/* The PS4 focus frame: a square 3 px white border with a soft glow outside and a
 * faint lift inside. */
static void focus_frame(int x,int y,int w,int h){
 for(int i=12;i>=1;i--)outline(x-i,y-i,w+2*i,h+2*i,1,WHITE,(unsigned)(96*(13-i)*(13-i)/169));
 fill_a(x+3,y+3,w-6,h-6,WHITE,20);
 for(int i=0;i<6;i++)outline(x+3+i,y+3+i,w-6-2*i,h-6-2*i,1,WHITE,(unsigned)(6*(6-i)));
 outline(x,y,w,h,3,WHITE,255);
}

/* ---- wallpaper: the theme gradient with the PS4's soft light curves ---- */
static unsigned char wcov[WIDTH*HEIGHT]; /* one curve's stroke coverage while building */
static void wall_mix(int x,int y,uint32_t rgb,unsigned a){
 if(x<0||y<0||x>=WIDTH||y>=HEIGHT||!a)return;
 uint32_t*p=&wall[y*WIDTH+x];*p=0xff000000u|mixc(*p&0xffffff,rgb,a);
}
struct curve{float x0,y0,x1,y1,x2,y2,x3,y3;};
static void bez(const struct curve*k,float t,float*x,float*y){
 float u=1-t,a=u*u*u,b=3*u*u*t,c=3*u*t*t,d=t*t*t;
 *x=a*k->x0+b*k->x1+c*k->x2+d*k->x3;*y=a*k->y0+b*k->y1+c*k->y2+d*k->y3;
}
#define NOY (-32768)
#define BEZ_STEPS 4096
/* The curve's y in each pixel column it crosses (all these curves run left to right).
 * Left of its start the band's edge runs along the start height, so a band whose
 * curves begin at different x closes smoothly instead of with a vertical edge. */
static void wall_trace(const struct curve*k,short*col){
 for(int x=0;x<WIDTH;x++)col[x]=NOY;
 for(int i=0;i<=BEZ_STEPS;i++){float fx,fy;bez(k,(float)i/BEZ_STEPS,&fx,&fy);int x=(int)fx;if(x>=0&&x<WIDTH)col[x]=(short)fy;}
 for(int x=0;x<WIDTH&&col[x]==NOY;x++)col[x]=(short)k->y0;
}
/* A faint band filling the space between two traced curves. */
static void wall_band(const short*a,const short*b,unsigned alpha){
 for(int x=0;x<WIDTH;x++){
  if(a[x]==NOY||b[x]==NOY)continue;
  int y0=a[x]<b[x]?a[x]:b[x],y1=a[x]<b[x]?b[x]:a[x];
  for(int y=y0<0?0:y0;y<y1&&y<HEIGHT;y++)wall_mix(x,y,WHITE,alpha);
 }
}
/* A round-capped stroke of radius r along the curve. Coverage is max-combined in
 * wcov first, so the overlapping stamps don't build up into a brighter line. */
static void wall_stroke(const struct curve*k,float r,unsigned alpha){
 memset(wcov,0,sizeof(wcov));
 int R=(int)r+2;
 for(int i=0;i<=BEZ_STEPS;i++){
  float fx,fy;bez(k,(float)i/BEZ_STEPS,&fx,&fy);
  for(int y=(int)fy-R;y<=(int)fy+R;y++)for(int x=(int)fx-R;x<=(int)fx+R;x++){
   if(x<0||y<0||x>=WIDTH||y>=HEIGHT)continue;
   int n=0;
   for(int v=0;v<4;v++)for(int u=0;u<4;u++){float dx=x+(u+0.5f)/4-fx,dy=y+(v+0.5f)/4-fy;if(dx*dx+dy*dy<=r*r)n++;}
   unsigned char c=(unsigned char)(n*255/16),*p=&wcov[y*WIDTH+x];if(c>*p)*p=c;
  }
 }
 for(int i=0;i<WIDTH*HEIGHT;i++)if(wcov[i])wall_mix(i%WIDTH,i/WIDTH,WHITE,alpha*wcov[i]/255);
}
static void build_wall(void){
 const struct theme*th=&THEMES[ui_theme];
 for(int y=0;y<HEIGHT;y++)for(int x=0;x<WIDTH;x++){
  /* A 165-degree gradient (down and a little right, 1540 px long), light at the top left. */
  int t=((x-960)*265+(y-540)*989)/1540+512;if(t<0)t=0;if(t>1024)t=1024;
  uint32_t v=t<532?mixc(th->top,th->mid,(unsigned)(t*256/532)):mixc(th->mid,th->bot,(unsigned)((t-532)*256/492));
  /* soft light from the upper right */
  int dx=x-1450,dy=y-230,d2=dx*dx+dy*dy;
  if(d2<577600){int f=1024-(int)((long long)d2*1024/577600);v=mixc(v,WHITE,(unsigned)(41*f/1024*f/1024));}
  wall[y*WIDTH+x]=0xff000000u|v;
 }
 static const struct curve K[5]={
  {760,-20,1080,250,1500,420,1940,300},{700,-20,1060,320,1500,500,1940,380},
  {-20,900,480,800,1120,1010,1940,720},{-20,980,520,880,1160,1080,1940,800},
  {1180,1100,1320,820,1620,640,1940,600}};
 static short ya[WIDTH],yb[WIDTH];
 wall_trace(&K[0],ya);wall_trace(&K[1],yb);wall_band(ya,yb,13);
 wall_trace(&K[2],ya);wall_trace(&K[3],yb);wall_band(ya,yb,11);
 wall_stroke(&K[0],1.0f,87);wall_stroke(&K[1],0.75f,41);
 wall_stroke(&K[2],1.0f,61);wall_stroke(&K[3],0.75f,31);wall_stroke(&K[4],0.75f,36);
 wall_theme=ui_theme;
}

/* ---- text: proportional anti-aliased glyphs, 1/16 px advances ---- */
static int gl_index(unsigned char c){return(c<32||c>126)?63-32:c-32;}
static int text_w(int f,const char*s){
 const struct face*fc=&FACES[f];unsigned tot=0;
 for(;*s;s++)tot+=fc->g[gl_index((unsigned char)*s)].adv16;
 return(int)((tot+8)>>4);
}
/* Text carries the system UI's faint drop shadow, one pixel down. */
static void text(int f,int x,int base,const char*s,uint32_t rgb){
 const struct face*fc=&FACES[f];int pen=x*16;
 for(;*s;s++){
  const struct glyph*g=&fc->g[gl_index((unsigned char)*s)];
  int gx=((pen+8)>>4)+g->bx,gy=base+g->by;
  for(int j=0;j<g->h;j++)for(int i=0;i<g->w;i++){unsigned a=fc->bits[g->off+j*g->w+i];if(a){blend(gx+i,gy+j+1,0x000000,a*60/255);blend(gx+i,gy+j,rgb,a);}}
  pen+=g->adv16;
 }
}
#define TBUF 272
/* Returns s if it fits maxw, else a copy in buf shortened with "...". */
static const char*fit(int f,const char*s,int maxw,char*buf){
 if(text_w(f,s)<=maxw)return s;
 int dots=text_w(f,"...");size_t len=strlen(s);if(len>TBUF-8)len=TBUF-8;
 unsigned pref[TBUF];unsigned acc=0;pref[0]=0;
 for(size_t i=0;i<len;i++){acc+=FACES[f].g[gl_index((unsigned char)s[i])].adv16;pref[i+1]=acc;}
 size_t n=len;while(n>0&&(int)((pref[n]+8)>>4)+dots>maxw)n--;
 while(n>0&&s[n-1]==' ')n--;
 memcpy(buf,s,n);memcpy(buf+n,"...",4);return buf;
}
static void text_fit(int f,int x,int base,const char*s,uint32_t rgb,int maxw){char b[TBUF];text(f,x,base,fit(f,s,maxw,b),rgb);}
static void text_c(int f,int cx,int base,const char*s,uint32_t rgb,int maxw){char b[TBUF];s=fit(f,s,maxw,b);text(f,cx-text_w(f,s)/2,base,s,rgb);}
static void text_r(int f,int rx,int base,const char*s,uint32_t rgb){text(f,rx-text_w(f,s),base,s,rgb);}
/* 8-dot busy indicator (cos,sin * 1024 offsets, starting at 12 o'clock). */
static const short SPIN8[8][2]={{0,-1024},{724,-724},{1024,0},{724,724},{0,1024},{-724,724},{-1024,0},{-724,-724}};
static void spinner(int cx,int cy,int r,uint32_t rgb,uint64_t t_ms,int dr){
 int head=(int)((t_ms/90)%8u);
 for(int i=0;i<8;i++){
  int age=(head-i+8)%8,a=255-age*28;if(a<40)a=40;
  disc(cx+((r*SPIN8[i][0]+512)>>10),cy+((r*SPIN8[i][1]+512)>>10),dr,rgb,(unsigned)a);
 }
}

/* ---- shared helpers ---- */
static int ui_link(const struct ui_ctx*c){
 if(!c->online)return LINK_OFFLINE;
 return state.link>=LINK_IDLE&&state.link<=LINK_SEARCHING?state.link:LINK_IDLE;
}
static const char*LAT_NAMES[3]={"Lowest","Balanced","Most Stable"};
static const char*LAT_NOTES[3]={"Shortest delay","Default","Fewest dropouts"};
static int lat_mode(void){int m=state.latency_mode;return m<0||m>2?1:m;}
static void dev_name(const struct ui_device*d,char*out,size_t cap){
 const char*src=d->name;size_t len=strlen(src),n=len;
 while(n>0&&(src[n-1]=='?'||src[n-1]==' '))n--;
 int q=0;for(size_t i=n;i<len;i++)if(src[i]=='?')q++;
 if(n<len&&q<2){n=len;while(n>0&&src[n-1]==' ')n--;}
 if(n==0){snprintf(out,cap,"Unnamed Device %04lX",d->address&0xffff);return;}
 if(n>=cap)n=cap-1;memcpy(out,src,n);out[n]=0;
}
/* True when device i is the one the service is linked or linking to. */
static int dev_active(const struct ui_ctx*c,int i){
 int l=ui_link(c);
 return i>=0&&i<state.count&&state.devices[i].address==state.selected&&l!=LINK_IDLE&&l!=LINK_OFFLINE;
}
/* Home lists the devices that have played sound through the service before, plus
 * the one it is linked or linking to; Add Device lists everything nearby. Home
 * row k (k < home_count) shows state.devices[home_dev(k)]. */
static int on_home(const struct ui_ctx*c,int i){return state.devices[i].known||dev_active(c,i);}
static int home_count(const struct ui_ctx*c){int n=0;for(int i=0;i<state.count;i++)n+=on_home(c,i);return n;}
static int home_dev(const struct ui_ctx*c,int k){
 for(int i=0;i<state.count;i++)if(on_home(c,i)&&k--==0)return i;
 return -1;
}
static int home_rows(const struct ui_ctx*c){return home_count(c)+ROW_FIXED;}
/* The Options menu belongs to the connected device's row on Home. */
static int focus_is_active(const struct ui_ctx*c){
 return c->screen==SCREEN_HOME&&c->focus>=0&&c->focus<home_count(c)&&dev_active(c,home_dev(c,c->focus));
}

/* ---- list rows ---- */
/* The list is centred, and moves right to make room for the phone-control panel
 * on the left while that is shown (draw() sets list_x each frame). */
#define LIST_X_CENTRED 288
#define LIST_X_PANEL 480
static int list_x = LIST_X_CENTRED;
#define LIST_X list_x
#define LIST_W 1344
#define LIST_Y 172
#define ROW_H 84
#define VALUE_R (LIST_X+LIST_W-28)   /* right edge of a row's value text */
#define BOX_X (LIST_X+LIST_W-68)     /* left edge of a row's checkbox */
static void row_frame(int y,int sel){
 if(sel)focus_frame(LIST_X,y,LIST_W,ROW_H);
 else fill_a(LIST_X+84,y+ROW_H-2,LIST_W-84,2,WHITE,77);   /* separator */
}
static void row_label(int y,const char*s,int maxw){text_fit(F_ROW,LIST_X+96,y+52,s,WHITE,maxw);}
static void row_value(int y,const char*s){text_r(F_SMALL,VALUE_R,y+51,s,WHITE);}
/* A value with the busy indicator in front of it (connecting, searching...). */
static void row_busy(int y,const char*s){row_value(y,s);spinner(VALUE_R-text_w(F_SMALL,s)-30,y+42,11,WHITE,g_now/1000,3);}
/* Settings rows wear the system's white disc with the icon cut in the theme ink. */
static void disc_icon(int id,int cx,int cy){disc(cx,cy,20,WHITE,255);icon_c(id,cx,cy,THEMES[ui_theme].ink,255);}
static void checkbox(int y,int on){
 outline(BOX_X,y+19,46,46,2,WHITE,230);fill_a(BOX_X+2,y+21,42,42,WHITE,20);
 if(on)icon_c(I_CHECK,BOX_X+23,y+42,WHITE,255);
}
static void device_row(const struct ui_ctx*c,int i,int y,int sel){
 char buf[TBUF];int l=ui_link(c);
 row_frame(y,sel);
 icon_c(I_BT,LIST_X+42,y+42,WHITE,255);
 dev_name(&state.devices[i],buf,sizeof(buf));
 row_label(y,buf,LIST_W-96-360);
 if(!dev_active(c,i))return;
 if(l==LINK_STREAMING)row_value(y,"Connected");
 else if(l==LINK_FAILED)row_value(y,"Couldn't Connect");
 else row_busy(y,l==LINK_LOST?"Reconnecting...":l==LINK_SEARCHING?"Searching...":"Connecting...");
}
static void home_row(const struct ui_ctx*c,int i,int y,int sel){
 int nd=home_count(c);
 if(i<nd){device_row(c,home_dev(c,i),y,sel);return;}
 int k=i-nd,cx=LIST_X+42,cy=y+42;char buf[32];
 row_frame(y,sel);
 if(k==ROW_ADD){disc_icon(I_PLUS,cx,cy);row_label(y,"Add Device",900);return;}
 disc_icon(I_WRENCH,cx,cy);
 if(k==ROW_VOLUME){
  int v=state.volume<0?0:state.volume>100?100:state.volume,bx=VALUE_R-36-20-240;
  row_label(y,"Volume",900);snprintf(buf,sizeof(buf),"%d",v);row_value(y,buf);
  fill_a(bx,y+39,240,6,WHITE,77);fill(bx,y+39,240*v/100,6,WHITE);
 }else if(k==ROW_TV){row_label(y,"Keep TV Sound On",900);checkbox(y,state.tv);}
 else if(k==ROW_LATENCY){row_label(y,"Audio Latency",900);row_value(y,LAT_NAMES[lat_mode()]);}
 else if(k==ROW_PHONE){
  row_label(y,"Allow Phone Control",900);checkbox(y,state.lan);
  /* The service reveals the pairing code only to the console (loopback). Without
   * an address there is no phone panel, so the code shows here instead. */
  if(state.lan&&state.pair_code[0]&&!state.ip[0]){snprintf(buf,sizeof(buf),"Code %s",state.pair_code);text_r(F_SMALL,BOX_X-28,y+51,buf,WHITE);}
 }else row_label(y,"Stop Audio Service",900);
}
/* The thin native scrollbar to the right of a list that doesn't fit. */
static void scrollbar(int y,int visible,int total,int top){
 if(total<=visible)return;
 int h=visible*ROW_H,x=LIST_X+LIST_W+28;
 fill_a(x,y,2,h,WHITE,77);fill_a(x-2,y+h*top/total,6,h*visible/total,WHITE,230);
}

/* ======================= screens ======================= */
static void draw_home(const struct ui_ctx*c){
 int n=home_rows(c);
 for(int r=0;r<HOME_VISIBLE&&c->top+r<n;r++)home_row(c,c->top+r,LIST_Y+r*ROW_H,c->focus==c->top+r);
 scrollbar(LIST_Y,HOME_VISIBLE,n,c->top);
}
static void draw_scan(const struct ui_ctx*c){
 int n=state.count;
 if(state.scanning||ui_link(c)==LINK_SEARCHING){spinner(LIST_X+42,198,11,WHITE,g_now/1000,3);text(F_ROW,LIST_X+96,208,"Searching...",WHITE);}
 else if(!n)text(F_ROW,LIST_X+96,208,"No devices found.",WHITE);
 for(int r=0;r<SCAN_VISIBLE&&c->top+r<n;r++)device_row(c,c->top+r,244+r*ROW_H,c->focus==c->top+r);
 scrollbar(244,SCAN_VISIBLE,n,c->top);
}
static const char*TEST_ITEMS[TEST_COUNT]={"Yes","No, Play Again","Disconnect"};
static void draw_test(const struct ui_ctx*c){
 text_c(F_ROW,LIST_X+LIST_W/2,218,"Can you hear the test tone?",WHITE,1600);
 for(int i=0;i<TEST_COUNT;i++){
  int y=268+i*90;
  if(c->focus==i)focus_frame(LIST_X,y,LIST_W,ROW_H);
  text(F_ROW,LIST_X+39,y+52,TEST_ITEMS[i],WHITE);
 }
}
/* Like the system's choice lists: a check marks the current mode, the frame the focus. */
static void draw_latency(const struct ui_ctx*c){
 /* Full width, or the list's width beside the phone panel. */
 int x=LIST_X==LIST_X_CENTRED?137:LIST_X,w=LIST_X==LIST_X_CENTRED?1651:LIST_W;
 for(int i=0;i<3;i++){
  int y=178+i*96;
  if(c->focus==i)focus_frame(x,y,w,ROW_H);
  if(i==lat_mode())icon_c(I_CHECK,x+46,y+42,WHITE,255);
  text(F_ROW,x+89,y+52,LAT_NAMES[i],WHITE);text(F_ROW,x+660,y+52,LAT_NOTES[i],WHITE);
 }
}
/* The Options side menu: black panel on the right, the list behind it faded. */
static const char*MENU_ITEMS[MENU_COUNT]={"Test Sound","Disconnect"};
static void draw_menu(const struct ui_ctx*c){
 fill_a(0,140,1440,803,THEMES[ui_theme].mid,150);
 fill_a(1440,0,480,HEIGHT,0x000000,245);
 for(int i=0;i<MENU_COUNT;i++){
  int y=440+i*78;
  if(c->menu_focus==i)focus_frame(1462,y,436,74);
  text(F_ROW,1495,y+48,MENU_ITEMS[i],WHITE);
 }
}

/* ---- dialogs: the screen dims and a centred panel asks one question ---- */
static void dialog_panel(int x,int y,int w,int h){
 const struct theme*th=&THEMES[ui_theme];
 for(int j=0;j<h;j++)fill(x,y+j,w,1,mixc(th->mid,th->bot,(unsigned)(j*256/h)));
 outline(x,y,w,h,2,WHITE,90);
}
static void dialog_button(int x,int y,const char*label,int sel){
 if(sel)focus_frame(x,y,320,72);
 else{outline(x,y,320,72,2,WHITE,153);fill_a(x+2,y+2,316,68,WHITE,10);}
 text_c(F_ROW,x+160,y+47,label,WHITE,300);
}
static void dialog2(const char*title,const char*body,const char*b0,const char*b1,int focus){
 int y=340;
 dim_frame(140);dialog_panel(480,y,960,300);
 if(body&&body[0]){text_c(F_ROW,960,y+92,title,WHITE,840);text_c(F_SMALL,960,y+140,body,WHITE,840);}
 else text_c(F_ROW,960,y+110,title,WHITE,840);
 dialog_button(624,y+180,b0,focus==0);dialog_button(976,y+180,b1,focus==1);
}
/* PIN entry: left/right pick a digit, up/down change it. */
static void draw_pin(const struct ui_ctx*c){
 int y=250;char t[TBUF];
 dim_frame(140);dialog_panel(480,y,960,360);
 if(state.name[0])snprintf(t,sizeof(t),"Enter the PIN for %s.",state.name);
 else snprintf(t,sizeof(t),"%s","Enter the PIN for this device.");
 text_c(F_ROW,960,y+90,t,WHITE,840);
 for(int i=0;i<4;i++){
  int bx=732+i*120,by=y+164;char ch[2]={c->pin[i],0};
  if(i==c->digit){
   focus_frame(bx,by,96,112);
   icon_c(I_CHEV_UP,bx+48,by-20,WHITE,255);icon_c(I_CHEV_DOWN,bx+48,by+132,WHITE,255);
  }else{outline(bx,by,96,112,2,WHITE,140);fill_a(bx+2,by+2,92,108,0x000000,46);}
  text_c(F_TITLE,bx+48,by+76,ch,WHITE,90);
 }
}

/* ---- phone control: QR code, address and code on the left while it is on ---- */
static int phone_panel(const struct ui_ctx*c){return c->online&&state.lan&&state.ip[0]&&state.pair_code[0];}
static struct qr phone_qr;
static char phone_url[32];
static void draw_phone(void){
 char url[32],buf[32];
 snprintf(url,sizeof(url),"http://%s:8097",state.ip);
 if(strcmp(url,phone_url)){snprintf(phone_url,sizeof(phone_url),"%s",url);qr_encode(&phone_qr,url);}
 int box=296,x=106,y=LIST_Y+4,cx=x+box/2;
 rrect(x,y,box,box,10,WHITE);
 if(phone_qr.size){ /* dark modules on the white tile, which leaves at least the 4-module quiet zone */
  int n=phone_qr.size,m=box/(n+8),o=(box-m*n)/2;
  for(int r=0;r<n;r++)for(int k=0;k<n;k++)if(phone_qr.m[r][k])fill(x+o+k*m,y+o+r*m,m,m,0x000000);
 }
 text_c(F_SMALL,cx,y+box+46,"Scan, or go to",WHITE,box);
 snprintf(buf,sizeof(buf),"%s:8097",state.ip);text_c(F_SMALL,cx,y+box+82,buf,WHITE,box);
 snprintf(buf,sizeof(buf),"Code %s",state.pair_code);text_c(F_ROW,cx,y+box+138,buf,WHITE,box);
}

/* ---- button legend along the bottom rule ---- */
enum { HINT_ENTER, HINT_BACK, HINT_TRIANGLE, HINT_OPTIONS };
/* The confirm symbol follows the console's enter-button setting (Cross in most
 * regions, Circle in Japan); the cancel symbol is always the other one. */
static void glyph(int kind,int cx,int cy,int circle_confirms){
 ring(cx,cy,107,-1,0x000000,51);ring(cx,cy,120,107,WHITE,255);
 if(kind==HINT_TRIANGLE)icon_c(I_GT,cx,cy,GLYPH_T,255);
 else if((kind==HINT_ENTER)!=(circle_confirms!=0))icon_c(I_GX,cx,cy,GLYPH_X,255);
 else icon_c(I_GO,cx,cy,GLYPH_O,255);
}
static void legend(const struct ui_ctx*c,int linew,int back,int search,int options){
 static const char*LABELS[4]={"Enter","Back","Search Again","Options Menu"};
 int kinds[4],n=0,x=106;
 kinds[n++]=HINT_ENTER;if(back)kinds[n++]=HINT_BACK;
 if(search)kinds[n++]=HINT_TRIANGLE;
 if(options)kinds[n++]=HINT_OPTIONS;
 fill_a(0,943,linew,2,WHITE,128);
 for(int k=0;k<n;k++){
  if(kinds[k]==HINT_OPTIONS){int tw=text_w(F_TAG,"OPTIONS")+20;rr_outline(x,971,tw,26,13,2,WHITE,255);text(F_TAG,x+10,989,"OPTIONS",WHITE);x+=tw+10;}
  else{glyph(kinds[k],x+16,984,c->confirm_circle);x+=42;}
  text(F_SMALL,x,993,LABELS[kinds[k]],WHITE);x+=text_w(F_SMALL,LABELS[kinds[k]])+44;
 }
}

/* ---- top-level draw ---- */
static void draw(const struct ui_ctx*c){
 clip(0,0,WIDTH,HEIGHT);
 if(wall_theme!=ui_theme)build_wall();
 memcpy(back,wall,sizeof(back));
 int sc=c->screen;if(sc<SCREEN_HOME||sc>SCREEN_LATENCY)sc=SCREEN_HOME;
 text(F_TITLE,106,112,sc==SCREEN_HOME?"Bluetooth Audio":sc==SCREEN_LATENCY?"Audio Latency":"Add Device",WHITE);
 fill_a(0,137,WIDTH,2,WHITE,128);
 /* While phone control is on, every screen keeps the way in on the left. */
 int panel=phone_panel(c);
 list_x=panel?LIST_X_PANEL:LIST_X_CENTRED;
 if(panel)draw_phone();
 /* Offline, the Start dialog stands alone: a list behind it would show values the
  * service never reported. */
 if(!c->online){}
 else if(sc==SCREEN_SCAN)draw_scan(c);
 else if(sc==SCREEN_TEST)draw_test(c);
 else if(sc==SCREEN_LATENCY)draw_latency(c);
 else draw_home(c);
 int menu=c->menu&&sc==SCREEN_HOME&&c->online,modal=c->pinmode||!c->online||c->dialog;
 if(menu)draw_menu(c);
 /* One line of feedback above the legend, like the system's info text: the
  * app's own notice, else why the link failed. */
 int noticing=notice[0]&&g_now<notice_until;
 if(!c->online){
  dialog2("The audio service isn't running.",noticing?notice:"","Start","Close",c->dialog_focus);
  /* Starting: the busy indicator in front of the message, as on a busy row. */
  if(c->busy&&noticing)spinner(960-text_w(F_SMALL,notice)/2-30,471,11,WHITE,g_now/1000,3);
 }
 else if(c->dialog==DLG_STOP)dialog2("Stop the audio service?","","Stop","Cancel",c->dialog_focus);
 else if(noticing)text_c(F_SMALL,960,905,notice,WHITE,1600);
 else if(sc==SCREEN_HOME&&ui_link(c)==LINK_FAILED&&state.message[0])text_c(F_SMALL,960,905,state.message,WHITE,1600);
 if(c->pinmode)draw_pin(c);
 /* Back on Home does nothing (the app stays open; the PS button leaves it), so it isn't offered there. */
 int show_back=modal||menu||sc!=SCREEN_HOME;
 legend(c,menu&&!modal?1440:WIDTH,show_back,sc==SCREEN_SCAN&&!modal&&!state.scanning,!modal&&(menu||focus_is_active(c)));
}
