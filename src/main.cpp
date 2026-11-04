/*
 * AEROCyd 1.0  -  Windows Phone x Windows Aero style "OS" for ESP32-2432S028R (CYD)
 *
 * Display : ILI9341 320x240 (landscape)      Touch : XPT2046 (own SPI bus)
 * Render  : two 320x120 bands through one 16-bit sprite (a full 320x240 sprite
 *           does not fit in ESP32 heap without PSRAM)
 *
 * Gestures
 *   lock screen : swipe up to unlock
 *   status bar  : swipe down  -> notification shade
 *   tiles       : drag left/right (2 pages), tap to open
 *   nav bar     : [back] [home] [search -> terminal]
 *
 * Terminal commands: help | poweroff | reboot | dreammode hard|easy
 *   dreammode hard = full Aero effects (animated wallpaper, light rays, bubbles, flip tiles)
 *   dreammode easy = light mode (static wallpaper, fewer redraws, no tile flips)
 */
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <esp_sleep.h>

#define VERSION "1.0"
#define CYD_INVERT 0          // 1 = invert panel colours

// ---------- hardware ----------
#define PIN_BL   21
#define T_CS     33
#define T_IRQ    36
#define T_CLK    25
#define T_MOSI   32
#define T_MISO   39
// raw touch calibration (rotation 1)
#define TX_MIN   200
#define TX_MAX   3700
#define TY_MIN   240
#define TY_MAX   3800

// ---------- layout ----------
#define SW 320
#define SH 240
#define BH 120
#define STATUS_H 18
#define NAV_Y 214
#define NAV_H 26
#define SHADE_MAX 156
#define PIf 3.14159265f

#define RGB(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))

// ---------- small helpers ----------
static inline int imin(int a,int b){return a<b?a:b;}
static inline int imax(int a,int b){return a>b?a:b;}
static inline int clampi(int v,int a,int b){return v<a?a:(v>b?b:v);}
static inline float clampf(float v,float a,float b){return v<a?a:(v>b?b:v);}
static inline float lerpf(float a,float b,float t){return a+(b-a)*t;}
static float easeOut(float t){t=clampf(t,0,1);float u=1-t;return 1-u*u*u;}
static float easeInOut(float t){t=clampf(t,0,1);return t<0.5f?4*t*t*t:1-powf(-2*t+2,3)/2;}

// ---------- backlight ----------
static void blInit(){
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BL,5000,8);
#else
  ledcSetup(0,5000,8); ledcAttachPin(PIN_BL,0);
#endif
}
static void blSet(uint8_t v){
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_BL,v);
#else
  ledcWrite(0,v);
#endif
}

// ---------- settings ----------
struct Settings{ uint8_t bright=200; uint8_t wall=0; uint8_t accent=0; bool hard=true; } cfg;
static Preferences prefs;
static bool cfgDirty=false;
static void loadCfg(){
  prefs.begin("aerocyd",false);
  cfg.bright=prefs.getUChar("br",200); cfg.wall=prefs.getUChar("wall",0)&3;
  cfg.accent=prefs.getUChar("acc",0)&7; cfg.hard=prefs.getBool("hard",true);
}
static void saveCfg(){
  prefs.putUChar("br",cfg.bright); prefs.putUChar("wall",cfg.wall);
  prefs.putUChar("acc",cfg.accent); prefs.putBool("hard",cfg.hard); cfgDirty=false;
}

// ---------- time (starts at compile time, no RTC on CYD) ----------
static time_t baseT=0;
static void initTime(){
  char mon[4]; int d=1,y=2026,hh=0,mm=0,ss=0;
  sscanf(__DATE__,"%3s %d %d",mon,&d,&y); sscanf(__TIME__,"%d:%d:%d",&hh,&mm,&ss);
  const char* M="JanFebMarAprMayJunJulAugSepOctNovDec"; const char* p=strstr(M,mon);
  struct tm t; memset(&t,0,sizeof t);
  t.tm_year=y-1900; t.tm_mon=p?(int)(p-M)/3:0; t.tm_mday=d; t.tm_hour=hh; t.tm_min=mm; t.tm_sec=ss;
  baseT=mktime(&t);
}
static struct tm nowTm(){ time_t t=baseT+(time_t)(millis()/1000); struct tm r; gmtime_r(&t,&r); return r; }
static const char* WDS[7]={"sun","mon","tue","wed","thu","fri","sat"};
static const char* WDF[7]={"sunday","monday","tuesday","wednesday","thursday","friday","saturday"};
static const char* MON[12]={"january","february","march","april","may","june","july","august","september","october","november","december"};

// ---------- palette / themes ----------
static const uint16_t ACC[8]={RGB(27,161,226),RGB(216,0,115),RGB(164,196,0),RGB(250,104,0),
                              RGB(0,138,0),RGB(229,20,0),RGB(106,0,255),RGB(0,171,169)};
struct Theme{int tr,tg,tb,br,bg,bb;};
static const Theme THEMES[4]={
  {70,180,235,  8,45,125},    // aero blue
  {120,220,150, 6,80,80},     // aero green
  {255,160,100, 80,25,120},   // aero sunset
  {40,70,150,   4,8,35}};     // aero night
static uint16_t rowCol[SH];
static uint16_t themeCol(int th,float f){
  f=clampf(f,0,1); f=f*f*(3-2*f)*0.35f+f*0.65f; const Theme&t=THEMES[th];
  return RGB((int)(t.tr+(t.br-t.tr)*f),(int)(t.tg+(t.bg-t.tg)*f),(int)(t.tb+(t.bb-t.tb)*f));
}
static void buildWall(){ for(int y=0;y<SH;y++) rowCol[y]=themeCol(cfg.wall,y/(float)(SH-1)); }

// ---------- graphics layer (band renderer) ----------
static TFT_eSPI tft;
static TFT_eSprite spr(&tft);
static uint16_t* BUF;
static int bandY=0;
static inline uint16_t sw16(uint16_t c){return (c>>8)|(c<<8);}
static inline uint16_t blend(uint16_t fg,uint16_t bg,int a){ // a: 0..32
  if(a<=0) return bg; if(a>32) a=32;
  uint32_t b=(bg|((uint32_t)bg<<16))&0x07E0F81F, f=(fg|((uint32_t)fg<<16))&0x07E0F81F;
  uint32_t r=((((f-b)*a)>>5)+b)&0x07E0F81F; return (uint16_t)((r>>16)|r);
}
static inline bool vis(int y,int h){return y+h>bandY && y<bandY+BH;}
static void gFill(int x,int y,int w,int h,uint16_t c){ if(w>0&&h>0&&vis(y,h)) spr.fillRect(x,y-bandY,w,h,c); }
static void gRRect(int x,int y,int w,int h,int r,uint16_t c){ if(vis(y,h)) spr.fillRoundRect(x,y-bandY,w,h,r,c); }
static void gRectO(int x,int y,int w,int h,uint16_t c){ if(vis(y,h)) spr.drawRect(x,y-bandY,w,h,c); }
static void gHLine(int x,int y,int w,uint16_t c){ if(y>=bandY&&y<bandY+BH) spr.drawFastHLine(x,y-bandY,w,c); }
static void gLine(int x0,int y0,int x1,int y1,uint16_t c){ if(vis(imin(y0,y1),abs(y1-y0)+1)) spr.drawLine(x0,y0-bandY,x1,y1-bandY,c); }
static void gLineT(int x0,int y0,int x1,int y1,uint16_t c){ gLine(x0,y0,x1,y1,c); gLine(x0+1,y0,x1+1,y1,c); gLine(x0,y0+1,x1,y1+1,c); }
static void gCirc(int x,int y,int r,uint16_t c){ if(vis(y-r,2*r+1)) spr.fillCircle(x,y-bandY,r,c); }
static void gCircO(int x,int y,int r,uint16_t c){ if(vis(y-r,2*r+1)) spr.drawCircle(x,y-bandY,r,c); }
static void gTri(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c){
  if(vis(imin(y0,imin(y1,y2)),imax(y0,imax(y1,y2))-imin(y0,imin(y1,y2))+1)) spr.fillTriangle(x0,y0-bandY,x1,y1-bandY,x2,y2-bandY,c); }
