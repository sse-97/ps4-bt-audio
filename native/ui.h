/* Screen drawing for the home-screen app. Pure C with no console dependencies, so
 * the PC harness (native/host_ui.c) runs this exact code and renders it to a PPM
 * for the CI render check and for eyeballing a change. Included by main.c.
 *
 * Output is 1920x1080. Everything stays inside the inner 90% TV safe area
 * (96 px side margins, 54 px top/bottom). The app is a client of the control API
 * (docs/API.md): all state comes from GET /api/status via model.h, never from a
 * private binary layout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "model.h"
#include "ui_font.h"
#include "ui_icons.h"

#define WIDTH 1920
#define HEIGHT 1080
#define MARGIN 96

/* Screens and the sub-steps of the guided "Add headphones" flow. */
enum { SCREEN_HOME = 0, SCREEN_ADD = 1, SCREEN_FIX = 2 };
enum { ADD_TYPE = 0, ADD_SCAN = 1, ADD_TEST = 2 };

/* Everything input-dependent the renderer needs, filled by main.c each frame
 * (and by the host harness from a scene file). State itself lives in `state`. */
struct ui_ctx {
    int screen;         /* SCREEN_* */
    int focus;          /* focus index within the current screen's focus ring */
    int top;            /* scroll offset for the Home device list */
    int confirm_circle; /* region: 1 => the Circle button confirms, Cross cancels */
    int online;         /* service is answering */
    int add_step;       /* ADD_* while on the Add screen */
    int add_type;       /* 0 earbuds, 1 headphones, 2 speaker */
    int pinmode;        /* PIN overlay active */
    const char *pin;    /* four PIN digits */
    int digit;          /* selected PIN digit */
};

/* PlayStation design system, translated to a 10-foot console surface.
 *
 * The page is a Console-Black "channel": a near-black section gradient for the
 * canvas, dark elevated panels for content, and a PlayStation-blue anchored bar
 * along the bottom (the web footer's cobalt anchor). PlayStation Blue is the
 * resting accent; Cyan is reserved strictly for focus (see focus_panel) and
 * never appears at rest. There is no green and no warm colour in the chrome:
 * working/active states read blue, problems read red.
 */
#define BG_TOP 0x101217          /* dark section gradient, near-black */
#define BG_BOT 0x000000          /* console black */
#define CARD   0x12151d          /* content panel on black */
#define CARD2  0x191e29          /* inner row / inlay surface */
#define FOOTER 0x071a30          /* blue-anchored bottom bar (cobalt, dimmed) */
#define LINE   0x242b38
#define TEXT   0xffffff          /* inverse white on dark */
#define DIM    0xbac3d1          /* secondary text */
#define FAINT  0x7a8698          /* tertiary text / metadata */
#define SUBTLE 0x9aa6b7
#define PSBLUE 0x0070cc          /* PlayStation Blue — brand anchor, resting accent */
#define PSBLUEH 0x38a0ff         /* brightened blue for presence on dark (active, meters) */
#define PSCYAN 0x1eaedb          /* interaction colour — focus only, never at rest */
#define STEEL  0x8fb6e6          /* soft blue for in-progress (connecting/searching) */
#define RED    0xff5d6c          /* warning red, lightened for dark surfaces */
#define GREY   0x8795ab          /* neutral: idle / unknown */
/* The physical DualShock face-button glyphs keep their iconic colours — ✕ blue,
 * ○ red, □ pink, △ green — so the on-screen prompts match the pad in hand. */
#define BTN_X  0x7ba7ff
#define BTN_SQ 0xff9ad5
#define BTN_TRI 0x5fe0a4
#define BTN_CI 0xff7a7a

static uint32_t back[WIDTH*HEIGHT]; /* cached off-screen target; the video buffers are slow to read back */
static struct ui_state state;
static int meter_s;
static uint64_t g_now, testing_until, notice_until;
static char notice[160];

