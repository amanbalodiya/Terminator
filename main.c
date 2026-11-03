/* X11 front end for the existing shell engine. Xlib is used only on the GUI thread. */
#include "shell.h"
#include "history.h"
#include "parser.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#define MAX_TABS 16
#define INPUT_MAX 8192
#define OUTPUT_MAX 131072
#define BAR 94
#define INITIAL_FONT 19
#define LEFT 12

typedef struct {
 int id, input_write, output_read, worker_read, worker_write, alive, running;
 pid_t worker_pid;
 pthread_t thread;
 shell_t *shell;
 char input[INPUT_MAX], pending[INPUT_MAX]; size_t len, cursor;
 char output[OUTPUT_MAX]; size_t outlen;
 int scroll, histpos, searching;
 char search[INPUT_MAX], search_match[INPUT_MAX]; size_t search_len;
} Tab;
static Tab *tabs[MAX_TABS]; static int ntabs, active, nextid=1, process_mode;
static Display *display; static Window window; static GC gc;
static XftFont *barfont; static XftFont *font; static XftDraw *xdraw; static XftColor fg;
static int font_size=INITIAL_FONT, line_height=26, char_width=14;
static Atom clipboard_atom, utf8_atom, targets_atom, paste_atom, compound_atom, incr_atom;
static int paste_incr=0;
static size_t paste_total=0;
static Atom paste_selection;
static int quit_requested=0;
static int paste_fallback=0;
static char *selected=NULL; static int selecting=0, sel_start=-1, sel_end=-1;
static int view_start=0, view_end=0, view_cols=1, view_rows=1;
static int *view_lines=NULL, view_count=0;
static int width=1000,height=650,dirty=1;
static int cursor_visible=1;
static Cursor terminal_pointer=None, tab_pointer=None;
static int pointer_in_tabs=-1;
static void update_pointer_for_y(int y){
 int in_tabs=(y<BAR);
 if(in_tabs==pointer_in_tabs)return;
 pointer_in_tabs=in_tabs;
 XDefineCursor(display,window,in_tabs?tab_pointer:terminal_pointer);
}

static int clipboard_debug=0;
static long long monotonic_ms(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (long long)ts.tv_sec*1000+ts.tv_nsec/1000000;}
#define CLIPLOG(...) do {if(clipboard_debug)fprintf(stderr,__VA_ARGS__);}while(0)
static unsigned long white,black,blue;
/* Configurable shortcuts: ~/.config/terminator/hotkeys.conf (ACTION=Ctrl+Key). */
typedef struct {const char *action; const char *default_combo; unsigned int modifiers; KeySym key;} Hotkey;
static Hotkey hotkeys[]={
 {"new_tab","Ctrl+t",0,0},{"close_tab","Ctrl+Shift+w",0,0},
 {"next_tab","Ctrl+Tab",0,0},{"zoom_in","Ctrl+plus",0,0},
 {"zoom_out","Ctrl+minus",0,0},{"zoom_reset","Ctrl+0",0,0},
 {"clear","Ctrl+l",0,0},{"interrupt","Ctrl+c",0,0},
 {"suspend","Ctrl+z",0,0},{"copy","Ctrl+Shift+c",0,0},
 {"paste","Ctrl+Shift+v",0,0},{"reverse_search","Ctrl+r",0,0}
};
#define NHOTKEYS ((int)(sizeof(hotkeys)/sizeof(hotkeys[0])))
static int parse_hotkey(const char *combo,unsigned int *mods,KeySym *key){
 char tmp[128];snprintf(tmp,sizeof(tmp),"%s",combo);char *save=NULL,*part=strtok_r(tmp,"+",&save);
 unsigned int m=0;KeySym k=NoSymbol;
 while(part){while(isspace((unsigned char)*part))part++;char *e=part+strlen(part);while(e>part&&isspace((unsigned char)e[-1]))*--e=0;
  if(!strcasecmp(part,"Ctrl")||!strcasecmp(part,"Control"))m|=ControlMask;
  else if(!strcasecmp(part,"Shift"))m|=ShiftMask;
  else if(!strcasecmp(part,"Alt"))m|=Mod1Mask;
  else {k=XStringToKeysym(part);if(k==NoSymbol&&strlen(part)==1){char a[2]={(char)tolower((unsigned char)part[0]),0};k=XStringToKeysym(a);}}
  part=strtok_r(NULL,"+",&save);
 }
 if(k==NoSymbol)return -1;
 *mods=m;*key=k;return 0;
}
static void load_hotkeys(void){
 for(int i=0;i<NHOTKEYS;i++)parse_hotkey(hotkeys[i].default_combo,&hotkeys[i].modifiers,&hotkeys[i].key);
 const char *home=getenv("HOME");if(!home)return;char path[PATH_MAX];snprintf(path,sizeof(path),"%s/.config/terminator/hotkeys.conf",home);
 FILE *f=fopen(path,"r");if(!f)return;char line[256];
 while(fgets(line,sizeof(line),f)){char *p=line;while(isspace((unsigned char)*p))p++;if(*p=='#'||!*p)continue;char *eq=strchr(p,'=');if(!eq)continue;*eq++=0;char *end=p+strlen(p);while(end>p&&isspace((unsigned char)end[-1]))*--end=0;
  for(int i=0;i<NHOTKEYS;i++)if(!strcmp(p,hotkeys[i].action)){unsigned int m;KeySym k;if(parse_hotkey(eq,&m,&k)==0){hotkeys[i].modifiers=m;hotkeys[i].key=k;}break;}
 }fclose(f);
}
static int hotkey_match(int id,XKeyEvent *event,KeySym sym){
 Hotkey *h=&hotkeys[id];unsigned int mods=event->state&(ControlMask|ShiftMask|Mod1Mask);
 if(h->key==XK_plus && (sym==XK_plus||sym==XK_equal||sym==XK_KP_Add)) mods &= ~ShiftMask;
 if(mods!=h->modifiers)return 0;
 if(sym==h->key || XLookupKeysym(event,0)==h->key)return 1;
 if(h->key>=XK_a&&h->key<=XK_z){KeySym raw=XLookupKeysym(event,0);if(sym==h->key-(XK_a-XK_A)||raw==h->key-(XK_a-XK_A))return 1;}
 if(h->key==XK_plus&&(sym==XK_equal||sym==XK_KP_Add))return 1;
 if(h->key==XK_minus&&sym==XK_KP_Subtract)return 1;
 return 0;
}