static void gText(const char*s,int x,int y,uint8_t font,uint16_t col,uint8_t datum=TL_DATUM,uint8_t size=1){
  if(y>bandY+BH+80||y<bandY-80) return;
  spr.setTextSize(size); spr.setTextDatum(datum); spr.setTextColor(col);
  spr.drawString(s,x,y-bandY,font); spr.setTextSize(1);
}
static void gTextS(const char*s,int x,int y,uint8_t font,uint16_t col,uint8_t datum=TL_DATUM){
  gText(s,x+1,y+1,font,RGB(0,25,60),datum); gText(s,x,y,font,col,datum);
}
static int textW(const char*s,uint8_t f){ spr.setTextSize(1); return spr.textWidth(s,f); }
static void gBlend(int x,int y,int w,int h,uint16_t col,int a){
  if(a<=0) return;
  int x0=imax(x,0),x1=imin(x+w,SW),y0=imax(y,bandY),y1=imin(y+h,bandY+BH);
  for(int yy=y0;yy<y1;yy++){ uint16_t*p=BUF+(yy-bandY)*SW; for(int xx=x0;xx<x1;xx++) p[xx]=sw16(blend(col,sw16(p[xx]),a)); }
}
static void gGlow(int cx,int cy,int r,uint16_t col,int amax){
  int r2=r*r, ys=imax(cy-r,bandY), ye=imin(cy+r,bandY+BH-1), xs=imax(cx-r,0), xe=imin(cx+r,SW-1);
  for(int y=ys;y<=ye;y++){ int dy=y-cy; uint16_t*row=BUF+(y-bandY)*SW;
    for(int x=xs;x<=xe;x++){ int dx=x-cx, d2=dx*dx+dy*dy; if(d2>=r2) continue;
      int t=(r2-d2)*32/r2; int a=t*t*amax/1024; if(a<=0) continue; row[x]=sw16(blend(col,sw16(row[x]),a)); } }
}
static void gBeam(int x0,int slope16,int w0,int amax){ // diagonal soft light ray
  for(int y=bandY;y<bandY+BH;y++){
    int wd=w0+y/5, xl=x0+y*slope16/16, c=xl+wd/2, a=amax*(SH-y)/SH; if(a<=0||wd<4) continue;
    uint16_t*row=BUF+(y-bandY)*SW; int half=wd/2;
    for(int x=imax(xl,0);x<imin(xl+wd,SW);x++){ int p=(half-abs(x-c))*32/half; int aa=a*p/32; if(aa<=0)continue;
      row[x]=sw16(blend(0xFFFF,sw16(row[x]),aa)); } }
}
static void gRibbon(float base,float amp,float freq,float ph,int th,int amax){
  int half=th/2;
  for(int x=0;x<SW;x++){ int yc=(int)(base+amp*sinf(x*freq+ph));
    for(int y=imax(yc-half,bandY);y<=imin(yc+half,bandY+BH-1);y++){ int a=amax*(half-abs(y-yc))/half; if(a<=0)continue;
      uint16_t*p=BUF+(y-bandY)*SW+x; *p=sw16(blend(0xFFFF,sw16(*p),a)); } }
}

// ---------- state ----------
enum Scr{S_LOCK,S_HOME,S_APP};
enum { A_NONE=-1,A_CLOCK,A_TERM,A_SET,A_PAINT,A_ABOUT,A_TORCH,Q_DREAM,Q_WALL,Q_ACCENT,Q_LOCK,Q_REBOOT,Q_OFF };
static Scr scr=S_LOCK; static int app=A_NONE;
static float homeIn=0,homeInT=0,lockY=0,lockYT=0,scrollX=0,scrollT=0;
static float shadeH=0,shadeTarget=0; static bool shadeDrag=false;
static float appWipe=0, navRip=-1; static int navRipBtn=0, navPressed=-1;
static bool anim=false,dirty=true;
static uint8_t dimA=0; static const char* msgTxt=nullptr; static float crtT=0;
static int pendingAct=0; static uint32_t pendAt=0;
struct Tr{bool act=false,opening=true;float t=0;int x=0,y=0,w=0,h=0;uint16_t col=0;int appId=0;} tr;
static int lastTile=-1; static int lrx=0,lry=0,lrw=0,lrh=0; static uint16_t lcol=0;

// ---------- bubbles ----------
struct Bub{float x,y,r,v;} bub[9];
static void initBubbles(){ for(auto&b:bub){ b.x=random(SW); b.y=random(SH); b.r=6+random(22); b.v=4+random(10);} }
static void updBubbles(float dt){ for(auto&b:bub){ b.y-=b.v*dt; if(b.y<-b.r){b.y=SH+b.r; b.x=random(SW);} } }

// ---------- wallpaper ----------
static void drawWallpaper(float t,float par){
  for(int y=bandY;y<bandY+BH;y++) spr.drawFastHLine(0,y-bandY,SW,rowCol[y]);
  float ox=-par*0.15f;
  if(!cfg.hard){ gGlow(250,50,110,0xFFFF,9); gBeam(60,5,30,7); return; }
  gGlow((int)(250+sinf(t*0.3f)*20+ox),(int)(50+cosf(t*0.25f)*10),120,0xFFFF,12);
  gBeam(30+(int)(sinf(t*0.2f)*14+ox),5,34,10);
  gBeam(110+(int)(cosf(t*0.17f)*18+ox),6,22,8);
  gBeam(210+(int)(sinf(t*0.23f)*12+ox),5,40,7);
  gRibbon(172,18,0.020f,t*0.6f+ox*0.02f,34,6);
  gRibbon(192,14,0.027f,-t*0.5f+1.7f,26,5);
  for(auto&b:bub){ if(b.y<bandY-30||b.y>bandY+BH+30) continue; int y=(int)b.y; int x=(int)(b.x+ox*0.5f);
    uint16_t base=rowCol[clampi(y,0,SH-1)];
    gCirc(x,y,(int)b.r,blend(0xFFFF,base,3)); gCircO(x,y,(int)b.r,blend(0xFFFF,base,12)); }
}

// ---------- tiles ----------
struct Tile{int8_t col,row,w,h;uint8_t ci;int8_t id;const char*name;};
static const Tile TILES[]={
 {0,0,2,2,0,A_CLOCK,"clock"},   {0,2,2,1,1,A_TERM,"terminal"},
 {2,0,1,1,2,A_SET,"settings"},  {3,0,1,1,3,A_PAINT,"paint"},
 {2,1,2,1,4,A_ABOUT,"about"},   {2,2,1,1,5,A_TORCH,"torch"},   {3,2,1,1,6,Q_DREAM,"dream"},
 {4,0,2,2,7,Q_WALL,"wallpaper"},{6,0,1,1,1,Q_ACCENT,"accent"}, {7,0,1,1,2,Q_LOCK,"lock"},
 {6,1,1,1,3,Q_REBOOT,"reboot"}, {7,1,1,1,5,Q_OFF,"power"}};