/* ---- drawing primitives. The PC previewer mirrors these exactly (integer math). ---- */
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
static void gradient(uint32_t top_c,uint32_t bot_c){
 for(int y=0;y<HEIGHT;y++){uint32_t v=0xff000000u|mixc(top_c,bot_c,(unsigned)((y*256)/(HEIGHT-1)));uint32_t*row=back+y*WIDTH;for(int x=0;x<WIDTH;x++)row[x]=v;}
}
static void fill(int x,int y,int w,int h,uint32_t rgb){
 int x0=x>cx0?x:cx0,y0=y>cy0?y:cy0,x1=x+w<cx1?x+w:cx1,y1=y+h<cy1?y+h:cy1;
 if(x1<=x0||y1<=y0)return;
 uint32_t v=0xff000000u|rgb;
 for(int j=y0;j<y1;j++){uint32_t*row=back+j*WIDTH;for(int i=x0;i<x1;i++)row[i]=v;}
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
static void icon(int id,int x,int y,uint32_t rgb,unsigned scale){
 const struct icon*ic=&ICONS[id];
 for(int j=0;j<ic->h;j++)for(int i=0;i<ic->w;i++){unsigned a=ic->a[j*ic->w+i];if(a)blend(x+i,y+j,rgb,a*scale/255);}
}
static void icon_c(int id,int cx,int cy,uint32_t rgb,unsigned scale){icon(id,cx-ICONS[id].w/2,cy-ICONS[id].h/2,rgb,scale);}
static void logo(int x,int y){
 for(int j=0;j<LOGO_H;j++)for(int i=0;i<LOGO_W;i++){unsigned p=logo_argb[j*LOGO_W+i];unsigned a=p>>24;if(a)blend(x+i,y+j,p&0xffffff,a);}
}
static void dim_frame(unsigned a){
 unsigned ia=255-a;
 for(int i=0;i<WIDTH*HEIGHT;i++){uint32_t v=back[i];back[i]=0xff000000u|((((v>>16)&255)*ia+127)/255<<16)|((((v>>8)&255)*ia+127)/255<<8)|(((v&255)*ia+127)/255);}
}
/* The PlayStation focus signature, translated from web hover to d-pad focus: an
 * element at rest is a dark panel; focused, it "powers on" with a 2px blue outer
 * ring, a 2px white border, a cyan-lit fill, and a small lift (drawn 4px larger
 * on every side). Nested filled rrects stand in for the ring/border strokes. The
 * fill stays dark-enough that white body text keeps its couch-distance contrast,
 * so the cyan reads as an edge-lit glow rather than a flat wash behind the text.
 * Returns the fill colour so callers can tint sub-elements against it. */
static uint32_t focus_panel(int x,int y,int w,int h,int r,int sel,uint32_t base){
 if(!sel){rrect(x,y,w,h,r,base);return base;}
 rrect(x-4,y-4,w+8,h+8,r+4,PSBLUE);      /* blue outer ring + lift */
 rrect(x-2,y-2,w+4,h+4,r+2,0xffffff);    /* white border */
 uint32_t f=mixc(base,PSCYAN,44);         /* cyan-lit fill */
 rrect(x,y,w,h,r,f);
 return f;
}

/* ---- text: proportional anti-aliased glyphs, 1/16 px advances ---- */
static int gl_index(unsigned char c){return(c<32||c>126)?63-32:c-32;}
static int text_w(int f,const char*s){
 const struct face*fc=&FACES[f];unsigned tot=0;
 for(;*s;s++)tot+=fc->g[gl_index((unsigned char)*s)].adv16;
 return(int)((tot+8)>>4);
}
static void text(int f,int x,int base,const char*s,uint32_t rgb){
 const struct face*fc=&FACES[f];int pen=x*16;
 for(;*s;s++){
  const struct glyph*g=&fc->g[gl_index((unsigned char)*s)];
  int gx=((pen+8)>>4)+g->bx,gy=base+g->by;
  for(int j=0;j<g->h;j++)for(int i=0;i<g->w;i++){unsigned a=fc->bits[g->off+j*g->w+i];if(a)blend(gx+i,gy+j,rgb,a);}
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
/* Greedy wrap to at most two lines (second line ellipsised if text remains). */
static int wrap2(int f,const char*s,int maxw,char l1[TBUF],char l2[TBUF]){
 char lines[3][TBUF];int nl=0,more=0;char cur[TBUF];cur[0]=0;
 const char*p=s;
 while(*p){
  const char*q=p;while(*q&&*q!=' ')q++;
  size_t wl=(size_t)(q-p);if(wl>TBUF/2)wl=TBUF/2;
  char t[TBUF];size_t cl=strlen(cur);
  if(cl)snprintf(t,sizeof(t),"%s %.*s",cur,(int)wl,p);else snprintf(t,sizeof(t),"%.*s",(int)wl,p);
  if(text_w(f,t)<=maxw||!cl)memcpy(cur,t,strlen(t)+1);
  else{if(nl<3)memcpy(lines[nl++],cur,cl+1);snprintf(cur,sizeof(cur),"%.*s",(int)wl,p);if(nl>=3){more=1;break;}}
  p=*q?q+1:q;
 }
 if(!more){if(nl<3)memcpy(lines[nl++],cur,strlen(cur)+1);else more=1;}
 char b[TBUF];
 if(nl<=1&&!more){snprintf(l1,TBUF,"%s",fit(f,lines[0],maxw,b));l2[0]=0;return 1;}
 snprintf(l1,TBUF,"%s",fit(f,lines[0],maxw,b));
 if(nl>2||more){char t[TBUF];snprintf(t,sizeof(t),"%s ...",lines[1]);snprintf(l2,TBUF,"%s",fit(f,t,maxw,b));}
 else snprintf(l2,TBUF,"%s",fit(f,lines[1],maxw,b));
 return 2;
}
/* 8- and 10-dot spinner offsets (cos,sin * 1024), starting at 12 o'clock. */
static const short SPIN8[8][2]={{0,-1024},{724,-724},{1024,0},{724,724},{0,1024},{-724,724},{-1024,0},{-724,-724}};
static const short SPIN10[10][2]={{0,-1024},{602,-828},{974,-316},{974,316},{602,828},{0,1024},{-602,828},{-974,316},{-974,-316},{-602,-828}};
static void spinner(int cx,int cy,int r,uint32_t rgb,uint64_t t_ms,int dots,int dr){
 const short(*tab)[2]=dots==8?SPIN8:SPIN10;int head=(int)((t_ms/90)%(uint64_t)dots);
 for(int i=0;i<dots;i++){
  int age=(head-i+dots)%dots,a=255-age*28;if(a<40)a=40;
  disc(cx+((r*tab[i][0]+512)>>10),cy+((r*tab[i][1]+512)>>10),dr,rgb,(unsigned)a);
 }
}

/* ---- shared helpers ---- */
static int ui_link(const struct ui_ctx*c){
 if(!c->online)return LINK_OFFLINE;
 return state.link>=LINK_IDLE&&state.link<=LINK_SEARCHING?state.link:LINK_IDLE;
}
static uint32_t link_color(int l){
 switch(l){case LINK_STREAMING:return PSBLUEH;case LINK_CONNECTING:return STEEL;case LINK_SEARCHING:return STEEL;
  case LINK_LOST:case LINK_FAILED:case LINK_OFFLINE:return RED;default:return GREY;}
}
static int secs(int ms){return(ms+999)/1000;}
static const char*LAT_NAMES[3]={"Lowest","Balanced","Most stable"};
static int lat_mode(void){int m=state.latency_mode;return m<0||m>2?1:m;}
/* The connecting phase in words: the service distinguishes pairing and
 * configuring, which the coarse link state folds into "connecting". */
static const char*connect_word(void){
 if(!strcmp(state.conn_state,"pairing"))return "Pairing";
 if(!strcmp(state.conn_state,"configuring"))return "Configuring";
 return "Connecting";
}
static void dev_name(const struct ui_device*d,char*out,size_t cap){
 const char*src=d->name;size_t len=strlen(src),n=len;
 while(n>0&&(src[n-1]=='?'||src[n-1]==' '))n--;
 int q=0;for(size_t i=n;i<len;i++)if(src[i]=='?')q++;
 if(n<len&&q<2){n=len;while(n>0&&src[n-1]==' ')n--;}
 if(n==0){snprintf(out,cap,"Unnamed device %04lX",d->address&0xffff);return;}
 if(n>=cap)n=cap-1;memcpy(out,src,n);out[n]=0;
}
/* The glyph for the physical "confirm" button, following the console's enter-button
 * setting (Cross in most regions, Circle in Japan/parts of Asia). The cancel glyph
 * is always the other one. Shape keeps its conventional colour so it matches the pad. */
static int confirm_icon(const struct ui_ctx*c){return c->confirm_circle?I_BCIRCLE:I_BCROSS;}
static int cancel_icon(const struct ui_ctx*c){return c->confirm_circle?I_BCROSS:I_BCIRCLE;}
static uint32_t btn_glyph_color(int id){return id==I_BCROSS?BTN_X:id==I_BCIRCLE?BTN_CI:id==I_BSQUARE?BTN_SQ:BTN_TRI;}
/* True when the Home-focused row is the device currently linked (so confirm
 * disconnects it rather than starting a new connection). */
static int focus_is_connected(const struct ui_ctx*c){
 int l=ui_link(c);
 if(c->focus<0||c->focus>=state.count)return 0;
 return state.devices[c->focus].address==state.selected&&l!=LINK_IDLE&&l!=LINK_OFFLINE;
}

/* ---- header: logo, title, live status pill (shared by every screen) ---- */
static void header(const struct ui_ctx*c,const char*subtitle){
 int l=ui_link(c);uint32_t col=link_color(l);uint64_t t=g_now/1000;
 logo(MARGIN,44);
 text(F_TITLE,232,116,"Bluetooth Audio",TEXT);
 text(F_SMALL,236,156,subtitle,SUBTLE);
 const char*pill=l==LINK_STREAMING?"Connected":l==LINK_CONNECTING?"Connecting":l==LINK_SEARCHING?"Searching":l==LINK_LOST?"Disconnected":l==LINK_FAILED?"Can't connect":l==LINK_IDLE?"Not connected":"Service offline";
 int pw=text_w(F_SMALLB,pill)+32+34+30,px=1824-pw;
 rrect(px,56,pw,56,28,mixc(CARD,col,36));
 unsigned pulse=(l==LINK_CONNECTING||l==LINK_SEARCHING)?150+(unsigned)((t/6)%100):255;
 disc(px+34,84,8,col,pulse>255?255:pulse);
 text(F_SMALLB,px+58,94,pill,mixc(TEXT,col,90));
 /* Phone-control pairing code: shown whenever LAN control is on. The service
  * only reveals the code to the console (loopback), so this is console-only. */
 if(state.lan&&state.pair_code[0]){
  char label[32];snprintf(label,sizeof(label),"Phone code %s",state.pair_code);
  int cw=text_w(F_SMALLB,label)+34+30+16,cxp=px-20-cw;
  rrect(cxp,56,cw,56,28,mixc(CARD,PSBLUE,26));
  icon_c(I_PHONE,cxp+32,84,STEEL,255);
  text(F_SMALLB,cxp+54,94,label,mixc(TEXT,STEEL,60));
 }
}

/* ---- status banner: the one place problems are spelled out (Home only) ---- */
static void banner(const struct ui_ctx*c){
 int l=ui_link(c);uint32_t col=link_color(l);uint64_t t=g_now/1000;
 int retry=state.retry_ms,quiet=state.quiet_ms;
 const char*msg=state.message;char full[TBUF],b1[TBUF],b2[TBUF];
 int has=1;uint32_t bc=STEEL;int spin=0;const char*btext=0;
 if(notice[0]&&g_now<notice_until){bc=STEEL;btext=notice;}
 else if(l==LINK_OFFLINE){bc=RED;btext="The audio service isn't running. Press the confirm button to start it, or jailbreak again with GoldHEN's BinLoader enabled.";}
 else if(l==LINK_LOST||l==LINK_FAILED){bc=RED;
  if(retry>0)snprintf(full,sizeof(full),"%s Next try in %ds.",msg,secs(retry));else snprintf(full,sizeof(full),"%s",msg);btext=full;}
 else if(l==LINK_CONNECTING){bc=STEEL;spin=1;btext=msg;}
 else if(l==LINK_SEARCHING){bc=STEEL;spin=1;btext=msg;}
 else if(l==LINK_STREAMING&&quiet>4000){bc=STEEL;btext="Connected, waiting for sound from the PS4. Play something or press Options to test.";}
 else has=0;
 if(has){
  rrect(MARGIN,176,1728,100,20,mixc(CARD,bc,30));rrect(MARGIN,176,8,100,4,bc);
  if(spin)spinner(146,226,20,bc,t,8,4);else{disc(146,226,24,bc,255);icon_c(I_BANG,146,226,BG_TOP,255);}
  int nl=wrap2(F_BODY,btext,1728-130-40,b1,b2);
  if(nl==1)text(F_BODY,192,236,b1,TEXT);else{text(F_BODY,192,218,b1,TEXT);text(F_BODY,192,256,b2,TEXT);}
 }else{
  disc(120,226,7,col,255);
  text(F_BODY,148,236,l==LINK_STREAMING?"Game audio is playing on your headphones.":"Choose your headphones, then press the confirm button to connect.",DIM);
 }
}

/* ---- a reusable legend chip (icon or button tag + label) ---- */
struct chip{int icon;const char*tag1,*tag2,*label;uint32_t lc;};
static void legend(const struct chip*items,int count){
 int x=MARGIN;
 for(int k=0;k<count;k++){
  int w=text_w(F_SMALL,items[k].label)+40;int t1=0,t2=0;
  if(items[k].icon>=0)w+=44;
  if(items[k].tag1){t1=text_w(F_SMALLB,items[k].tag1)+24;w+=t1+10;if(items[k].tag2){t2=text_w(F_SMALLB,items[k].tag2)+24;w+=t2+8;}}
  rrect(x,952,w,58,29,CARD);
  int tx=x+20;
  if(items[k].icon>=0){disc(x+32,981,20,CARD2,255);icon_c(items[k].icon,x+32,981,btn_glyph_color(items[k].icon),255);tx=x+62;}
  if(items[k].tag1){
   tx=x+16;rrect(tx,966,t1,34,10,CARD2);text(F_SMALLB,tx+12,991,items[k].tag1,items[k].lc);tx+=t1+8;
   if(items[k].tag2){rrect(tx,966,t2,34,10,CARD2);text(F_SMALLB,tx+12,991,items[k].tag2,items[k].lc);tx+=t2+8;}
   tx+=4;
  }
  text(F_SMALL,tx,991,items[k].label,items[k].lc);
  x+=w+16;
 }
}

/* ---- one device row, used by Home and the Add scan list ---- */
static void device_row(const struct ui_ctx*c,int i,int x,int y,int w,int sel){
 int l=ui_link(c);uint32_t col=link_color(l);char buf[TBUF];
 const struct ui_device*d=&state.devices[i];
 int is_t=d->address==state.selected&&l!=LINK_IDLE&&l!=LINK_OFFLINE;
 uint32_t rf=focus_panel(x,y,w,72,16,sel,CARD2);
 uint32_t ic=is_t?col:FAINT;
 disc(x+48,y+36,26,mixc(rf,ic,40),255);
 icon_c(I_ROWHP,x+48,y+36,is_t?mixc(TEXT,ic,60):DIM,255);
 char sub[80];sub[0]=0;uint32_t subc=FAINT;
 if(is_t){
  switch(l){
   case LINK_CONNECTING:if(state.attempts>1)snprintf(sub,sizeof(sub),"%s... attempt %d",connect_word(),state.attempts);else snprintf(sub,sizeof(sub),"%s...",connect_word());break;
   case LINK_SEARCHING:snprintf(sub,sizeof(sub),"Searching...");break;
   case LINK_LOST:snprintf(sub,sizeof(sub),"Connection lost - reconnecting");break;
   case LINK_FAILED:snprintf(sub,sizeof(sub),"Can't connect");break;
   default:break;
  }
  subc=col;
 }else if(d->address==state.selected&&l==LINK_IDLE)snprintf(sub,sizeof(sub),"Last used");
 dev_name(d,buf,sizeof(buf));
 int textmax=w-120-200;
 text_fit(F_NAME,x+88,y+(sub[0]?30:46),buf,TEXT,textmax);
 if(sub[0])text_fit(F_SMALL,x+88,y+58,sub,subc,textmax);
 int re=x+w-24;
 if(sel&&!is_t){int cw=text_w(F_SMALLB,"Connect");icon_c(confirm_icon(c),re-cw-26,y+36,btn_glyph_color(confirm_icon(c)),255);text_r(F_SMALLB,re,y+45,"Connect",TEXT);}
 else if(is_t){
  const char*label=l==LINK_STREAMING?"PLAYING":l==LINK_CONNECTING?"CONNECTING":l==LINK_SEARCHING?"SEARCHING":l==LINK_LOST?"LOST":"FAILED";
  int lw=text_w(F_SMALLB,label)+36;rrect(re-lw,y+19,lw,34,17,mixc(CARD2,col,50));text(F_SMALLB,re-lw+18,y+44,label,col);
 }
}

/* ---- first-run / empty Home: one centered gallery hero instead of two empty
 * cards. Leads with a single call to action and lets whitespace do the work,
 * the design system's gallery pace. The focus ring is unchanged (Add=0, Fix=1),
 * so main.c's input handling needs no change. ---- */
static void draw_home_empty(const struct ui_ctx*c){
 int off=!c->online;uint32_t accent=off?RED:PSBLUEH;
 if(off)banner(c);                       /* offline keeps the how-to-start banner */
 int cx=960,cy=402;
 icon_c(I_GLOW,cx,cy,accent,off?95:66);
 disc(cx,cy,104,accent,255);
 disc(cx,cy,99,mixc(CARD,accent,off?46:32),255);
 icon_c(I_HERO,cx,cy-2,off?mixc(TEXT,RED,30):TEXT,255);
 if(off){disc(cx+72,cy+72,30,CARD,255);disc(cx+72,cy+72,26,RED,255);icon_c(I_BANG,cx+72,cy+72,BG_TOP,255);}
 text_c(F_TITLE,cx,566,off?"Start the audio service":"Add your headphones",TEXT,1400);
 text_c(F_BODY,cx,620,off?"Press the confirm button below to start it.":
  "Pair your first wireless headphones, earbuds or speaker. It only takes a minute.",DIM,1320);
 struct{int icon;const char*label;}acts[2]={
  {I_ADD, off?"Start service":"Add headphones"},
  {I_WRENCH,"Fix a problem"}};
 int ph=80,py=700;
 int w0=112+text_w(F_NAME,acts[0].label),w1=112+text_w(F_NAME,acts[1].label);
 int gap=28,x0=cx-(w0+gap+w1)/2;int xs[2]={x0,x0+w0+gap},ws[2]={w0,w1};
 for(int a=0;a<2;a++){
  int sel=c->focus==a,x=xs[a],w=ws[a];
  /* Dark base both: focus (Add is focus 0 on entry) lights with the power-on
   * signature, the same read as every other focusable element. */
  focus_panel(x,py,w,ph,ph/2,sel,CARD2);
  icon_c(acts[a].icon,x+48,py+ph/2,sel?TEXT:PSBLUEH,255);
  text(F_NAME,x+80,py+ph/2+13,acts[a].label,TEXT);
 }
}

/* ======================= Home ======================= */
static void draw_home(const struct ui_ctx*c){
 uint64_t t=g_now/1000;int l=ui_link(c);uint32_t col=link_color(l);int n=state.count;
 int testing=state.test||g_now<testing_until;
 char buf[TBUF];
 if(n==0){
  draw_home_empty(c);
  if(!c->pinmode){
   char tvlabel[32];snprintf(tvlabel,sizeof(tvlabel),"TV: %s",state.tv?"On":"Muted");
   struct chip chips[4]={
    {confirm_icon(c),0,0,!c->online?"Start service":"Select",DIM},
    {I_BTRI,0,0,"Add headphones",DIM},
    {I_BSQUARE,0,0,tvlabel,DIM},
    {cancel_icon(c),0,0,"Close",DIM}};
   legend(chips,4);
  }
  return;
 }
 banner(c);

 /* hero card: the active device and its controls */
 rrect(MARGIN,300,600,584,24,CARD);
 int cx=MARGIN+300,cy=440;
 clip(MARGIN,300,MARGIN+600,884);icon_c(I_GLOW,cx,cy,col,l!=LINK_IDLE?95:30);clip(0,0,WIDTH,HEIGHT);
 disc(cx,cy,96,mixc(CARD,col,60),255);disc(cx,cy,96,col,255);
 disc(cx,cy,92,l!=LINK_IDLE?mixc(CARD,col,52):CARD2,255);
 icon_c(I_HERO,cx,cy-2,l!=LINK_IDLE?TEXT:FAINT,255);
 if(l==LINK_CONNECTING||l==LINK_SEARCHING)spinner(cx,cy,120,col,t,10,5);
 int badge=l==LINK_STREAMING?I_CHECK:l==LINK_LOST?I_BANG:l==LINK_FAILED?I_XMARK:l==LINK_OFFLINE?I_BANG:-1;
 if(badge>=0){disc(cx+70,cy+70,30,CARD,255);disc(cx+70,cy+70,26,col,255);icon_c(badge,cx+70,cy+70,BG_TOP,255);}
 const struct ui_device*target=0;for(int i=0;i<n;i++)if(state.devices[i].address==state.selected)target=&state.devices[i];
 if(target&&l!=LINK_IDLE&&l!=LINK_OFFLINE)dev_name(target,buf,sizeof(buf));else if(state.name[0]&&l!=LINK_IDLE&&l!=LINK_OFFLINE)snprintf(buf,sizeof(buf),"%s",state.name);else snprintf(buf,sizeof(buf),"No headphones");
 text_c(F_NAME,cx,600,buf,TEXT,540);
 char sl[64];
 switch(l){
  case LINK_STREAMING:snprintf(sl,sizeof(sl),"Playing");break;
  case LINK_CONNECTING:if(state.attempts>1)snprintf(sl,sizeof(sl),"%s  (attempt %d)",connect_word(),state.attempts);else snprintf(sl,sizeof(sl),"%s",connect_word());break;
  case LINK_SEARCHING:snprintf(sl,sizeof(sl),"Searching");break;
  case LINK_LOST:snprintf(sl,sizeof(sl),"Connection lost");break;
  case LINK_FAILED:snprintf(sl,sizeof(sl),"Can't connect");break;
  case LINK_IDLE:snprintf(sl,sizeof(sl),"Not connected");break;
  default:snprintf(sl,sizeof(sl),"Service offline");break;
 }
 if(testing)snprintf(sl,sizeof(sl),"Playing test tone");
 text_c(F_BODY,cx,644,sl,l!=LINK_IDLE?col:DIM,540);
 char detail[80];detail[0]=0;
 if(l==LINK_STREAMING)snprintf(detail,sizeof(detail),"%s  -  %s latency",state.tv?"TV on":"TV muted",LAT_NAMES[lat_mode()]);
 else if(l==LINK_LOST||l==LINK_FAILED){if(state.retry_ms>0)snprintf(detail,sizeof(detail),"Retrying in %ds",secs(state.retry_ms));else snprintf(detail,sizeof(detail),"Retrying...");}
 else if(l==LINK_IDLE)snprintf(detail,sizeof(detail),"Pick headphones on the right");
 if(detail[0])text_c(F_SMALL,cx,680,detail,FAINT,540);
 if(l==LINK_STREAMING){
  rrect(cx-150,712,300,10,5,LINE);int fw=300*meter_s/100;if(fw>=10)rrect(cx-150,712,fw,10,5,PSBLUEH);
 }
 icon_c(I_SPEAKER,154,812,DIM,255);
 rrect(190,807,372,10,5,LINE);int vw=372*state.volume/100;if(vw>=10)rrect(190,807,vw,10,5,PSBLUEH);
 disc(190+vw,812,11,TEXT,255);
 snprintf(buf,sizeof(buf),"%d%%",state.volume);text_r(F_BODY,656,820,buf,TEXT);

 /* device list card with Add / Fix actions pinned at the bottom */
 rrect(728,300,1096,584,24,CARD);
 text(F_NAME,760,358,"Your headphones",TEXT);
 if(state.scanning){spinner(1764,350,12,STEEL,t,8,3);text_r(F_SMALL,1740,359,"Scanning",STEEL);}
 int rx=744,rw=1064;    /* n>=1 here: the empty case is handled by draw_home_empty */
 for(int row=0;row<4;row++){
  int i=c->top+row;if(i>=n)break;
  device_row(c,i,rx,384+row*80,rw,c->focus==i);
 }
 if(n>4){snprintf(buf,sizeof(buf),"%d-%d of %d",c->top+1,c->top+4<n?c->top+4:n,n);text_r(F_SMALL,1792,359,buf,FAINT);}
 fill(rx,712,rw,2,LINE);
 struct{int icon;const char*label,*sub;}acts[2]={{I_ADD,"Add headphones","Pair a new device, with tips"},{I_WRENCH,"Fix a problem","No sound, stutter, not listed"}};
 for(int a=0;a<2;a++){
  int y=724+a*80,sel=c->focus==n+a;
  uint32_t rf=focus_panel(rx,y,rw,72,16,sel,CARD2);
  disc(rx+48,y+36,26,mixc(rf,PSBLUE,34),255);
  icon_c(acts[a].icon,rx+48,y+36,sel?TEXT:PSBLUEH,255);
  text(F_NAME,rx+88,y+32,acts[a].label,TEXT);
  text(F_SMALL,rx+88,y+58,acts[a].sub,FAINT);
  if(sel&&c->online){int cw=text_w(F_SMALLB,"Open");icon_c(confirm_icon(c),rx+rw-24-cw-26,y+36,btn_glyph_color(confirm_icon(c)),255);text_r(F_SMALLB,rx+rw-24,y+45,"Open",TEXT);}
 }

 const char*tri="Add headphones";
 char tvlabel[32];snprintf(tvlabel,sizeof(tvlabel),"TV: %s",state.tv?"On":"Muted");
 struct chip chips[6]={
  {confirm_icon(c),0,0,!c->online?"Start service":c->focus<n?(focus_is_connected(c)?"Disconnect":"Connect"):"Open",DIM},
  {I_BTRI,0,0,tri,DIM},
  {I_BSQUARE,0,0,tvlabel,DIM},
  {-1,"OPTIONS",0,"Test sound",l==LINK_STREAMING?DIM:FAINT},
  {-1,"L1","R1","Volume",DIM},
  {cancel_icon(c),0,0,"Close",DIM}};
 if(!c->pinmode)legend(chips,6);
}

/* ======================= Add headphones ======================= */
struct devtype{int icon;const char*name,*note,*tip;};
static const struct devtype TYPES[3]={
 {I_TYPE_EARBUD,"Earbuds","AirPods, buds in a case","Put the earbuds in the case, open the lid, then hold the case button until the light flashes white."},
 {I_TYPE_HP,"Headphones","Over- or on-ear headset","Turn the headset on, then hold the power or Bluetooth button until the light flashes to enter pairing mode."},
 {I_TYPE_SPK,"Speaker","Portable Bluetooth speaker","Turn the speaker on, then press and hold its Bluetooth button until it announces pairing or the light flashes."}};

static void draw_add(const struct ui_ctx*c){
 uint64_t t=g_now/1000;int l=ui_link(c);
 char b1[TBUF],b2[TBUF];
 if(c->add_step==ADD_TYPE){
  text(F_BODY,MARGIN,240,"What are you pairing? Pick the closest match for the right tip.",DIM);
  int cw=536,gap=60;
  for(int i=0;i<3;i++){
   int x=MARGIN+i*(cw+gap),sel=c->focus==i;
   focus_panel(x,300,cw,330,24,sel,CARD);
   icon_c(TYPES[i].icon,x+cw/2,440,sel?TEXT:DIM,255);
   text_c(F_NAME,x+cw/2,560,TYPES[i].name,TEXT,cw-48);
   text_c(F_SMALL,x+cw/2,598,TYPES[i].note,FAINT,cw-48);
  }
  rrect(MARGIN,666,1728,150,20,CARD2);rrect(MARGIN,666,6,150,3,PSBLUE);
  text(F_SMALLB,146,712,"Pairing tip",PSBLUEH);
  int nl=wrap2(F_BODY,TYPES[c->focus%3].tip,1728-100,b1,b2);
  if(nl==1)text(F_BODY,146,756,b1,TEXT);else{text(F_BODY,146,748,b1,TEXT);text(F_BODY,146,786,b2,TEXT);}
  struct chip chips[2]={{confirm_icon(c),0,0,"Start pairing",DIM},{cancel_icon(c),0,0,"Back",DIM}};
  if(!c->pinmode)legend(chips,2);
  return;
 }
 if(c->add_step==ADD_SCAN){
  /* chosen type's tip + live scan status across the top */
  rrect(MARGIN,224,1728,96,20,mixc(CARD,PSBLUE,20));rrect(MARGIN,224,8,96,4,PSBLUE);
  if(state.scanning||l==LINK_SEARCHING)spinner(146,272,18,STEEL,t,8,4);else icon_c(TYPES[c->add_type%3].icon,146,272,STEEL,255);
  text(F_SMALLB,192,258,state.scanning?"Searching for your device...":"Devices found",PSBLUEH);
  wrap2(F_SMALL,TYPES[c->add_type%3].tip,1728-130-40,b1,b2);text(F_SMALL,192,292,b1,DIM);
  rrect(MARGIN,340,1728,544,24,CARD);
  int n=state.count,rx=MARGIN+16,rw=1728-32;
  if(n==0){
   if(state.scanning||l==LINK_SEARCHING)spinner(960,560,26,STEEL,t,10,6);else icon_c(I_HERO,960,540,LINE,255);
   text_c(F_NAME,960,640,state.scanning?"Looking for devices...":"No devices found yet",DIM,1200);
   text_c(F_SMALL,960,680,"Keep the device in pairing mode. Press Triangle to scan again.",FAINT,1200);
  }
  for(int row=0;row<5;row++){int i=c->top+row;if(i>=n)break;device_row(c,i,rx,372+row*96,rw,c->focus==i);}
  struct chip chips[3]={{confirm_icon(c),0,0,"Connect",DIM},{I_BTRI,0,0,"Scan again",DIM},{cancel_icon(c),0,0,"Back",DIM}};
  if(!c->pinmode)legend(chips,3);
  return;
 }
 /* ADD_TEST: confirm the user can hear the tone */
 int cx=960,cy=470;uint32_t tc=l==LINK_STREAMING?PSBLUEH:STEEL;
 icon_c(I_GLOW,cx,cy,tc,90);
 disc(cx,cy,96,tc,255);disc(cx,cy,92,mixc(CARD,tc,52),255);
 icon_c(I_SPEAKER,cx,cy,TEXT,255);
 disc(cx+70,cy+70,30,CARD,255);disc(cx+70,cy+70,26,tc,255);icon_c(I_CHECK,cx+70,cy+70,BG_TOP,255);
 text_c(F_TITLE,cx,660,"Can you hear the test tone?",TEXT,1500);
 char line[160];snprintf(line,sizeof(line),state.name[0]?"Playing a short tone on %s.":"Playing a short tone on your headphones.",state.name);
 text_c(F_BODY,cx,712,line,DIM,1400);
 text_c(F_SMALL,cx,752,"If it's silent, choose No to get help.",FAINT,1400);
 struct chip chips[3]={{confirm_icon(c),0,0,"Yes, all set",DIM},{I_BTRI,0,0,"Play again",DIM},{cancel_icon(c),0,0,"No, get help",DIM}};
 if(!c->pinmode)legend(chips,3);
}

/* ======================= Fix a problem ======================= */
struct problem{int icon;const char*title,*sub,*action;const char*help;};
static const struct problem PROBLEMS[4]={
 {I_SPEAKER,"No sound in the headphones","Connected but silent","Play test tone",
  "Press the confirm button to play a test tone. If it stays silent, the headset may have connected in a silent mode: disconnect and connect again, or forget it on the headset and pair fresh."},
 {I_BANG,"Sound is choppy or cutting out","Stutters, especially online","Show what helps",
  "Wi-Fi shares the PS4's antenna with Bluetooth: use a wired LAN cable or 5 GHz Wi-Fi, keep the headset close and in sight. The button here steps the latency mode up - Most stable adds a little delay for far fewer dropouts."},
 {I_ROWHP,"My headphones aren't listed","They never show up in the scan","Scan again",
  "Put the headset into pairing mode (not just powered on), keep it close, and scan again. A headset still connected to a phone won't appear - turn Bluetooth off on the phone first."},
 {I_PAD,"The controller feels laggy","Input delay while streaming","Why this happens",
  "The controller and the headset share one radio. The service already keeps controller slots free; if lag persists, lower on-screen Bluetooth load by disconnecting other wireless devices."}};

/* The choppy-sound row (index 1) doubles as the latency control: its subtitle
 * shows the current mode and its action steps to the next, more stable one. */
static const char*fix_sub(int i){
 static char s[48];
 if(i==1){snprintf(s,sizeof(s),"Now: %s latency",LAT_NAMES[lat_mode()]);return s;}
 return PROBLEMS[i].sub;
}
static const char*fix_action(int i){
 static char s[32];
 if(i==1){snprintf(s,sizeof(s),"Use %s",LAT_NAMES[(lat_mode()+1)%3]);return s;}
 return PROBLEMS[i].action;
}
static void draw_fix(const struct ui_ctx*c){
 text(F_BODY,MARGIN,240,"Pick what's happening. Each one has a single step to try.",DIM);
 for(int i=0;i<4;i++){
  int y=300+i*92,sel=c->focus==i;
  uint32_t rf=focus_panel(MARGIN,y,1728,80,16,sel,CARD);
  disc(MARGIN+48,y+40,28,mixc(rf,PSBLUE,30),255);
  icon_c(PROBLEMS[i].icon,MARGIN+48,y+40,sel?TEXT:PSBLUEH,255);
  text(F_NAME,MARGIN+92,y+36,PROBLEMS[i].title,TEXT);
  text(F_SMALL,MARGIN+92,y+62,fix_sub(i),FAINT);
  const char*act=fix_action(i);int cw=text_w(F_SMALLB,act);
  if(sel){icon_c(confirm_icon(c),1824-24-cw-26,y+40,btn_glyph_color(confirm_icon(c)),255);text_r(F_SMALLB,1824-24,y+49,act,TEXT);}
  else text_r(F_SMALL,1824-24,y+49,act,SUBTLE);
 }
 /* guidance for the focused problem, plus the service's own last words */
 int fi=c->focus%4;char b1[TBUF],b2[TBUF];
 rrect(MARGIN,684,1728,200,20,CARD2);
 text(F_SMALLB,MARGIN+40,728,PROBLEMS[fi].title,PSBLUEH);
 int nl=wrap2(F_BODY,PROBLEMS[fi].help,1728-80,b1,b2);
 text(F_BODY,MARGIN+40,772,b1,TEXT);if(nl>1)text(F_BODY,MARGIN+40,810,b2,TEXT);
 char last[TBUF];
 if(fi==1&&(state.drops||state.underruns))snprintf(last,sizeof(last),"Recent: %d drops, %d underruns.",state.drops,state.underruns);
 else if(state.message[0])snprintf(last,sizeof(last),"Service says: %s",state.message);
 else snprintf(last,sizeof(last),"Service says: nothing to report.");
 char lb[TBUF];text(F_SMALL,MARGIN+40,852,fit(F_SMALL,last,1728-80,lb),FAINT);
 struct chip chips[2]={{confirm_icon(c),0,0,fix_action(fi),DIM},{cancel_icon(c),0,0,"Back",DIM}};
 if(!c->pinmode)legend(chips,2);
}

/* ---- PIN overlay (modal over any screen) ---- */
static void draw_pin(const struct ui_ctx*c){
 dim_frame(150);
 rrect(540,330,840,420,24,0x101826);rrect(540,330,840,6,3,PSBLUE);
 text_c(F_TITLE,960,416,"Pairing PIN",TEXT,780);
 text_c(F_BODY,960,464,"These headphones ask for a PIN. Try 0000 or 1234.",DIM,780);
 for(int i=0;i<4;i++){
  int bx=732+i*120;rrect(bx,504,96,104,16,i==c->digit?PSCYAN:CARD2);
  char tch[2]={c->pin[i],0};text_c(F_TITLE,bx+48,578,tch,i==c->digit?BG_TOP:TEXT,80);
 }
 text_c(F_SMALL,960,676,"Left/Right pick a digit      Up/Down change it",FAINT,790);
 struct chip chips[2]={{confirm_icon(c),0,0,"Submit",DIM},{cancel_icon(c),0,0,"Cancel",DIM}};
 legend(chips,2);
}

/* ---- top-level draw ---- */
static const char*SUBTITLES[3]={"PS4 game audio on your wireless headphones","Add headphones","Fix a problem"};
static void draw(const struct ui_ctx*c){
 clip(0,0,WIDTH,HEIGHT);gradient(BG_TOP,BG_BOT);
 /* PlayStation three-surface channel: the button-legend sits on a cobalt bar
  * along the bottom, the console translation of the site's deep-blue footer. */
 fill(0,920,WIDTH,HEIGHT-920,FOOTER);fill(0,920,WIDTH,2,PSBLUE);
 int sc=c->screen;if(sc<0||sc>2)sc=0;
 header(c,SUBTITLES[sc]);
 if(sc==SCREEN_ADD)draw_add(c);
 else if(sc==SCREEN_FIX)draw_fix(c);
 else draw_home(c);
 if(c->pinmode)draw_pin(c);
}
