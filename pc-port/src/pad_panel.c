// SPDX-License-Identifier: GPL-2.0-or-later
// The touchpad and spray window (F5 in game): a small always-on-top window that never takes the
// focus from the game. TOUCHPAD: a touchpad to press and drag with the mouse, the sides, click and
// swipes of PhonePad Desktop, and the mouse as touchpad (F6). SPRAY: the spray can of the graffiti
// missions (F7, src/pad.c), its sensitivity and a guide. Its thread also reads the mouse's raw
// counts (Raw Input) for the spray can.
#include "pad_panel.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

enum {B_TAB_TOUCH,B_TAB_SPRAY,B_CLOSE,B_LEFT,B_BOTH,B_RIGHT,B_CLICK,B_UP,B_DOWN,B_SWLEFT,B_SWRIGHT,B_MOUSE,
      B_SPRAY,B_RECENTRE,B_LESS,B_MORE,B_COUNT};
static const wchar_t *const labels[B_COUNT]={L"TOUCHPAD",L"SPRAY",L"\x2715",L"Left",L"Both",L"Right",L"Click",
    L"\x2191  Swipe",L"\x2193  Swipe",L"\x2190  Swipe",L"\x2192  Swipe",L"Mouse as touchpad in the game   (F6)",
    L"START SPRAY MODE   (F7)",L"Recenter",L"\x2212",L"+"};
static const wchar_t *const touch_guide=
    L"Drag on the pad: a finger.  Right button: click.  Shift: two fingers.\n"
    L"Left / Right / Both: press that side of the touchpad (the game's touchpad prompts). Click: the middle.\n"
    L"Swipes: the game's swipe prompts.\n"
    L"Mouse as touchpad (F6): in the game window the left button is a finger, the right button clicks.";
static const wchar_t *const spray_guide=
    L"For the graffiti missions:\n\n"
    L"1.  At a graffiti spot, press START SPRAY MODE (or F7). The controller is held like a spray can "
    L"and the cursor is hidden in the game window.\n\n"
    L"2.  Shake the can: hold the right mouse button.\n\n"
    L"3.  Spray: hold the left mouse button and move the mouse to paint. Shift: fine aim. "
    L"Middle button: recenter.\n\n"
    L"4.  Press F7 to end spray mode and get the cursor back.";
enum {W=420,H=505,TITLE=30,PAD_X=12,PAD_Y=74,PAD_W=396,PAD_H=194}; // 96 DPI units; the pad is 1920:942
enum {WM_PANEL_TOGGLE=WM_APP+40,WM_PANEL_RAW,WM_PANEL_MODE};

static SRWLOCK panel_lock=SRWLOCK_INIT;
static HWND panel;
static HANDLE panel_thread,panel_ready;
static int placed,shown,pressed=-1,mode_shown,tab,raw_requested;
static PadPanel state={.swipe=-1,.mode=-1,.sensitivity=1};
static long long raw_x,raw_y;
static ULONGLONG raw_last;
static float scale=1;