#define NTILES (int)(sizeof(TILES)/sizeof(TILES[0]))
static int pressedTile=-1;
static uint16_t tileColor(int i){ return ACC[(cfg.accent+TILES[i].ci)&7]; }
static void tileRect(int i,int&x,int&y,int&w,int&h){
  const Tile&t=TILES[i]; x=8+t.col*78-(int)scrollX; y=24+t.row*64; w=t.w*74+(t.w-1)*4; h=t.h*60+(t.h-1)*4; }
static int hitTile(int px,int py){
  for(int i=0;i<NTILES;i++){ int x,y,w,h; tileRect(i,x,y,w,h); if(px>=x&&px<x+w&&py>=y&&py<y+h) return i; } return -1; }

static void drawIcon(int id,int cx,int cy,uint16_t bg){
  const uint16_t W=0xFFFF;
  switch(id){
    case A_TERM: gRectO(cx-12,cy-9,24,18,W); gRectO(cx-11,cy-8,22,16,W);
      gLine(cx-7,cy-3,cx-3,cy,W); gLine(cx-3,cy,cx-7,cy+3,W); gHLine(cx,cy+4,7,W); break;
    case A_SET: gCircO(cx,cy,7,W); gCircO(cx,cy,6,W); gCirc(cx,cy,2,W);
      for(int k=0;k<8;k++){ float a=k*PIf/4; gLineT(cx+(int)(cosf(a)*9),cy+(int)(sinf(a)*9),cx+(int)(cosf(a)*12),cy+(int)(sinf(a)*12),W);} break;
    case A_PAINT: gCirc(cx-7,cy+7,4,W); gLineT(cx-5,cy+5,cx+9,cy-9,W); gLineT(cx-4,cy+5,cx+10,cy-8,W); break;
    case A_ABOUT: gCircO(cx,cy,11,W); gCircO(cx,cy,10,W); gFill(cx-1,cy-1,3,8,W); gFill(cx-1,cy-6,3,3,W); break;
    case A_TORCH: gCirc(cx,cy-3,6,W); gFill(cx-3,cy+3,7,5,W);
      gLine(cx,cy-13,cx,cy-11,W); gLine(cx-10,cy-8,cx-8,cy-6,W); gLine(cx+10,cy-8,cx+8,cy-6,W); break;
    case Q_DREAM: gCirc(cx,cy,10,W); gCirc(cx+5,cy-4,9,bg); break;
    case Q_ACCENT: gCirc(cx-5,cy+3,7,W); gCircO(cx+5,cy+3,7,W); gCircO(cx,cy-5,7,W); break;
    case Q_LOCK: gCircO(cx,cy-4,6,W); gCircO(cx,cy-4,5,W); gRRect(cx-9,cy-2,18,13,2,W); gFill(cx-1,cy+3,3,4,bg); break;
    case Q_REBOOT: gCircO(cx,cy,9,W); gCircO(cx,cy,8,W); gFill(cx+2,cy-11,10,9,bg); gTri(cx+1,cy-14,cx+1,cy-6,cx+9,cy-10,W); break;
    case Q_OFF: gCircO(cx,cy+1,9,W); gCircO(cx,cy+1,8,W); gFill(cx-4,cy-12,9,9,bg); gFill(cx-1,cy-12,3,12,W); break;
  }
}

static void liveFace(float ts,int i,int&face,float&s){
  float u=fmodf(ts+i*1.3f,8.f); s=1; face=0;
  if(u<0.3f){face=0;s=u/0.3f;} else if(u<3.7f){face=0;}
  else if(u<4.0f){face=0;s=1-(u-3.7f)/0.3f;} else if(u<4.3f){face=1;s=(u-4.0f)/0.3f;}
  else if(u<7.7f){face=1;} else {face=1;s=1-(u-7.7f)/0.3f;}
}

static void drawTileContent(int i,int x,int y,int w,int h,uint16_t col,int face){
  const Tile&t=TILES[i]; const uint16_t W=0xFFFF; char b[40];
  if(t.id==A_CLOCK){
    if(face==0){ struct tm tm=nowTm(); snprintf(b,sizeof b,"%02d:%02d",tm.tm_hour,tm.tm_min);
      int f=textW(b,6)>w-8?4:6; gText(b,x+w/2,y+h/2-12,f,W,MC_DATUM);
      snprintf(b,sizeof b,"%s %d %s",WDS[tm.tm_wday],tm.tm_mday,MON[tm.tm_mon]); gText(b,x+w/2,y+h-20,2,W,MC_DATUM);
    } else { gText("AEROCyd",x+w/2,y+h/2-10,4,W,MC_DATUM); gText("v" VERSION,x+w/2,y+h/2+16,2,W,MC_DATUM); }
    return; }
  if(t.id==A_ABOUT && face==1){
    snprintf(b,sizeof b,"free ram %u kb",(unsigned)(ESP.getFreeHeap()/1024)); gText(b,x+w/2,y+h/2-8,2,W,MC_DATUM);
    snprintf(b,sizeof b,"up %lus",(unsigned long)(millis()/1000)); gText(b,x+w/2,y+h/2+10,2,W,MC_DATUM); return; }
  if(t.id==Q_WALL){
    for(int r=0;r<h-26;r++) gHLine(x+6,y+6+r,w-12,themeCol(cfg.wall,r/(float)(h-27)));
    gRectO(x+6,y+6,w-12,h-26+0,W);
    static const char* TN[4]={"aero blue","aero green","aero sunset","aero night"};
    gText(TN[cfg.wall],x+6,y+h-18,2,W); return; }
  int cx=x+(w>100?30:w/2), cy=y+h/2-9; if(h>100) cy=y+h/2-12;
  drawIcon(t.id,cx,cy,col);
  gText(t.name,x+5,y+h-19,2,W);
  if(t.id==Q_DREAM) gText(cfg.hard?"HARD":"easy",x+w-4,y+4,1,W,TR_DATUM);
}

static void drawTiles(float ts){
  for(int i=0;i<NTILES;i++){
    int x,y,w,h; tileRect(i,x,y,w,h); if(x>=SW||x+w<=0) continue;
    const Tile&t=TILES[i];
    float d=(t.col&3)*0.12f+t.row*0.1f, p=clampf(homeIn*1.6f-d,0,1); if(p<=0.01f) continue;
    float e=easeOut(p);
    int px=x+(int)((1-e)*90), pw=(int)(w*(0.55f+0.45f*e)), py=y, ph=h;
    if(i==pressedTile){px+=3;py+=3;pw-=6;ph-=6;}
    int face=0; float s=1;
    if((t.id==A_CLOCK||t.id==A_ABOUT)&&cfg.hard&&e>0.99f){ liveFace(ts,i,face,s); s=easeInOut(s); }
    if(s<0.999f){ int hh=imax(2,(int)(ph*s)); py+=(ph-hh)/2; ph=hh; }
    uint16_t col=tileColor(i);
    gFill(px,py,pw,ph,col); gBlend(px,py,pw,ph/2,0xFFFF,3); gHLine(px,py,pw,blend(0xFFFF,col,10));
    if(e>0.75f&&s>0.95f) drawTileContent(i,px,py,pw,ph,col,face);
  }
}