static void set_nonblock(int fd){int f=fcntl(fd,F_GETFL);if(f>=0)fcntl(fd,F_SETFL,f|O_NONBLOCK);}
static void append(Tab *t,const char *s,size_t n){
 if(n>=OUTPUT_MAX){s+=n-(OUTPUT_MAX-1);n=OUTPUT_MAX-1;}
 if(t->outlen+n>=OUTPUT_MAX){size_t drop=t->outlen+n-OUTPUT_MAX+1;memmove(t->output,t->output+drop,t->outlen-drop);t->outlen-=drop;}
 /* Handle ANSI clear-screen emitted by `clear` or printf. */
 const char *clear_seq="\033[2J\033[H";
 for(size_t i=0;i<n;i++){
  if(i+7<=n && !memcmp(s+i,clear_seq,7)){t->outlen=0;t->output[0]=0;i+=6;continue;}
  if(t->outlen>=OUTPUT_MAX-1){memmove(t->output,t->output+1,--t->outlen);}
  t->output[t->outlen++]=s[i];
 }
 t->output[t->outlen]=0;t->scroll=0;dirty=1;
}
static void print_prompt(shell_t *sh,int fd,int continuation){
 if(continuation){(void)write(fd,"> ",2);return;}
 const char *cwd=sh->cwd,*home=getenv("HOME");
 char path[PATH_MAX+2];
 if(home && *home && !strncmp(cwd,home,strlen(home)) &&
    (cwd[strlen(home)]=='/' || cwd[strlen(home)]=='\0'))
  snprintf(path,sizeof(path),"~%s",cwd+strlen(home));
 else snprintf(path,sizeof(path),"%s",cwd);
 char prompt[PATH_MAX+64];
 int n=snprintf(prompt,sizeof(prompt),"user@terminator:%s$ ",path);
 if(n>0)(void)write(fd,prompt,(size_t)n<sizeof(prompt)?(size_t)n:sizeof(prompt)-1);
}
static void worker_loop(Tab *t){
 shell_t *s=shell_new(t->id,-1,t->worker_write,t->worker_write);t->shell=s;
 print_prompt(s,t->worker_write,0);
 char data[2048], line[INPUT_MAX], pending[INPUT_MAX];size_t used=0,plen=0;
 for(;;){ssize_t n=read(t->worker_read,data,sizeof(data));if(n<0&&errno==EINTR)continue;if(n<=0)break;
  for(ssize_t i=0;i<n;i++){
   if(data[i]!='\n'){if(used<sizeof(line)-1)line[used++]=data[i];continue;}
   line[used]=0;used=0;
   if(!strcmp(line,"\003")){if(plen){plen=0;pending[0]=0;print_prompt(s,t->worker_write,0);}else shell_signal_fg(s,SIGINT);continue;}
   if(!strcmp(line,"\032")){shell_signal_fg(s,SIGTSTP);continue;}
   size_t len=strlen(line);
   if(plen+len+2>=sizeof(pending)){plen=0;sh_err(s,"terminator: multiline command too long\n");print_prompt(s,t->worker_write,0);continue;}
   memcpy(pending+plen,line,len);plen+=len;pending[plen]=0;
   if(line_incomplete(pending)){
    /* Remove escaped newline outside quotes (shell parser handles the rest). */
    if(plen && pending[plen-1]=='\\') pending[--plen]=0;
    else pending[plen++]='\n',pending[plen]=0;
    print_prompt(s,t->worker_write,1);continue;
   }
   shell_submit(s,pending);plen=0;pending[0]=0;
   if(s->exit_requested)goto done;
   print_prompt(s,t->worker_write,0);
  }
 }
 done:shell_free(s);t->shell=NULL;
}
static void *thread_main(void *arg){Tab *t=arg;worker_loop(t);close(t->worker_read);close(t->worker_write);return NULL;}
static int newtab(void){
 if(ntabs>=MAX_TABS)return -1;
 int in[2],out[2];if(pipe(in)||pipe(out))return -1;
 Tab *t=calloc(1,sizeof(*t));if(!t){close(in[0]);close(in[1]);close(out[0]);close(out[1]);return -1;}
 t->id=nextid++;t->input_write=in[1];t->output_read=out[0];t->worker_read=in[0];t->worker_write=out[1];t->alive=1;
 if(process_mode){pid_t pid=fork();if(pid<0){free(t);return -1;}if(pid==0){close(in[1]);close(out[0]);worker_loop(t);_exit(0);}t->worker_pid=pid;close(in[0]);close(out[1]);t->worker_read=t->worker_write=-1;}
 else {if(pthread_create(&t->thread,NULL,thread_main,t)!=0){free(t);return -1;}}
 set_nonblock(t->output_read);tabs[ntabs++]=t;active=ntabs-1;dirty=1;return 0;
}
static void closetab(int index){
 if(index<0||index>=ntabs)return;
 Tab *t=tabs[index];close(t->input_write);
 if(process_mode){kill(t->worker_pid,SIGHUP);waitpid(t->worker_pid,NULL,0);}else pthread_join(t->thread,NULL);
 close(t->output_read);free(t);
 for(int i=index;i<ntabs-1;i++) tabs[i]=tabs[i+1];
 ntabs--;
 if(active>=ntabs) active=ntabs-1;
 dirty=1;
}
static void sendline(Tab *t);
static void load_bar_font(void){
 if(barfont)XftFontClose(display,barfont);
 /* Independently sized from terminal zoom; large enough on HiDPI. */
 barfont=XftFontOpenName(display,DefaultScreen(display),"sans:pixelsize=46");
 if(!barfont)barfont=XftFontOpenName(display,DefaultScreen(display),"monospace:pixelsize=46");
}
static void load_font(void){
 if(font) XftFontClose(display,font);
 char name[128]; snprintf(name,sizeof(name),"monospace:size=%d",font_size);
 font=XftFontOpenName(display,DefaultScreen(display),name);
 if(!font){fprintf(stderr,"Cannot load Xft monospace font\n");exit(1);}
 line_height=font->ascent+font->descent+5;
 XGlyphInfo ext;XftTextExtentsUtf8(display,font,(FcChar8*)"M",1,&ext);
 char_width=ext.xOff>0?ext.xOff:12;
 dirty=1;
}
static void drawtext(int x,int y,const char *s,int n){if(n>0)XftDrawStringUtf8(xdraw,&fg,font,x,y,(const FcChar8*)s,n);}
static int row_index_at(int y){int r=(y-BAR)/line_height;return r<0?0:r;}
static int text_index_at(int x,int y){
 if(!view_lines||!view_count)return 0;
 int row=view_start+row_index_at(y);
 if(row<0) row=0;
 if(row>=view_count) row=view_count-1;
 int start=view_lines[row],end=row+1<view_count?view_lines[row+1]:0;
 if(row+1==view_count)end=(int)tabs[active]->outlen+(int)tabs[active]->len;
 int col=(x-LEFT)/char_width;if(col<0)col=0;
 int idx=start+col;return idx>end?end:idx;
}
static char *combined_text(Tab *t){
 char *b=malloc(t->outlen+t->len+1);if(!b)return NULL;
 memcpy(b,t->output,t->outlen);memcpy(b+t->outlen,t->input,t->len);
 b[t->outlen+t->len]=0;return b;
}
static void save_selection(void){
 if(!ntabs||sel_start<0||sel_end<0)return;
 Tab *t=tabs[active];char *all=combined_text(t);if(!all)return;
 int n=(int)(t->outlen+t->len),a=sel_start,b=sel_end;
 if(a>b){int z=a;a=b;b=z;}if(a>n)a=n;if(b>n)b=n;
 free(selected);selected=strndup(all+a,(size_t)(b-a));free(all);
 XSetSelectionOwner(display,XA_PRIMARY,window,CurrentTime);
 XSetSelectionOwner(display,clipboard_atom,window,CurrentTime);
 dirty=1;
}
static void insert_text(Tab *t,const char *s,size_t n){
 for(size_t i=0;i<n;i++){
  unsigned char c=(unsigned char)s[i];if(c=='\n'||c=='\r'){sendline(t);continue;}
  if(c<32||t->len>=INPUT_MAX-2)continue;
  memmove(t->input+t->cursor+1,t->input+t->cursor,t->len-t->cursor+1);
  t->input[t->cursor++]=(char)c;t->len++;
 }dirty=1;
}