static int S(int v){return (int)(v*scale+.5f);}
static int visible(int b){return b<=B_CLOSE||(tab==0?b>=B_LEFT&&b<=B_MOUSE:b>=B_SPRAY);}
static RECT button_rect(int b){
    RECT r={0,0,0,0};
    switch(b){
    case B_TAB_TOUCH:r=(RECT){12,38,206,66};break;
    case B_TAB_SPRAY:r=(RECT){214,38,408,66};break;
    case B_CLOSE:r=(RECT){W-36,3,W-6,TITLE-3};break;
    case B_MOUSE:r=(RECT){12,354,408,384};break;
    case B_SPRAY:r=(RECT){12,74,408,124};break;
    case B_RECENTRE:r=(RECT){12,134,206,164};break;
    case B_LESS:r=(RECT){214,134,254,164};break;
    case B_MORE:r=(RECT){368,134,408,164};break;
    default:{const int i=b-B_LEFT,row=i/4,col=i%4;r.left=PAD_X+col*101;r.top=278+row*38;r.right=r.left+93;r.bottom=r.top+30;}
    }
    r.left=S(r.left);r.top=S(r.top);r.right=S(r.right);r.bottom=S(r.bottom);return r;
}
static RECT pad_rect(void){RECT r={S(PAD_X),S(PAD_Y),S(PAD_X+PAD_W),S(PAD_Y+PAD_H)};return r;}
static int hit_button(int x,int y){
    for(int b=0;b<B_COUNT;++b){if(!visible(b))continue;RECT r=button_rect(b);if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom)return b;}
    return -1;
}
static int in_pad(int x,int y){RECT r=pad_rect();return tab==0&&x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}
static void set_finger(int x,int y){
    RECT r=pad_rect();float fx=(float)(x-r.left)/(float)(r.right-r.left),fy=(float)(y-r.top)/(float)(r.bottom-r.top);
    state.x=fx<0?0:fx>1?1:fx;state.y=fy<0?0:fy>1?1:fy;state.two=(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0;
}
static void release_all(void){
    AcquireSRWLockExclusive(&panel_lock);
    state.touch=state.two=state.click=state.left=state.right=state.both=state.centre=0;pressed=-1;
    ReleaseSRWLockExclusive(&panel_lock);
}
static void press(int b,int down){
    AcquireSRWLockExclusive(&panel_lock);
    switch(b){
    case B_LEFT:state.left=down;break;case B_BOTH:state.both=down;break;case B_RIGHT:state.right=down;break;
    case B_CLICK:state.centre=down;break;
    case B_UP:case B_DOWN:case B_SWLEFT:case B_SWRIGHT:if(down)state.swipe=b-B_UP;break;
    case B_RECENTRE:if(down)state.recentre=1;break;
    case B_SPRAY:if(down)state.mode=2;break;
    case B_MOUSE:if(down)state.mode=1;break;
    case B_LESS:if(down&&state.sensitivity>.25f)state.sensitivity-=.1f;break;
    case B_MORE:if(down&&state.sensitivity<1.95f)state.sensitivity+=.1f;break;
    case B_TAB_TOUCH:if(down)tab=0;break;
    case B_TAB_SPRAY:if(down)tab=1;break;
    default:break;
    }
    ReleaseSRWLockExclusive(&panel_lock);
}

static void fill(HDC dc,RECT r,COLORREF c,int round){
    HBRUSH b=CreateSolidBrush(c);HPEN p=CreatePen(PS_SOLID,1,c);
    HGDIOBJ ob=SelectObject(dc,b),op=SelectObject(dc,p);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,round,round);
    SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(b);DeleteObject(p);
}
static void text(HDC dc,const wchar_t *s,RECT r,COLORREF c,UINT align){SetTextColor(dc,c);DrawTextW(dc,s,-1,&r,align|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);}
static void paint(HWND window){
    PAINTSTRUCT ps;HDC screen=BeginPaint(window,&ps);RECT all;GetClientRect(window,&all);
    HDC dc=CreateCompatibleDC(screen);HBITMAP bmp=CreateCompatibleBitmap(screen,all.right,all.bottom);HGDIOBJ old_bmp=SelectObject(dc,bmp);
    HFONT font=CreateFontW(-S(13),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    HFONT small_font=CreateFontW(-S(12),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    HGDIOBJ old_font=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
    AcquireSRWLockShared(&panel_lock);const PadPanel s=state;const int down=pressed,mode=mode_shown;ReleaseSRWLockShared(&panel_lock);
    const COLORREF blue=RGB(0,112,209),fg=RGB(232,234,238),dim=RGB(150,156,168);
    fill(dc,all,RGB(22,24,29),0);
    RECT bar={0,0,all.right,S(TITLE)};fill(dc,bar,RGB(14,15,18),0);
    RECT title={S(12),0,S(240),S(TITLE)};text(dc,L"TOUCHPAD & SPRAY",title,fg,DT_LEFT);
    SelectObject(dc,small_font);RECT hint={S(172),0,S(W-44),S(TITLE)};text(dc,L"F5 shows / hides",hint,dim,DT_LEFT);SelectObject(dc,font);
    if(tab==0){
        // The pad: the finger(s) while pressed, the middle line between the two sides.
        RECT pad=pad_rect();fill(dc,pad,s.click?RGB(30,50,78):RGB(35,38,45),S(14));
        HPEN line=CreatePen(PS_DOT,1,RGB(58,63,74));HGDIOBJ op=SelectObject(dc,line);
        MoveToEx(dc,(pad.left+pad.right)/2,pad.top+S(10),NULL);LineTo(dc,(pad.left+pad.right)/2,pad.bottom-S(10));SelectObject(dc,op);DeleteObject(line);
        if(s.touch){for(int f=0;f<(s.two?2:1);++f){const float fx=f?1-s.x:s.x;
                const int cx=pad.left+(int)(fx*(float)(pad.right-pad.left)),cy=pad.top+(int)(s.y*(float)(pad.bottom-pad.top));
                RECT dot={cx-S(11),cy-S(11),cx+S(11),cy+S(11)};fill(dc,dot,blue,S(22));}}
    }else{
        RECT sens={S(254),S(134),S(368),S(164)};wchar_t value[32];swprintf(value,32,L"Speed %d%%",(int)(s.sensitivity*100+.5f));
        text(dc,value,sens,fg,DT_CENTER);
    }
    for(int b=0;b<B_COUNT;++b){if(!visible(b))continue;RECT r=button_rect(b);
        const int tab_on=(b==B_TAB_TOUCH&&tab==0)||(b==B_TAB_SPRAY&&tab==1);
        const int active=b==down||(b==B_SPRAY&&mode==2)||(b==B_MOUSE&&mode==1)||(b==B_LEFT&&s.left)||(b==B_RIGHT&&s.right)||
                         (b==B_BOTH&&s.both)||(b==B_CLICK&&s.centre);
        COLORREF c=b==B_CLOSE?(active?RGB(196,43,28):RGB(14,15,18)):active?blue:RGB(42,46,54);
        if(b<=B_TAB_SPRAY)c=tab_on?RGB(52,58,70):RGB(30,33,40);
        fill(dc,r,c,b==B_CLOSE?0:S(8));
        if(b<=B_TAB_SPRAY&&tab_on){RECT u={r.left+S(8),r.bottom-S(3),r.right-S(8),r.bottom};fill(dc,u,blue,0);}
        const wchar_t *label=b==B_SPRAY&&mode==2?L"SPRAY MODE ON   \x00B7   F7 TO STOP":b==B_MOUSE&&mode==1?L"Mouse as touchpad: ON   (F6 to stop)":labels[b];
        text(dc,label,r,b<=B_TAB_SPRAY&&!tab_on?dim:fg,DT_CENTER);}
    // The guide of the tab.
    SelectObject(dc,small_font);SetTextColor(dc,dim);
    RECT guide=tab==0?(RECT){S(14),S(394),all.right-S(14),all.bottom-S(8)}:(RECT){S(14),S(178),all.right-S(14),all.bottom-S(8)};
    DrawTextW(dc,tab==0?touch_guide:spray_guide,-1,&guide,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
    BitBlt(screen,0,0,all.right,all.bottom,dc,0,0,SRCCOPY);
    SelectObject(dc,old_font);SelectObject(dc,old_bmp);DeleteObject(font);DeleteObject(small_font);DeleteObject(bmp);DeleteDC(dc);
    EndPaint(window,&ps);
}

static void read_raw(HRAWINPUT handle){
    RAWINPUT input;UINT size=sizeof(input);
    if(GetRawInputData(handle,RID_INPUT,&input,&size,sizeof(RAWINPUTHEADER))==(UINT)-1||input.header.dwType!=RIM_TYPEMOUSE)return;
    if(input.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)return; // tablets and remote sessions: not deltas
    AcquireSRWLockExclusive(&panel_lock);raw_x+=input.data.mouse.lLastX;raw_y+=input.data.mouse.lLastY;raw_last=GetTickCount64();
    ReleaseSRWLockExclusive(&panel_lock);
}
static LRESULT CALLBACK panel_proc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    const int x=(short)LOWORD(lp),y=(short)HIWORD(lp);
    switch(message){
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE; // the game keeps the focus (and the keyboard)
    case WM_NCHITTEST:{POINT p={x,y};ScreenToClient(window,&p);
        if(p.y<S(TITLE)&&hit_button(p.x,p.y)<0)return HTCAPTION;return HTCLIENT;}
    case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:{SetCapture(window);
        if(in_pad(x,y)){AcquireSRWLockExclusive(&panel_lock);state.touch=1;set_finger(x,y);if(message==WM_RBUTTONDOWN)state.click=1;
            ReleaseSRWLockExclusive(&panel_lock);}
        else if(message==WM_LBUTTONDOWN){const int b=hit_button(x,y);
            if(b==B_CLOSE){ShowWindow(window,SW_HIDE);shown=0;release_all();ReleaseCapture();return 0;}
            if(b>=0){press(b,1);AcquireSRWLockExclusive(&panel_lock);pressed=b;ReleaseSRWLockExclusive(&panel_lock);}}
        InvalidateRect(window,NULL,FALSE);return 0;}
    case WM_MOUSEMOVE:
        if(state.touch){AcquireSRWLockExclusive(&panel_lock);set_finger(x,y);ReleaseSRWLockExclusive(&panel_lock);InvalidateRect(window,NULL,FALSE);}
        return 0;
    case WM_LBUTTONUP:case WM_RBUTTONUP:{
        AcquireSRWLockExclusive(&panel_lock);const int b=pressed;pressed=-1;
        if(message==WM_RBUTTONUP)state.click=0;
        if(!(wp&(MK_LBUTTON|MK_RBUTTON)))state.touch=state.two=state.click=0;
        ReleaseSRWLockExclusive(&panel_lock);
        if(b>=0)press(b,0);
        if(!(wp&(MK_LBUTTON|MK_RBUTTON)))ReleaseCapture();
        InvalidateRect(window,NULL,FALSE);return 0;}
    case WM_CAPTURECHANGED:{
        AcquireSRWLockExclusive(&panel_lock);const int b=pressed;ReleaseSRWLockExclusive(&panel_lock);
        if(b==B_LEFT||b==B_BOTH||b==B_RIGHT||b==B_CLICK||state.touch){release_all();InvalidateRect(window,NULL,FALSE);}
        return 0;}
    case WM_INPUT:read_raw((HRAWINPUT)lp);return DefWindowProcW(window,message,wp,lp);
    case WM_PANEL_TOGGLE:
        if(shown){ShowWindow(window,SW_HIDE);shown=0;release_all();return 0;}
        if(!placed&&lp){RECT g;if(GetWindowRect((HWND)lp,&g))SetWindowPos(window,HWND_TOPMOST,g.right-S(W)-S(28),g.top+S(56),0,0,SWP_NOSIZE|SWP_NOACTIVATE);placed=1;}
        SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);shown=1;
        InvalidateRect(window,NULL,FALSE);return 0;
    case WM_PANEL_RAW:{
        // Raw relative counts for the spray can, also while another window has the focus.
        RAWINPUTDEVICE device={1,2,RIDEV_INPUTSINK,window};
        if(!RegisterRawInputDevices(&device,1,sizeof(device)))fprintf(stderr,"PAD_PANEL raw input failed: %lu\n",GetLastError());
        else fprintf(stderr,"PAD_PANEL raw input on\n");
        return 0;}
    case WM_PANEL_MODE:{
        // In spray mode the window shows the spray guide and lets the clicks through to the game
        // (the hidden cursor is held at the game window's centre, which the window may cover).
        AcquireSRWLockExclusive(&panel_lock);mode_shown=(int)wp;if(wp==2)tab=1;ReleaseSRWLockExclusive(&panel_lock);
        LONG_PTR ex=GetWindowLongPtrW(window,GWL_EXSTYLE);
        ex=wp==2?(ex|WS_EX_TRANSPARENT|WS_EX_LAYERED):(ex&~(LONG_PTR)WS_EX_TRANSPARENT);
        SetWindowLongPtrW(window,GWL_EXSTYLE,ex);
        if(ex&WS_EX_LAYERED)SetLayeredWindowAttributes(window,0,wp==2?215:255,LWA_ALPHA);
        InvalidateRect(window,NULL,FALSE);return 0;}
    case WM_PAINT:paint(window);return 0;
    case WM_ERASEBKGND:return 1;
    default:break;
    }
    return DefWindowProcW(window,message,wp,lp);
}
static DWORD WINAPI panel_main(LPVOID param){
    (void)param;
    WNDCLASSEXW wc;memset(&wc,0,sizeof(wc));wc.cbSize=sizeof(wc);wc.lpfnWndProc=panel_proc;wc.hInstance=GetModuleHandleW(NULL);
    wc.hCursor=LoadCursorW(NULL,(LPCWSTR)IDC_ARROW);wc.lpszClassName=L"IssTouchpadPanel";RegisterClassExW(&wc);
    HDC dc=GetDC(NULL);scale=(float)GetDeviceCaps(dc,LOGPIXELSX)/96.f;ReleaseDC(NULL,dc);if(scale<1)scale=1;
    panel=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"Touchpad & Spray",WS_POPUP|WS_BORDER,
                          100,100,S(W),S(H),NULL,NULL,wc.hInstance,NULL);
    SetEvent(panel_ready);
    if(!panel){fprintf(stderr,"PAD_PANEL window failed: %lu\n",GetLastError());return 1;}
    MSG m;while(GetMessageW(&m,NULL,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}
    return 0;
}
static int start(void){
    if(!panel_thread){panel_ready=CreateEventW(NULL,TRUE,FALSE,NULL);panel_thread=CreateThread(NULL,0,panel_main,NULL,0,NULL);
        if(panel_thread)WaitForSingleObject(panel_ready,2000);}
    return panel!=NULL;
}
void pad_panel_toggle(HWND game){if(start())PostMessageW(panel,WM_PANEL_TOGGLE,0,(LPARAM)game);}
HWND pad_panel_window(void){return panel;}
void pad_panel_set_mode(int mode){
    if(panel)PostMessageW(panel,WM_PANEL_MODE,(WPARAM)mode,0);
    else{AcquireSRWLockExclusive(&panel_lock);mode_shown=mode;if(mode==2)tab=1;ReleaseSRWLockExclusive(&panel_lock);}
}
void pad_panel_read(PadPanel *out){
    AcquireSRWLockExclusive(&panel_lock);*out=state;state.swipe=state.mode=-1;state.recentre=0;ReleaseSRWLockExclusive(&panel_lock);
}
int pad_panel_raw(float *dx,float *dy){
    *dx=*dy=0;
    if(!raw_requested){raw_requested=1;if(start())PostMessageW(panel,WM_PANEL_RAW,0,0);return 0;}
    AcquireSRWLockExclusive(&panel_lock);
    // Raw Input counts as delivering while it moved in the last 2 s (another registration in the
    // process, if any, takes it over: then the cursor's movement is used).
    const int live=raw_last&&GetTickCount64()-raw_last<2000;
    if(live){*dx=(float)raw_x;*dy=(float)raw_y;}
    raw_x=raw_y=0;
    ReleaseSRWLockExclusive(&panel_lock);
    return live;
}