// ---------- status / nav ----------
static void drawStatus(){
  gBlend(0,0,SW,STATUS_H,0,10);
  uint16_t c=(scr==S_APP&&app==A_TORCH)?0:0xFFFF;
  gText("AEROCyd",6,5,1,c);
  struct tm tm=nowTm(); char b[8]; snprintf(b,sizeof b,"%02d:%02d",tm.tm_hour,tm.tm_min);
  gText(b,SW-6,1,2,c,TR_DATUM);
  if(scr==S_HOME){ int pg=scrollX>156?1:0; for(int k=0;k<2;k++) gCirc(SW/2-6+k*12,9,k==pg?3:2,k==pg?c:blend(c,0x0000,14)); }
}
static void drawNav(){
  int off=(scr==S_APP)?0:(int)((1.f-homeIn)*NAV_H); if(off>=NAV_H) return;
  int y=NAV_Y+off;
  gFill(0,y,SW,NAV_H,RGB(6,10,22)); gFill(0,y,SW,NAV_H/2,RGB(22,34,60));
  gHLine(0,y,SW,RGB(90,150,220)); gHLine(0,y+1,SW,RGB(40,70,120));
  int cy=y+NAV_H/2+1; uint16_t ac=ACC[cfg.accent];
  for(int b=0;b<3;b++){
    int cx=53+b*107; bool p=(navPressed==b); uint16_t c=p?ac:0xFFFF;
    if(p) gCirc(cx,cy,12,RGB(38,66,112));
    if(navRip>=0&&navRipBtn==b) gCircO(cx,cy,(int)(12*easeOut(navRip)),blend(0xFFFF,RGB(14,22,40),(int)(26*(1-navRip))));
    if(b==0){ gLineT(cx+8,cy,cx-8,cy,c); gLineT(cx-8,cy,cx-1,cy-7,c); gLineT(cx-8,cy,cx-1,cy+7,c); }
    else if(b==1){
      uint16_t a1=p?c:RGB(240,80,40),a2=p?c:RGB(130,200,40),a3=p?c:RGB(30,140,230),a4=p?c:RGB(250,200,30);
      gFill(cx-9,cy-8,8,7,a1); gFill(cx+1,cy-8,8,7,a2); gFill(cx-9,cy,8,7,a3); gFill(cx+1,cy,8,7,a4); }
    else { gCircO(cx-2,cy-2,6,c); gCircO(cx-2,cy-2,5,c); gLineT(cx+2,cy+2,cx+8,cy+8,c); }
  }
}

// ---------- lock ----------
static void drawLock(float ts){
  int oy=(int)lockY; struct tm tm=nowTm(); char b[40];
  snprintf(b,sizeof b,"%02d:%02d",tm.tm_hour,tm.tm_min); gTextS(b,16,96+oy,6,0xFFFF);
  snprintf(b,sizeof b,"%s, %s %d",WDF[tm.tm_wday],MON[tm.tm_mon],tm.tm_mday); gTextS(b,18,152+oy,4,0xFFFF);
  gTextS("slide up to unlock",SW/2,196+oy,2,0xFFFF,MC_DATUM);
  float ph=fmodf(ts*1.1f,1.f);
  for(int i=0;i<3;i++){ float p=fmodf(ph+i*0.18f,1.f); int y=226+oy-(int)(p*16); int a=(int)(32*sinf(p*PIf));
    uint16_t c=blend(0xFFFF,rowCol[clampi(y,0,SH-1)],a);
    gLineT(SW/2-9,y+5,SW/2,y-3,c); gLineT(SW/2+9,y+5,SW/2,y-3,c); }
}

// ---------- apps ----------
static void dimBg(int a=14){ gBlend(0,22,SW,NAV_Y-22,0,a); }

// --- clock
static void hand(int cx,int cy,float deg,int len,uint16_t c){
  float a=deg*PIf/180.f; int x=cx+(int)(sinf(a)*len), y=cy-(int)(cosf(a)*len);
  gLine(cx,cy,x,y,c); gLine(cx+1,cy,x+1,y,c); gLine(cx,cy+1,x,y+1,c);
}
static void appClock(){
  dimBg(); struct tm tm=nowTm(); float sec=tm.tm_sec+(millis()%1000)/1000.f; uint16_t ac=ACC[cfg.accent];
  int cx=90,cy=118,R=76;
  gCirc(cx,cy,R,RGB(8,18,38)); gCircO(cx,cy,R,0xFFFF); gCircO(cx,cy,R-1,ac); gCircO(cx,cy,R-2,ac);
  gBlend(cx-R+6,cy-R+4,2*R-12,R-6,0xFFFF,3);
  for(int i=0;i<12;i++){ float a=i*PIf/6; int r1=(i%3==0)?R-14:R-9;
    gLine(cx+(int)(sinf(a)*r1),cy-(int)(cosf(a)*r1),cx+(int)(sinf(a)*(R-4)),cy-(int)(cosf(a)*(R-4)),0xFFFF); }
  hand(cx,cy,(tm.tm_hour%12+tm.tm_min/60.f)*30.f,38,0xFFFF);
  hand(cx,cy,(tm.tm_min+sec/60.f)*6.f,58,0xFFFF);
  float a=sec*6.f*PIf/180.f; gLine(cx,cy,cx+(int)(sinf(a)*66),cy-(int)(cosf(a)*66),RGB(255,60,40)); gCirc(cx,cy,4,RGB(255,60,40));
  char b[40]; snprintf(b,sizeof b,"%02d:%02d",tm.tm_hour,tm.tm_min);
  int f=textW(b,6)>150?4:6; gTextS(b,250,100,f,0xFFFF,MC_DATUM);
  snprintf(b,sizeof b,":%02d",tm.tm_sec); gTextS(b,250,138,4,ac,MC_DATUM);
  snprintf(b,sizeof b,"%s %d %s",WDS[tm.tm_wday],tm.tm_mday,MON[tm.tm_mon]); gTextS(b,250,170,2,0xFFFF,MC_DATUM);
}

// --- torch
static void appTorch(){ gFill(0,0,SW,NAV_Y,0xFFFF); }

// --- about
static void appAbout(){
  dimBg(); gText("about",10,22,4,0xFFFF); char b[64]; int y=54; const int P=19;
  gText("AEROCyd v" VERSION,10,y,2,ACC[cfg.accent]); y+=P;
  snprintf(b,sizeof b,"cpu: %s r%d, %d core, %d mhz",ESP.getChipModel(),(int)ESP.getChipRevision(),(int)ESP.getChipCores(),(int)ESP.getCpuFreqMHz()); gText(b,10,y,2,0xFFFF); y+=P;
  snprintf(b,sizeof b,"flash: %u mb",(unsigned)(ESP.getFlashChipSize()/1048576)); gText(b,10,y,2,0xFFFF); y+=P;
  snprintf(b,sizeof b,"ram: %u kb free, block %u kb",(unsigned)(ESP.getFreeHeap()/1024),(unsigned)(ESP.getMaxAllocHeap()/1024)); gText(b,10,y,2,0xFFFF); y+=P;
  gText("display: ili9341 320x240",10,y,2,0xFFFF); y+=P;
  gText("touch: xpt2046 resistive",10,y,2,0xFFFF); y+=P;
  unsigned long s=millis()/1000; snprintf(b,sizeof b,"uptime: %02lu:%02lu:%02lu",s/3600,(s/60)%60,s%60); gText(b,10,y,2,0xFFFF); y+=P;
  snprintf(b,sizeof b,"dreammode: %s",cfg.hard?"hard":"easy"); gText(b,10,y,2,0xFFFF);
}