static int tab_step(void){
 int available=width-LEFT*2-46;
 int step=available/(ntabs>0?ntabs:1);
 if(step>300)step=300;
 if(step<190)step=190;
 return step;
}
static void draw_bar_label(int x,int y,const char *label){
 XftFont *f=barfont?barfont:font;
 if(f)XftDrawStringUtf8(xdraw,&fg,f,x,y,(const FcChar8*)label,(int)strlen(label));
}
static void redraw(void){
 int label_baseline=barfont? (BAR-(barfont->ascent+barfont->descent))/2+barfont->ascent : BAR/2;
 XSetForeground(display,gc,black);XFillRectangle(display,window,gc,0,0,(unsigned)width,(unsigned)height);
 XSetForeground(display,gc,blue);XFillRectangle(display,window,gc,0,0,(unsigned)width,BAR);
 int step=tab_step();
 for(int i=0;i<ntabs;i++){
  int x=LEFT+i*step;
  char label[32];snprintf(label,sizeof(label),"Tab %d",tabs[i]->id);
  if(i==active){XSetForeground(display,gc,white);XDrawRectangle(display,window,gc,x-4,8,(unsigned)(step-4),BAR-18);}
  /* Clip each label so it cannot overwrite the close button or adjacent tabs. */
  XRectangle clip={(short)(x+8),8,(unsigned short)(step-53),(unsigned short)(BAR-16)};
  XftDrawSetClipRectangles(xdraw,0,0,&clip,1);
  draw_bar_label(x+10,label_baseline,label);
  XftDrawSetClip(xdraw,NULL);
  draw_bar_label(x+step-30,label_baseline,"x");
 }
 if(LEFT+ntabs*step<width-16)draw_bar_label(LEFT+ntabs*step+8,label_baseline,"+");
 if(!ntabs){XFlush(display);dirty=0;return;}
 Tab *t=tabs[active];int rows=(height-BAR-8)/line_height;if(rows<1)rows=1;
 int cols=(width-2*LEFT)/char_width;if(cols<8)cols=8;
 char *all=combined_text(t);if(!all)return;
 int length=(int)(t->outlen+t->len);
 free(view_lines);view_lines=malloc((size_t)(length+2)*sizeof(int));
 if(!view_lines){free(all);return;}
 int count=1,col=0;view_lines[0]=0;
 for(int i=0;i<length;i++){
  if(all[i]=='\n'){view_lines[count++]=i+1;col=0;}
  else if(++col>=cols){view_lines[count++]=i+1;col=0;}
 }
 view_count=count;view_cols=cols;view_rows=rows;
 int end=count-t->scroll;if(end<0)end=0;if(end>count)end=count;
 int first=end-rows;if(first<0)first=0;view_start=first;view_end=end;
 for(int r=first;r<end;r++){
  int a=view_lines[r],b=(r+1<count?view_lines[r+1]:length);
  while(b>a&&(all[b-1]=='\n'||all[b-1]=='\r'))b--;
  int y=BAR+font->ascent+4+line_height*(r-first);
  if(sel_start>=0&&sel_end>=0&&sel_start!=sel_end){
   int lo=sel_start<sel_end?sel_start:sel_end,hi=sel_start>sel_end?sel_start:sel_end;
   int l=lo>a?lo:a,h=hi<b?hi:b;
   if(h>l){XSetForeground(display,gc,blue);XFillRectangle(display,window,gc,LEFT+(l-a)*char_width,y-font->ascent,(unsigned)((h-l)*char_width),(unsigned)line_height);}
  }
  drawtext(LEFT,y,all+a,b-a);
 }
 if(t->searching){
  char label[INPUT_MAX+64];snprintf(label,sizeof(label),"(reverse-i-search)`%.100s': %.400s",t->search,t->search_match);
  XSetForeground(display,gc,blue);XFillRectangle(display,window,gc,0,(height>line_height?height-line_height:0),(unsigned)width,(unsigned)line_height);
  drawtext(LEFT,height-6,label,(int)strlen(label));
 }
 if(t->scroll==0){
  int cursor=(int)t->outlen+(int)t->cursor,row=0;
  for(int r=0;r<count;r++){if(view_lines[r]<=cursor)row=r;else break;}
  if(cursor_visible&&row>=first&&row<end){int x=LEFT+(cursor-view_lines[row])*char_width;
   int y=BAR+font->ascent+4+line_height*(row-first);
   XSetForeground(display,gc,white);XDrawLine(display,window,gc,x,y-font->ascent,x,y+font->descent);
  }
 }
 free(all);XFlush(display);dirty=0;
}
static void sendline(Tab *t){
 char line[INPUT_MAX];size_t n=t->len;if(n>=sizeof(line)-1)n=sizeof(line)-2;
 memcpy(line,t->input,n);line[n]='\n';(void)write(t->input_write,line,n+1);
 append(t,t->input,t->len);append(t,"\n",1);
 t->len=t->cursor=0;t->input[0]=0;t->histpos=0;
}
static void refresh_search(Tab *t){
 t->search_match[0]=0;char h[INPUT_MAX];unsigned long count=history_count();
 for(unsigned long i=0;i<count;i++)if(history_get_back((int)i,h,sizeof(h))==0&&strstr(h,t->search)){
  snprintf(t->search_match,sizeof(t->search_match),"%s",h);break;
 }dirty=1;
}
static void paste_from(Atom selection){
 CLIPLOG("paste requested selection=%lu owner=%lu\n",(unsigned long)selection,(unsigned long)XGetSelectionOwner(display,selection));
 paste_selection=selection;paste_fallback=0;paste_incr=0;paste_total=0;
 XDeleteProperty(display,window,paste_atom);XConvertSelection(display,selection,utf8_atom,paste_atom,window,CurrentTime);XFlush(display);
}
static void keypress(XKeyEvent *event){
 if(!ntabs) return;
 Tab *t=tabs[active];KeySym sym;char buf[64];int n=XLookupString(event,buf,sizeof(buf),&sym,NULL);
 int ctrl=event->state&ControlMask,shift=event->state&ShiftMask;
 if(t->searching){
  if(sym==XK_Escape||(ctrl&&(sym==XK_g||sym==XK_G))){t->searching=0;dirty=1;return;}
  if(sym==XK_Return||sym==XK_KP_Enter){if(t->search_match[0]){snprintf(t->input,sizeof(t->input),"%s",t->search_match);t->len=t->cursor=strlen(t->input);}t->searching=0;dirty=1;return;}
  if(sym==XK_BackSpace){if(t->search_len)t->search[--t->search_len]=0;refresh_search(t);return;}
  if(n>0&&!ctrl&&t->search_len+(size_t)n<sizeof(t->search)){memcpy(t->search+t->search_len,buf,(size_t)n);t->search_len+=(size_t)n;t->search[t->search_len]=0;refresh_search(t);}return;
 }
 if(hotkey_match(9,event,sym)){save_selection();return;}
 if(hotkey_match(10,event,sym)||(ctrl&&shift&&(sym==XK_v||sym==XK_V))||(shift&&sym==XK_Insert)){
  CLIPLOG("paste shortcut detected keysym=%lu state=0x%x\n",(unsigned long)sym,event->state);
  paste_from((shift&&sym==XK_Insert)?XA_PRIMARY:clipboard_atom);return;
 }
 if(hotkey_match(3,event,sym)){if(font_size<40){font_size+=2;load_font();}return;}
 if(hotkey_match(4,event,sym)){if(font_size>9){font_size-=2;load_font();}return;}
 if(hotkey_match(5,event,sym)){font_size=INITIAL_FONT;load_font();return;}
 if(hotkey_match(0,event,sym)){newtab();return;}
 if(hotkey_match(1,event,sym)){closetab(active);if(!ntabs)quit_requested=1;return;}
 if(hotkey_match(2,event,sym)){active=(active+1)%ntabs;dirty=1;return;}
 if(hotkey_match(7,event,sym)){if(t->shell&&t->shell->fg_pgid)shell_signal_fg(t->shell,SIGINT);else {t->len=t->cursor=0;t->input[0]=0; (void)write(t->input_write,"\003\n",2);}dirty=1;return;}
 if(hotkey_match(8,event,sym)){if(t->shell)shell_signal_fg(t->shell,SIGTSTP);return;}
 if(hotkey_match(6,event,sym)){
  /* Clear the scrollback, but retain the latest visible shell prompt and
   * the editable input buffer. The prompt can be preceded by output on
   * the same line, so search backwards for the most recent prompt. */
  const char *prefix="user@terminator:";
  size_t prefix_len=strlen(prefix),found=t->outlen;
  for(size_t i=0;i+prefix_len<=t->outlen;i++)
   if(!memcmp(t->output+i,prefix,prefix_len))found=i;
  if(found<t->outlen){
   size_t end=found;
   while(end<t->outlen && t->output[end]!='\n')end++;
   size_t count=end-found;
   memmove(t->output,t->output+found,count);
   t->outlen=count;t->output[count]=0;
  }else{
   t->outlen=0;t->output[0]=0;
  }
  t->scroll=0;dirty=1;return;
 }
 if(hotkey_match(11,event,sym)){t->searching=1;t->search_len=0;t->search[0]=0;refresh_search(t);return;}
 /* Readline-style editing shortcuts. Ctrl+W must not close a tab. */
 if(ctrl&&!shift){
  if(sym==XK_a||sym==XK_A){t->cursor=0;dirty=1;return;}
  if(sym==XK_e||sym==XK_E){t->cursor=t->len;dirty=1;return;}
  if(sym==XK_u||sym==XK_U){memmove(t->input,t->input+t->cursor,t->len-t->cursor+1);t->len-=t->cursor;t->cursor=0;dirty=1;return;}
  if(sym==XK_k||sym==XK_K){t->len=t->cursor;t->input[t->len]=0;dirty=1;return;}
  if(sym==XK_w||sym==XK_W){size_t end=t->cursor;while(t->cursor&&isspace((unsigned char)t->input[t->cursor-1]))t->cursor--;while(t->cursor&&!isspace((unsigned char)t->input[t->cursor-1]))t->cursor--;memmove(t->input+t->cursor,t->input+end,t->len-end+1);t->len-=end-t->cursor;dirty=1;return;}
  if(sym==XK_d||sym==XK_D){if(t->len==0){insert_text(t,"exit",4);sendline(t);}else if(t->cursor<t->len){memmove(t->input+t->cursor,t->input+t->cursor+1,t->len-t->cursor);t->len--;dirty=1;}return;}
 }
 if(sym==XK_Return||sym==XK_KP_Enter){sendline(t);return;}
 if(sym==XK_BackSpace){if(t->cursor){memmove(t->input+t->cursor-1,t->input+t->cursor,t->len-t->cursor+1);t->cursor--;t->len--;}dirty=1;return;}
 if(sym==XK_Delete){if(t->cursor<t->len){memmove(t->input+t->cursor,t->input+t->cursor+1,t->len-t->cursor);t->len--;}dirty=1;return;}
 if(sym==XK_Left){if(t->cursor)t->cursor--;dirty=1;return;}
 if(sym==XK_Right){if(t->cursor<t->len)t->cursor++;dirty=1;return;}
 if(sym==XK_Home){t->cursor=0;dirty=1;return;}
 if(sym==XK_End){t->cursor=t->len;dirty=1;return;}
 if(sym==XK_Page_Up){t->scroll+=15;dirty=1;return;}
 if(sym==XK_Page_Down){t->scroll=t->scroll>15?t->scroll-15:0;dirty=1;return;}
 if(sym==XK_Up||sym==XK_Down){char h[INPUT_MAX];int pos=t->histpos+(sym==XK_Up?1:-1);if(pos<0)pos=0;if(pos>0&&history_get_back(pos-1,h,sizeof(h))==0){snprintf(t->input,sizeof(t->input),"%s",h);t->len=t->cursor=strlen(t->input);t->histpos=pos;}else if(pos==0){t->input[0]=0;t->len=t->cursor=0;t->histpos=0;}dirty=1;return;}
 if(ctrl)return;
 insert_text(t,buf,(size_t)n);
}
int main(int argc,char **argv){
 process_mode=argc>1&&!strcmp(argv[1],"useProcess=True");
 /* Always start each tab in the home directory, not the launch folder. */
 const char *startup_home=getenv("HOME");
 if(startup_home&&*startup_home&&chdir(startup_home)!=0)perror("terminator: chdir HOME");
 if(history_init(NULL)<0){fprintf(stderr,"history initialization failed\n");return 1;}
 display=XOpenDisplay(NULL);if(!display){fprintf(stderr,"Cannot open X11 display\n");return 1;}
 int screen=DefaultScreen(display);black=BlackPixel(display,screen);white=WhitePixel(display,screen);
 XColor c,exact;if(XAllocNamedColor(display,DefaultColormap(display,screen),"#245b91",&c,&exact))blue=c.pixel;else blue=white;
 window=XCreateSimpleWindow(display,RootWindow(display,screen),80,60,width,height,0,white,black);
 XStoreName(display,window,"Terminator - X11 Shell");
 XSelectInput(display,window,ExposureMask|KeyPressMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask|StructureNotifyMask|PropertyChangeMask);
 gc=XCreateGC(display,window,0,NULL);
 xdraw=XftDrawCreate(display,window,DefaultVisual(display,screen),DefaultColormap(display,screen));
 XRenderColor color={65535,65535,65535,65535};
 XftColorAllocValue(display,DefaultVisual(display,screen),DefaultColormap(display,screen),&color,&fg);
 load_font();
 load_bar_font();
 load_hotkeys();
 clipboard_debug=getenv("TERMINATOR_DEBUG_CLIPBOARD")!=NULL;
 terminal_pointer=XCreateFontCursor(display,XC_xterm);
 tab_pointer=XCreateFontCursor(display,XC_left_ptr);
 XDefineCursor(display,window,terminal_pointer);
 clipboard_atom=XInternAtom(display,"CLIPBOARD",False);
 utf8_atom=XInternAtom(display,"UTF8_STRING",False);
 targets_atom=XInternAtom(display,"TARGETS",False);
 paste_atom=XInternAtom(display,"TERMINATOR_PASTE",False);
 incr_atom=XInternAtom(display,"INCR",False);
 compound_atom=XInternAtom(display,"COMPOUND_TEXT",False);

 XMapWindow(display,window);newtab();int xfd=ConnectionNumber(display);int running=1;long long blink_time=monotonic_ms();
 while(running&&!quit_requested){struct pollfd fds[MAX_TABS+1];fds[0]=(struct pollfd){.fd=xfd,.events=POLLIN};for(int i=0;i<ntabs;i++)fds[i+1]=(struct pollfd){.fd=tabs[i]->output_read,.events=POLLIN|POLLHUP};
  int rc=poll(fds,(nfds_t)(ntabs+1),100);if(rc<0&&errno!=EINTR)break;
  for(int i=0;i<ntabs;i++)if(fds[i+1].revents&(POLLIN|POLLHUP)){char b[4096];ssize_t n;while((n=read(tabs[i]->output_read,b,sizeof(b)))>0)append(tabs[i],b,(size_t)n);
   if(n==0 || (fds[i+1].revents&POLLHUP)){closetab(i);i--;if(!ntabs)quit_requested=1;}
  }
  while(XPending(display)){XEvent ev;XNextEvent(display,&ev);
   if(ev.type==Expose)dirty=1;
   else if(ev.type==ConfigureNotify){width=ev.xconfigure.width;height=ev.xconfigure.height;dirty=1;}
   else if(ev.type==KeyPress)keypress(&ev.xkey);
   else if(ev.type==SelectionRequest){
    XSelectionRequestEvent *q=&ev.xselectionrequest;
    XEvent reply={0};reply.xselection.type=SelectionNotify;reply.xselection.display=display;
    reply.xselection.requestor=q->requestor;reply.xselection.selection=q->selection;
    reply.xselection.target=q->target;reply.xselection.time=q->time;reply.xselection.property=None;
    Atom prop=q->property==None?q->target:q->property;
    if(selected){
     if(q->target==targets_atom){Atom targets[]={utf8_atom,XA_STRING,targets_atom};XChangeProperty(display,q->requestor,prop,XA_ATOM,32,PropModeReplace,(unsigned char*)targets,3);reply.xselection.property=prop;}
     else if(q->target==utf8_atom||q->target==XA_STRING){XChangeProperty(display,q->requestor,prop,q->target,8,PropModeReplace,(unsigned char*)selected,(int)strlen(selected));reply.xselection.property=prop;}
    }
    XSendEvent(display,q->requestor,False,0,&reply);
   }
   else if(ev.type==SelectionNotify&&ev.xselection.property==None&&ntabs){
    CLIPLOG("selection notify NONE target=%lu fallback=%d\n",(unsigned long)ev.xselection.target,paste_fallback);
    if(paste_fallback==0){paste_fallback=1;XConvertSelection(display,paste_selection,XA_STRING,paste_atom,window,CurrentTime);XFlush(display);}
    else if(paste_fallback==1&&paste_selection==clipboard_atom){paste_fallback=2;paste_selection=XA_PRIMARY;XConvertSelection(display,XA_PRIMARY,utf8_atom,paste_atom,window,CurrentTime);XFlush(display);}
   }
   else if(ev.type==SelectionNotify&&ev.xselection.property!=None&&ntabs){
    CLIPLOG("selection notify property=%lu\n",(unsigned long)ev.xselection.property);
    Atom type=None;int format=0;unsigned long nitems=0,after=0;unsigned char *data=NULL;
    if(XGetWindowProperty(display,window,paste_atom,0,0,False,AnyPropertyType,&type,&format,&nitems,&after,&data)==Success){
     if(data)XFree(data);
     if(type==incr_atom){
      /* X11 INCR: delete the property to acknowledge, then read chunks. */
      paste_incr=1;paste_total=0;XDeleteProperty(display,window,paste_atom);XFlush(display);
     }else{
      /* Read all bytes of a normal clipboard transfer, not only its first chunk. */
      data=NULL;
      if(XGetWindowProperty(display,window,paste_atom,0,INPUT_MAX,True,AnyPropertyType,&type,&format,&nitems,&after,&data)==Success){
       CLIPLOG("clipboard data type=%lu format=%d bytes=%lu after=%lu\n",(unsigned long)type,format,nitems,after);
       if(data&&format==8)insert_text(tabs[active],(char*)data,nitems);
       if(data)XFree(data);
      }
     }
    }
   }
   else if(ev.type==PropertyNotify&&paste_incr&&ev.xproperty.window==window&&
           ev.xproperty.atom==paste_atom&&ev.xproperty.state==PropertyNewValue){
    Atom type=None;int format=0;unsigned long nitems=0,after=0;unsigned char *data=NULL;
    if(XGetWindowProperty(display,window,paste_atom,0,INPUT_MAX,True,AnyPropertyType,&type,&format,&nitems,&after,&data)==Success){
     if(nitems==0){paste_incr=0;}
     else if(format==8&&data&&ntabs){
      size_t available=paste_total<INPUT_MAX?INPUT_MAX-paste_total:0;
      size_t take=nitems<available?nitems:available;
      if(take)insert_text(tabs[active],(char*)data,take);
      paste_total+=take;
     }
     if(data)XFree(data);
    }
   }
   else if(ev.type==MotionNotify){
    update_pointer_for_y(ev.xmotion.y);
    if(selecting&&ntabs){sel_end=text_index_at(ev.xmotion.x,ev.xmotion.y);dirty=1;}
   }
   else if(ev.type==ButtonRelease&&ev.xbutton.button==Button1&&selecting){selecting=0;sel_end=text_index_at(ev.xbutton.x,ev.xbutton.y);save_selection();}
   else if(ev.type==ButtonPress){if(ev.xbutton.y<BAR){int step=tab_step();int i=(ev.xbutton.x-LEFT)/step;
    if(ev.xbutton.x>=LEFT && i>=0&&i<ntabs){int local=ev.xbutton.x-(LEFT+i*step);if(local>=step-42){closetab(i);if(!ntabs)quit_requested=1;}else{active=i;dirty=1;}}
    else if(ev.xbutton.x>=LEFT && i==ntabs)newtab();}else if(ntabs){if(ev.xbutton.button==Button1){selecting=1;sel_start=sel_end=text_index_at(ev.xbutton.x,ev.xbutton.y);dirty=1;}if(ev.xbutton.button==Button2){paste_from(XA_PRIMARY);}if(ev.xbutton.button==4)tabs[active]->scroll+=3;if(ev.xbutton.button==5)tabs[active]->scroll=tabs[active]->scroll>3?tabs[active]->scroll-3:0;dirty=1;}}
  }
  if(monotonic_ms()-blink_time>=500){cursor_visible=!cursor_visible;blink_time=monotonic_ms();dirty=1;}
  if(dirty)redraw();
 }
 while(ntabs) closetab(ntabs-1);
 free(selected);free(view_lines);if(barfont)XftFontClose(display,barfont);if(font)XftFontClose(display,font);XftColorFree(display,DefaultVisual(display,screen),DefaultColormap(display,screen),&fg);XftDrawDestroy(xdraw);
 XFreeCursor(display,terminal_pointer);XFreeCursor(display,tab_pointer);
 XFreeGC(display,gc);XDestroyWindow(display,window);XCloseDisplay(display);return 0;
}