// --- settings
static void setBright(int x){ cfg.bright=(uint8_t)clampi(20+(x-20)*235/280,20,255); blSet(cfg.bright); cfgDirty=true; }
static void appSettings(){
  dimBg(); uint16_t ac=ACC[cfg.accent], lt=RGB(200,220,245);
  gText("settings",10,22,4,0xFFFF);
  gText("brightness",10,50,2,lt);
  int fw=(cfg.bright-20)*280/235; gFill(20,77,280,5,RGB(70,90,120)); gFill(20,77,fw,5,ac); gCirc(20+fw,79,8,0xFFFF); gCirc(20+fw,79,5,ac);
  gText("wallpaper",10,92,2,lt);
  for(int i=0;i<4;i++){ int x=20+i*72; for(int r=0;r<28;r++) gHLine(x,112+r,64,themeCol(i,r/27.f));
    gRectO(x-1,111,66,30,i==cfg.wall?0xFFFF:RGB(60,70,90)); if(i==cfg.wall) gRectO(x-2,110,68,32,0xFFFF); }
  gText("accent",10,142,2,lt);
  for(int i=0;i<8;i++){ int cx=34+i*36; gCirc(cx,172,10,ACC[i]); if(i==cfg.accent){ gCircO(cx,172,13,0xFFFF); gCircO(cx,172,12,0xFFFF);} }
  gText("dream mode",10,190,2,lt); gText(cfg.hard?"hard":"easy",120,190,2,0xFFFF);
  gRRect(236,189,64,20,10,cfg.hard?ac:RGB(70,80,100)); gCirc(cfg.hard?290:246,199,8,0xFFFF);
}
static void settingsTouch(int x,int y,bool down){
  if(y>=64&&y<=94&&x>=8){ setBright(x); return; }
  if(!down) return;
  if(y>=110&&y<=142){ int i=(x-20)/72; if(x>=20&&i>=0&&i<4){ cfg.wall=i; buildWall(); cfgDirty=true; } }
  else if(y>=158&&y<=186){ int i=(x-16)/36; if(x>=16&&i>=0&&i<8){ cfg.accent=i; cfgDirty=true; } }
  else if(y>=184&&y<=212){ cfg.hard=!cfg.hard; cfgDirty=true; }
}

// --- paint
#define CW 160
#define CH 80
#define CY0 50
static uint8_t* cv=nullptr; static int pcol=1,lpx=-1,lpy=-1;
static const uint16_t PC[8]={0,RGB(255,255,255),RGB(229,20,0),RGB(250,104,0),RGB(255,220,0),RGB(60,200,60),RGB(27,161,226),RGB(180,60,255)};
static void stamp(int cx,int cy){ int rad=pcol==0?3:1;
  for(int dy=-rad;dy<=rad;dy++)for(int dx=-rad;dx<=rad;dx++){ int xx=cx+dx,yy=cy+dy; if(xx>=0&&xx<CW&&yy>=0&&yy<CH) cv[yy*CW+xx]=pcol; } }
static void paintAt(int x,int y){
  if(!cv) return; int cx=clampi(x/2,0,CW-1), cy=clampi((y-CY0)/2,0,CH-1);
  if(lpx<0){ stamp(cx,cy); }
  else { int x0=lpx,y0=lpy,dx=abs(cx-x0),dy=-abs(cy-y0),sx=x0<cx?1:-1,sy=y0<cy?1:-1,err=dx+dy;
    while(true){ stamp(x0,y0); if(x0==cx&&y0==cy)break; int e2=2*err; if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} } }
  lpx=cx; lpy=cy;
}
static void appPaint(){
  dimBg(10);
  for(int i=0;i<9;i++){ int x=8+i*28; if(i<8){ gFill(x,24,24,20,i==0?RGB(40,50,70):PC[i]); if(i==0){gLine(x+6,28,x+18,40,0xFFFF);gLine(x+18,28,x+6,40,0xFFFF);} }
    else { gFill(x,24,24,20,RGB(70,80,100)); gText("clr",x+12,34,1,0xFFFF,MC_DATUM); }
    if(i==pcol&&i<8) gRectO(x-2,22,28,24,0xFFFF); }
  gBlend(0,CY0-2,SW,CH*2+4,0,22); gRectO(0,CY0-2,SW,CH*2+4,RGB(90,130,190));
  if(!cv) return;
  for(int r=0;r<CH;r++){ int yy=CY0+r*2; if(!vis(yy,2)) continue;
    int c=0; while(c<CW){ uint8_t v=cv[r*CW+c]; if(!v){c++;continue;} int s=c; while(c<CW&&cv[r*CW+c]==v)c++; gFill(s*2,yy,(c-s)*2,2,PC[v]); } }
}
static void paintTouch(int x,int y,bool down){
  if(down&&y>=22&&y<48){ int i=(x-8)/28; if(x>=8&&i>=0&&i<9){ if(i==8&&cv) memset(cv,0,CW*CH); else if(i<8) pcol=i; } lpx=-1; return; }
  if(y>=CY0&&y<CY0+CH*2){ if(down) lpx=-1; paintAt(x,y); }
}

// --- terminal
#define TLN 16
static char tl[TLN][54]; static int tcount=0; static char tin[40]; static int tinLen=0;
static char kbFlash=0; static float kbFlashT=0;
#define KB_Y 128
static void pushLine(const char*l){ if(tcount==TLN){ memmove(tl[0],tl[1],(TLN-1)*54); tcount--; } strncpy(tl[tcount],l,53); tl[tcount][53]=0; tcount++; }
static void tPrint(const char*s){ Serial.println(s); char line[54];
  while(true){ strncpy(line,s,52); line[52]=0; pushLine(line); if(strlen(s)<=52) break; s+=52; } }
static void runCmd(const char*in){
  char buf[48]; strncpy(buf,in,47); buf[47]=0; for(char*p=buf;*p;p++) *p=tolower(*p);
  char line[54]; snprintf(line,sizeof line,"> %s",buf); tPrint(line);
  char*c=strtok(buf," "); if(!c) return; char*a=strtok(NULL," ");
  if(!strcmp(c,"help")){ tPrint("poweroff       power off"); tPrint("reboot         reboot"); tPrint("help           this help"); tPrint("dreammode      hard|easy"); }
  else if(!strcmp(c,"poweroff")){ tPrint("powering off..."); pendingAct=2; pendAt=millis()+450; }
  else if(!strcmp(c,"reboot")){ tPrint("rebooting..."); pendingAct=1; pendAt=millis()+450; }
  else if(!strcmp(c,"dreammode")){
    if(a&&!strcmp(a,"hard")){ cfg.hard=true; cfgDirty=true; tPrint("dreammode: hard"); }
    else if(a&&!strcmp(a,"easy")){ cfg.hard=false; cfgDirty=true; tPrint("dreammode: easy"); }
    else { snprintf(line,sizeof line,"dreammode: %s  (usage: dreammode hard|easy)",cfg.hard?"hard":"easy"); tPrint(line); } }
  else { snprintf(line,sizeof line,"unknown command: %s",c); tPrint(line); }
}
static void typeChar(char c){
  if(c==8){ if(tinLen>0) tin[--tinLen]=0; }
  else if(c==13){ tin[tinLen]=0; runCmd(tin); tinLen=0; tin[0]=0; }
  else if(tinLen<38){ tin[tinLen++]=c; tin[tinLen]=0; }
}
static const char* KR0="qwertyuiop"; static const char* KR1="asdfghjkl"; static const char* KR2="zxcvbnm";
static char kbHit(int x,int y){
  if(y<KB_Y||y>=KB_Y+80) return 0; int r=(y-KB_Y)/20;
  if(r==0){ int i=x/32; return i<10?KR0[i]:0; }
  if(r==1){ if(x<16) return 0; int i=(x-16)/32; return i<9?KR1[i]:0; }
  if(r==2){ if(x>=240) return 8; if(x<16) return 0; int i=(x-16)/32; return i<7?KR2[i]:0; }
  return x<224?' ':13;
}
static void key(int x,int y,int w,const char*lab,char code){
  bool p=(kbFlash==code&&kbFlashT>0); uint16_t c=p?ACC[cfg.accent]:RGB(34,50,82);
  gRRect(x+1,y+1,w-2,18,3,c); gBlend(x+1,y+1,w-2,8,0xFFFF,p?0:3); gText(lab,x+w/2,y+10,2,0xFFFF,MC_DATUM);
}
static void appTerm(float ts){
  gBlend(0,22,SW,104,0,28); gHLine(0,22,SW,RGB(90,130,190));
  int start=imax(0,tcount-8);
  for(int i=start;i<tcount;i++){ bool cmd=tl[i][0]=='>'&&tl[i][1]==' ';
    gText(tl[i],6,26+(i-start)*10,1,cmd?RGB(150,220,255):RGB(190,255,190)); }
  gHLine(0,108,SW,RGB(60,90,140));
  char b[48]; snprintf(b,sizeof b,"> %s%s",tin,((millis()/500)&1)?"_":" "); gText(b,6,113,2,0xFFFF);
  char s[2]={0,0};
  for(int i=0;i<10;i++){ s[0]=KR0[i]; key(i*32,KB_Y,32,s,KR0[i]); }
  for(int i=0;i<9;i++){ s[0]=KR1[i]; key(16+i*32,KB_Y+20,32,s,KR1[i]); }
  for(int i=0;i<7;i++){ s[0]=KR2[i]; key(16+i*32,KB_Y+40,32,s,KR2[i]); }
  key(240,KB_Y+40,80,"<-",8); key(0,KB_Y+60,224,"space",' '); key(224,KB_Y+60,96,"enter",13);
  (void)ts;
}
static void termHeader(){ if(tcount==0){ tPrint("Minimal Terminal."); tPrint("Type help to get commands"); } }

static void appInit(int id){
  if(id==A_PAINT){ cv=(uint8_t*)calloc(CW*CH,1); lpx=-1; }
  if(id==A_TERM) termHeader();
}
static void appCleanup(){ if(cv){ free(cv); cv=nullptr; } }
static void drawApp(float ts){
  switch(app){
    case A_CLOCK: appClock(); break; case A_TERM: appTerm(ts); break; case A_SET: appSettings(); break;
    case A_PAINT: appPaint(); break; case A_ABOUT: appAbout(); break; case A_TORCH: appTorch(); break; }
}
static void appTouch(int x,int y,bool down){
  switch(app){
    case A_SET: settingsTouch(x,y,down); break; case A_PAINT: paintTouch(x,y,down); break;
    case A_TERM: if(down){ char c=kbHit(x,y); if(c){ kbFlash=c; kbFlashT=0.12f; typeChar(c);} } break; }
}

// ---------- transitions ----------
static void startOpen(int appId,int ti,int rx,int ry,int rw,int rh,uint16_t col){
  if(tr.act||scr!=S_HOME) return;
  lastTile=ti; lrx=rx; lry=ry; lrw=rw; lrh=rh; lcol=col;
  tr.act=true; tr.opening=true; tr.t=0; tr.x=rx; tr.y=ry; tr.w=rw; tr.h=rh; tr.col=col; tr.appId=appId;
}
static void closeApp(){
  if(scr!=S_APP||tr.act) return;
  appCleanup(); app=A_NONE; scr=S_HOME;
  int x=lrx,y=lry,w=lrw,h=lrh; if(lastTile>=0) tileRect(lastTile,x,y,w,h);
  tr.act=true; tr.opening=false; tr.t=0; tr.x=x; tr.y=y; tr.w=w; tr.h=h; tr.col=lcol;
}
static void forceHome(){
  if(scr==S_APP){ appCleanup(); app=A_NONE; }
  tr.act=false; scr=S_HOME; lockY=lockYT=-SH; homeIn=homeInT=1; appWipe=0;
}
static void drawTransition(){
  float e=easeInOut(tr.t); if(!tr.opening) e=1-e;
  int x=(int)lerpf(tr.x,0,e),y=(int)lerpf(tr.y,0,e),w=(int)lerpf(tr.w,SW,e),h=(int)lerpf(tr.h,SH,e);
  gFill(x,y,w,h,tr.col); gBlend(x,y,w,h/2,0xFFFF,3);
}
static void drawWipe(){ float e=easeInOut(appWipe); gFill((int)(SW*(1-e)),0,SW-(int)(SW*(1-e)),SH,ACC[cfg.accent]); }

// ---------- shade ----------
static void closeShade(){ shadeDrag=false; shadeTarget=0; }
static void drawShade(){
  int h=(int)shadeH; if(h<2) return;
  gBlend(0,0,SW,SH,0,16*h/SHADE_MAX);
  gBlend(0,0,SW,h,RGB(4,14,38),26); gBlend(0,0,SW,h/2,0xFFFF,3);
  gHLine(0,h-1,SW,RGB(120,190,255)); gHLine(0,h,SW,RGB(20,40,80));
  int oy=h-SHADE_MAX; uint16_t ac=ACC[cfg.accent];
  static const char* nm[4]={"dream","torch","terminal","lock"};
  for(int i=0;i<4;i++){ int x=8+i*78,y=22+oy; uint16_t c=(i==0&&cfg.hard)?ac:RGB(44,60,92);
    gFill(x,y,72,42,c); gBlend(x,y,72,21,0xFFFF,3);
    gText(nm[i],x+36,y+(i==0?15:21),2,0xFFFF,MC_DATUM);
    if(i==0) gText(cfg.hard?"hard":"easy",x+36,y+33,1,0xFFFF,MC_DATUM); }
  gText("brightness",10,68+oy,1,RGB(190,210,240));
  int fw=(cfg.bright-20)*288/235; gFill(16,86+oy,288,4,RGB(60,80,110)); gFill(16,86+oy,fw,4,0xFFFF);
  gCirc(16+fw,88+oy,7,0xFFFF); gCirc(16+fw,88+oy,4,ac);
  gFill(8,102+oy,304,38,RGB(30,46,76)); gHLine(8,102+oy,304,RGB(90,130,190));
  gText("AEROCyd",16,106+oy,2,0xFFFF); gText("swipe the tiles, open terminal, try dreammode",16,126+oy,1,RGB(190,210,240));
  gRRect(SW/2-18,h-8,36,4,2,0xFFFF);
}

// ---------- scenes ----------
static bool showMsg=false;
static void sceneMain(){
  float ts=millis()/1000.f;
  drawWallpaper(ts,scrollX);
  if(scr==S_APP) drawApp(ts); else if(homeIn>0.001f) drawTiles(ts);
  if(scr==S_LOCK||lockY>-SH+1) drawLock(ts);
  if(tr.act) drawTransition();
  if(appWipe>0&&scr==S_APP) drawWipe();
  drawNav(); drawStatus(); drawShade();
  if(dimA) gBlend(0,0,SW,SH,0,dimA);
  if(showMsg&&msgTxt){ int n=(millis()/350)%4; int w=textW(msgTxt,4); gText(msgTxt,SW/2,SH/2,4,0xFFFF,MC_DATUM);
    char d[4]="   "; for(int i=0;i<n;i++) d[i]='.'; gText(d,SW/2+w/2+2,SH/2-9,4,0xFFFF); }
  if(crtT>0){
    if(crtT<1){ int vh=imax(3,(int)((1-crtT)*(1-crtT)*SH)); int top=(SH-vh)/2;
      gFill(0,0,SW,top,0); gFill(0,top+vh,SW,SH-top-vh,0); if(vh<=30) gBlend(0,SH/2-1,SW,2,0xFFFF,24); }
    else { gFill(0,0,SW,SH,0); float f=1.f-(crtT-1.f)*2.f; int w=f>0?(int)(SW*f):0; gFill(SW/2-w/2,SH/2-1,w,2,0xFFFF); }
  }
}
static float bootT=0;
static void sceneBoot(){
  drawWallpaper(bootT,0);
  int cx=SW/2,cy=90; const uint16_t pc[4]={RGB(240,80,40),RGB(130,200,40),RGB(30,140,230),RGB(250,200,30)};
  for(int i=0;i<4;i++){ float e=easeOut((bootT-0.3f-i*0.12f)/0.5f); int s=(int)(26*e);
    int ox=(i&1)?cx+3:cx-3-26, oy=(i>>1)?cy+3:cy-3-26, pcx=ox+13, pcy=oy+13;
    gFill(pcx-s/2,pcy-s/2,s,s,pc[i]); gBlend(pcx-s/2,pcy-s/2,s,s/2,0xFFFF,6); }
  int a=(int)(32*clampf((bootT-1.0f)/0.6f,0,1));
  gText("AEROCyd",cx,150,4,blend(0xFFFF,rowCol[150],a),MC_DATUM);
  if(bootT>1.2f){ for(int i=0;i<5;i++){ float cyc=fmodf(bootT*0.8f-i*0.09f,1.5f); if(cyc<0) cyc+=1.5f; if(cyc>=1.2f) continue;
      float u=cyc/1.2f; float xf=u+0.12f*sinf(2*PIf*u); gCirc(8+(int)(xf*(SW-16)),196,2,0xFFFF); } }
  float f=clampf(bootT/0.5f,0,1); gBlend(0,0,SW,SH,0,(int)(32*(1-f)));
}
static void renderBands(void(*sc)()){ for(int b=0;b<2;b++){ bandY=b*BH; sc(); spr.pushSprite(0,bandY);} }

// ---------- power ----------
bool ts_touched_wait();
static void shutAnim(bool off){
  msgTxt=off?"shutting down":"restarting"; showMsg=true; closeShade();
  uint32_t t0=millis();
  while(millis()-t0<900){ dimA=(uint8_t)(24*easeOut((millis()-t0)/900.f)); renderBands(sceneMain); }
  showMsg=false; t0=millis();
  while(millis()-t0<650){ crtT=1.5f*(millis()-t0)/650.f; renderBands(sceneMain); }
  tft.fillScreen(TFT_BLACK); blSet(0);
  if(off){
    while(ts_touched_wait()) delay(20);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)T_IRQ,0);
    esp_deep_sleep_start();
  } else { delay(150); ESP.restart(); }
}

// ---------- touch ----------
static XPT2046_Touchscreen ts(T_CS);
static SPIClass tsSPI(VSPI);
bool ts_touched_wait(){ return ts.touched(); }
struct TouchS{bool down=false;int sx=0,sy=0,lx=0,ly=0;bool moved=false;uint32_t lt=0;} T;
enum Mode{M_NONE,M_SHADE,M_SHADEUI,M_NAV,M_LOCK,M_HOME,M_APP};
static Mode mode=M_NONE; static float shadeStartH=0; static bool sliderOn=false,homeDrag=false;
static float scrollStart=0,scrollVel=0; static int lockDy=0;

static bool readTouch(int&x,int&y){
  if(!ts.touched()) return false; TS_Point p=ts.getPoint();
  x=clampi(map(p.x,TX_MIN,TX_MAX,0,SW),0,SW-1); y=clampi(map(p.y,TY_MIN,TY_MAX,0,SH),0,SH-1); return true;
}
static void lockDevice(){
  if(scr==S_APP){ appCleanup(); app=A_NONE; } tr.act=false; scr=S_LOCK; lockYT=0; homeInT=0; closeShade();
}
static void tileAction(int i){
  int id=TILES[i].id; int x,y,w,h; tileRect(i,x,y,w,h);
  if(id<=A_TORCH){ startOpen(id,i,x,y,w,h,tileColor(i)); return; }
  switch(id){
    case Q_DREAM: cfg.hard=!cfg.hard; break;
    case Q_WALL: cfg.wall=(cfg.wall+1)&3; buildWall(); break;
    case Q_ACCENT: cfg.accent=(cfg.accent+1)&7; break;
    case Q_LOCK: lockDevice(); break;
    case Q_REBOOT: pendingAct=1; pendAt=millis()+50; break;
    case Q_OFF: pendingAct=2; pendAt=millis()+50; break; }
  cfgDirty=true;
}
static void navAction(int b){
  if(b==0){ if(shadeH>8){closeShade();return;} if(scr==S_APP) closeApp(); else if(scr==S_HOME&&scrollT>0){scrollT=0;} }
  else if(b==1){ if(shadeH>8) closeShade(); if(scr==S_APP) closeApp(); else if(scr==S_HOME) scrollT=0; }
  else { if(shadeH>8) closeShade();
    if(scr==S_APP&&app!=A_TERM){ forceHome(); }
    if(scr==S_HOME) startOpen(A_TERM,-1,SW-80,NAV_Y,60,20,ACC[(cfg.accent+1)&7]); }
}
static void shadeTap(int x,int y){
  int oy=(int)shadeH-SHADE_MAX, yy=y-oy;
  if(yy>=22&&yy<64&&x>=8){ int i=(x-8)/78; if(i>3||(x-8)%78>=72) return;
    int bx=8+i*78, by=22+oy;
    if(i==0){ cfg.hard=!cfg.hard; cfgDirty=true; }
    else if(i==3){ lockDevice(); }
    else { int id=(i==1)?A_TORCH:A_TERM; closeShade(); if(scr!=S_HOME||tr.act){ forceHome(); } startOpen(id,-1,bx,by,72,42,ACC[cfg.accent]); }
  }
}
static void handleDown(int x,int y){
  T.sx=T.lx=x; T.sy=T.ly=y; T.moved=false; T.lt=millis(); sliderOn=false; homeDrag=false; pressedTile=-1;
  if(tr.act){ mode=M_NONE; return; }
  if(shadeH>8){
    if(y<shadeH){ mode=M_SHADEUI; int oy=(int)shadeH-SHADE_MAX;
      if(abs(y-(88+oy))<14&&x>=8&&x<=312){ sliderOn=true; cfg.bright=(uint8_t)clampi(20+(x-16)*235/288,20,255); blSet(cfg.bright); cfgDirty=true; } }
    else { mode=M_SHADE; shadeStartH=shadeH; }
    return; }
  if(y<STATUS_H+2){ mode=M_SHADE; shadeStartH=shadeH; return; }
  if(scr!=S_LOCK&&y>=NAV_Y){ mode=M_NAV; navPressed=x/107; navRip=0; navRipBtn=navPressed; return; }
  if(scr==S_LOCK){ mode=M_LOCK; lockDy=0; return; }
  if(scr==S_HOME){ mode=M_HOME; scrollStart=scrollX; scrollVel=0; pressedTile=(homeIn>0.8f)?hitTile(x,y):-1; return; }
  mode=M_APP; appTouch(x,y,true);
}
static void handleMove(int x,int y){
  switch(mode){
    case M_SHADE: if(abs(y-T.sy)>4){ T.moved=true; shadeDrag=true; }
      if(shadeDrag) shadeH=clampf(shadeStartH+(y-T.sy),0,SHADE_MAX); break;
    case M_SHADEUI:
      if(sliderOn){ cfg.bright=(uint8_t)clampi(20+(x-16)*235/288,20,255); blSet(cfg.bright); }
      else if(abs(y-T.sy)>8){ mode=M_SHADE; shadeStartH=shadeH; T.moved=true; shadeDrag=true; shadeH=clampf(shadeStartH+(y-T.sy),0,SHADE_MAX); } break;
    case M_LOCK: { int dy=T.sy-y; if(dy<0) dy=0; lockDy=dy; lockY=lockYT=-(float)dy; homeIn=homeInT=clampf(dy/110.f,0,1); anim=true; } break;
    case M_HOME:
      if(abs(x-T.sx)>8||abs(y-T.sy)>8){ pressedTile=-1; }
      if(!homeDrag&&abs(x-T.sx)>8&&abs(x-T.sx)>abs(y-T.sy)){ homeDrag=true; T.moved=true; scrollStart=scrollX+(x-T.sx)*0.f; }
      if(homeDrag){ uint32_t n=millis(); float dtm=imax(1,(int)(n-T.lt))/1000.f; scrollVel=0.7f*scrollVel+0.3f*((T.lx-x)/dtm); T.lt=n;
        scrollX=clampf(scrollStart+(T.sx-x),-30,342); } break;
    case M_APP: appTouch(x,y,false); break;
    default: break; }
  T.lx=x; T.ly=y;
}
static void handleUp(int x,int y){
  switch(mode){
    case M_SHADE: shadeDrag=false;
      if(T.moved){ if(shadeStartH<8) shadeTarget=(shadeH>SHADE_MAX*0.3f)?SHADE_MAX:0; else shadeTarget=(shadeH<SHADE_MAX*0.7f)?0:SHADE_MAX; }
      else shadeTarget=(shadeStartH>8)?0:SHADE_MAX; break;
    case M_SHADEUI: if(sliderOn) sliderOn=false; else shadeTap(x,y); break;
    case M_NAV: { int b=T.sx/107; if(b==x/107&&y>=NAV_Y-8) navAction(b); navPressed=-1; } break;
    case M_LOCK:
      if(lockDy>70){ scr=S_HOME; lockYT=-SH; homeInT=1; } else { lockYT=0; homeInT=0; } break;
    case M_HOME:
      if(homeDrag){ homeDrag=false; float pos=scrollX+scrollVel*0.18f; scrollT=(pos>156)?312:0; }
      else if(pressedTile>=0){ int i=pressedTile; pressedTile=-1; if(hitTile(x,y)==i) tileAction(i); } break;
    default: break; }
  navPressed=-1; pressedTile=-1; mode=M_NONE;
  if(cfgDirty) saveCfg();
}
static void pollTouch(){
  static uint8_t miss=0; int x,y;
  if(readTouch(x,y)){ miss=0; dirty=true;
    if(!T.down){ T.down=true; handleDown(x,y); T.lx=x; T.ly=y; } else handleMove(x,y); }
  else if(T.down){ if(++miss>=4){ T.down=false; miss=0; dirty=true; handleUp(T.lx,T.ly); } }
}

// ---------- serial console (same commands) ----------
static char sbuf[48]; static int slen=0;
static void serialPoll(){
  while(Serial.available()){ char c=Serial.read(); if(c=='\r') continue;
    if(c=='\n'){ sbuf[slen]=0; if(slen){ termHeader(); runCmd(sbuf);} slen=0; }
    else if(slen<46) sbuf[slen++]=c; }
}

// ---------- update ----------
static void update(float dt){
  anim=false;
  if(fabsf(lockYT-lockY)>0.5f){ lockY+=(lockYT-lockY)*(1-expf(-dt*10)); anim=true; } else lockY=lockYT;
  if(fabsf(homeInT-homeIn)>0.004f){ homeIn+=(homeInT-homeIn)*(1-expf(-dt*6)); anim=true; } else homeIn=homeInT;
  if(!homeDrag){ if(fabsf(scrollT-scrollX)>0.5f){ scrollX+=(scrollT-scrollX)*(1-expf(-dt*12)); anim=true; } else scrollX=scrollT; }
  if(!shadeDrag){ if(fabsf(shadeTarget-shadeH)>0.5f){ shadeH+=(shadeTarget-shadeH)*(1-expf(-dt*14)); anim=true; } else shadeH=shadeTarget; }
  if(tr.act){ tr.t+=dt/0.30f; anim=true;
    if(tr.t>=1){ tr.act=false; if(tr.opening){ scr=S_APP; app=tr.appId; appInit(app); appWipe=1; } } }
  if(appWipe>0){ appWipe-=dt/0.35f; if(appWipe<0) appWipe=0; anim=true; }
  if(navRip>=0){ navRip+=dt/0.4f; if(navRip>1) navRip=-1; anim=true; }
  if(kbFlashT>0){ kbFlashT-=dt; anim=true; }
  if(cfg.hard){ updBubbles(dt); anim=true; }
}

// ---------- setup / loop ----------
static void bootAnim(){
  uint32_t t0=millis();
  while((bootT=(millis()-t0)/1000.f)<3.0f){ updBubbles(0.03f); renderBands(sceneBoot); }
}
static uint32_t lastMs=0;
void setup(){
  Serial.begin(115200);
  loadCfg(); initTime(); randomSeed(esp_random());
  blInit(); blSet(0);
  tft.init(); tft.setRotation(1);
    tft.invertDisplay(true);
#if CYD_INVERT
  tft.invertDisplay(true);
#endif
  tft.fillScreen(TFT_BLACK);
  spr.setColorDepth(16);
  if(!spr.createSprite(SW,BH)){ Serial.println("sprite alloc failed"); while(1) delay(1000); }
  BUF=(uint16_t*)spr.getPointer();
  tsSPI.begin(T_CLK,T_MISO,T_MOSI,T_CS); ts.begin(tsSPI); ts.setRotation(1);
  buildWall(); initBubbles();
  blSet(cfg.bright);
  bootAnim();
  scr=S_LOCK; lockY=lockYT=0; homeIn=homeInT=0; lastMs=millis();
}
void loop(){
  uint32_t now=millis(); float dt=(now-lastMs)/1000.f; if(dt>0.05f) dt=0.05f; lastMs=now;
  pollTouch(); serialPoll();
  if(pendingAct&&now>=pendAt){ int a=pendingAct; pendingAct=0; shutAnim(a==2); }
  update(dt);
  static int lastSec=-1,lastBlink=-1; int sec=now/1000, bl=(now/500)&1;
  bool tick=(sec!=lastSec)||(bl!=lastBlink); lastSec=sec; lastBlink=bl;
  if(cfg.hard||anim||dirty||tick){ dirty=false; renderBands(sceneMain); } else delay(8);
}
