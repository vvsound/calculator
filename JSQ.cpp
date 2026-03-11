/*
 * 计算器 - 单文件 C++ / Win32 API
 * 编译: g++ JSQ.cpp sounds.o -o JSQ.exe -mwindows -lwinmm -municode -std=c++17 -static -static-libgcc -static-libstdc++
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmsystem.h>
#include <string>
#include <cmath>
#include <stdexcept>
#include <vector>

#pragma comment(lib,"winmm.lib")
#pragma comment(linker,"/subsystem:windows")

// ── 资源 ID：WAV 文件嵌入 exe ────────────────────────────
// 100-109 = 0-9.wav, 110=add, 111=sub, 112=mul, 113=div, 114=eq, 115=clr
static void PlayWav(int id) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    HRSRC hr = FindResourceW(hi, MAKEINTRESOURCEW(id), L"WAVE");
    if (!hr) return;
    void* p = LockResource(LoadResource(hi, hr));
    DWORD n  = SizeofResource(hi, hr);
    if (p && n) PlaySoundA((LPCSTR)p, nullptr, SND_MEMORY|SND_ASYNC|SND_NODEFAULT);
}
static void PlayDigit(int d)      { PlayWav(100 + d); }
static void PlayOp(wchar_t op) {
    if (op==L'+') PlayWav(110);
    else if (op==L'-') PlayWav(111);
    else if (op==L'*') PlayWav(112);
    else if (op==L'/') PlayWav(113);
}
static void PlayEqual()  { PlayWav(114); }
static void PlayDelete() { PlayWav(115); }
static void PlayError()  { PlayWav(114); }

// ── 表达式求值 ───────────────────────────────────────────
struct Parser {
    std::wstring s; size_t p=0;
    Parser(const std::wstring& e):s(e){}
    void skip(){ while(p<s.size()&&s[p]==L' ')++p; }
    double parse(){ double v=addSub(); skip(); if(p<s.size())throw std::runtime_error(""); return v; }
    double addSub(){
        double l=mulDiv();
        for(;;){ skip(); if(p>=s.size())break; wchar_t op=s[p];
            if(op!=L'+'&&op!=L'-')break; ++p; double r=mulDiv();
            l=(op==L'+')?l+r:l-r; } return l; }
    double mulDiv(){
        double l=unary();
        for(;;){ skip(); if(p>=s.size())break; wchar_t op=s[p];
            if(op!=L'*'&&op!=L'/')break; ++p; double r=unary();
            if(op==L'/'&&r==0)throw std::runtime_error("div0");
            l=(op==L'*')?l*r:l/r; } return l; }
    double unary(){ skip();
        if(p<s.size()&&s[p]==L'-'){++p;return -primary();}
        if(p<s.size()&&s[p]==L'+')++p; return primary(); }
    double primary(){ skip();
        if(p<s.size()&&s[p]==L'('){++p;double v=addSub();skip();if(p<s.size()&&s[p]==L')')++p;return v;}
        size_t st=p; while(p<s.size()&&(iswdigit(s[p])||s[p]==L'.'))++p;
        if(p==st)throw std::runtime_error(""); return std::stod(s.substr(st,p-st)); }
};

static std::wstring PrepareExpr(std::wstring e) {
    for(auto& c:e){ if(c==L'×')c=L'*'; if(c==L'÷')c=L'/'; }
    std::wstring out;
    for(size_t i=0;i<e.size();++i){
        if(e[i]==L'%'&&i>0){
            size_t j=i-1;
            while(j>0&&(iswdigit(e[j])||e[j]==L'.'))--j;
            if(!iswdigit(e[j]))++j;
            std::wstring num=out.substr(j); out.erase(j);
            out+=L'('+num+L"/100)";
        } else out+=e[i];
    }
    return out;
}

static std::wstring FmtResult(double v) {
    if(v==(long long)v&&std::abs(v)<1e15) return std::to_wstring((long long)v);
    wchar_t buf[64]; swprintf(buf,64,L"%.10g",v); return buf;
}

// ── 主题 ─────────────────────────────────────────────────
struct Theme {
    const wchar_t* name;
    COLORREF bg, dispBg, procFg, resFg;
    COLORREF num, op, eq, del, pct, btnFg;
};
static const Theme THEMES[] = {
    {L"🌙深空",  0x2E1E1E,0x3E2A2A,0x888888,0xFFFFFF,0x442D2D,0xDD78C6,0x79C398,0x756CE0,0xEFAF61,0xFFFFFF},
    {L"☀️米白",  0xE8F0F5,0xDCE8ED,0x889999,0x222222,0xC4D3D9,0xF65C8B,0x5EC222,0x4444EF,0xF68200,0x222222},
    {L"🌿护眼",  0x20241A,0x282D1F,0x809A6A,0xD8F0C8,0x28331A,0x82AF4C,0x6ABB66,0x7373E5,0xACB64D,0xDDEEDD},
    {L"🎰赛博",  0x1A0D0D,0x28111A,0x884444,0xCCFF00,0x3A1A1A,0xFF00FF,0x99FF00,0x6633FF,0xFFCC00,0xEEEEFF},
    {L"🍊暖橙",  0x0E1A2B,0x10233A,0x5588CC,0xCCE8FF,0x10253D,0x42A0FF,0x30C4F4,0x50E0B0,0x5088FF,0xE0F0FF},
};
static int g_theme = 2;

static COLORREF Dim(COLORREF c, double f=0.72){
    return RGB((int)(GetRValue(c)*f),(int)(GetGValue(c)*f),(int)(GetBValue(c)*f));
}

// ── 控件 ID & 按钮定义 ───────────────────────────────────
#define ID_PROC 100
#define ID_RES  101
#define ID_BTN  200

enum BK { BK_NUM, BK_OP, BK_EQ, BK_DEL, BK_PCT };
struct Btn { const wchar_t* lbl; int row,col; BK kind; };
static const Btn BTNS[] = {
    {L"C",0,0,BK_DEL},{L"←",0,1,BK_DEL},{L"%",0,2,BK_PCT},{L"÷",0,3,BK_OP},
    {L"7",1,0,BK_NUM},{L"8",1,1,BK_NUM},{L"9",1,2,BK_NUM},{L"×",1,3,BK_OP},
    {L"4",2,0,BK_NUM},{L"5",2,1,BK_NUM},{L"6",2,2,BK_NUM},{L"-",2,3,BK_OP},
    {L"1",3,0,BK_NUM},{L"2",3,1,BK_NUM},{L"3",3,2,BK_NUM},{L"+",3,3,BK_OP},
    {L"±",4,0,BK_NUM},{L"0",4,1,BK_NUM},{L".",4,2,BK_NUM},{L"=",4,3,BK_EQ},
};
static const int NBTN = 20;

// ── 全局状态 ─────────────────────────────────────────────
static std::wstring g_expr;
static HWND g_hWnd, g_hProc, g_hRes, g_hBtns[NBTN];
static HFONT g_fSmall, g_fLarge, g_fBtn;
static int g_hot = -1;

static void SetProc(const std::wstring& s){ SetWindowTextW(g_hProc,s.c_str()); }
static void SetRes (const std::wstring& s){ SetWindowTextW(g_hRes, s.c_str()); }

static COLORREF BtnCol(int i){
    const Theme& t=THEMES[g_theme];
    switch(BTNS[i].kind){
        case BK_OP:  return t.op;
        case BK_EQ:  return t.eq;
        case BK_DEL: return t.del;
        case BK_PCT: return t.pct;
        default:     return t.num;
    }
}

static void ApplyTheme(){
    const Theme& t=THEMES[g_theme];
    SetClassLongPtrW(g_hWnd,GCLP_HBRBACKGROUND,(LONG_PTR)CreateSolidBrush(t.bg));
    InvalidateRect(g_hWnd,nullptr,TRUE);
    for(int i=0;i<NBTN;i++) InvalidateRect(g_hBtns[i],nullptr,TRUE);
    InvalidateRect(g_hProc,nullptr,TRUE);
    InvalidateRect(g_hRes, nullptr,TRUE);
}

// ── 按钮点击逻辑 ─────────────────────────────────────────
static void OnClick(const wchar_t* lbl){
    wchar_t c=lbl[0];
    // 音效
    if(c>=L'0'&&c<=L'9'&&!lbl[1]) PlayDigit(c-L'0');
    else if(c==L'+') PlayOp(L'+');
    else if(c==L'-') PlayOp(L'-');
    else if(c==L'×') PlayOp(L'*');
    else if(c==L'÷') PlayOp(L'/');
    else if(c==L'←'||c==L'C') PlayDelete();

    // 逻辑
    std::wstring& e=g_expr;
    if(c==L'C'){ e.clear(); SetProc(L""); SetRes(L"0"); }
    else if(c==L'←'){ if(!e.empty())e.pop_back(); SetProc(e); SetRes(e.empty()?L"0":e); }
    else if(c==L'='){
        if(e.empty())return;
        try{
            double r=Parser(PrepareExpr(e)).parse();
            std::wstring out=FmtResult(r);
            SetProc(e+L" ="); e=out; SetRes(out);
            PlayEqual();
        }catch(...){
            PlayError(); SetProc(e+L" = ?"); SetRes(L"错误"); e.clear();
        }
    }
    else if(c==L'+'||c==L'-'||c==L'×'||c==L'÷'){
        if(!e.empty()){ wchar_t l=e.back();
            if(l==L'+'||l==L'-'||l==L'×'||l==L'÷')e.pop_back(); }
        e+=c; SetProc(e); SetRes(e);
    }
    else if(c==L'±'){
        size_t op=e.find_last_of(L"+-×÷");
        size_t ns=(op==std::wstring::npos)?0:op+1;
        std::wstring num=e.substr(ns);
        if(!num.empty()){
            std::wstring pre=e.substr(0,ns);
            e=pre+(num[0]==L'-'?num.substr(1):L"-"+num);
            SetProc(e); SetRes(e);
        }
    }
    else if(c==L'%'){ e+=L'%'; SetProc(e); SetRes(e); }
    else if(c==L'.'){
        size_t op=e.find_last_of(L"+-×÷");
        std::wstring seg=(op==std::wstring::npos)?e:e.substr(op+1);
        if(seg.find(L'.')==std::wstring::npos){
            if(seg.empty())e+=L'0'; e+=L'.'; SetProc(e); SetRes(e);
        }
    }
    else{ e+=c; SetProc(e); SetRes(e); }
}

// ── 按钮子类化（自绘纯色） ───────────────────────────────
static WNDPROC g_origBtn=nullptr;
static LRESULT CALLBACK BtnProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    int idx=(int)GetWindowLongPtrW(hw,GWLP_USERDATA);
    if(msg==WM_PAINT){
        PAINTSTRUCT ps; HDC dc=BeginPaint(hw,&ps);
        RECT rc; GetClientRect(hw,&rc);
        COLORREF col=(g_hot==idx)?Dim(BtnCol(idx)):BtnCol(idx);
        HBRUSH br=CreateSolidBrush(col);
        FillRect(dc,&rc,br); DeleteObject(br);
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,THEMES[g_theme].btnFg);
        SelectObject(dc,g_fBtn);
        wchar_t txt[8]; GetWindowTextW(hw,txt,8);
        DrawTextW(dc,txt,-1,&rc,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        EndPaint(hw,&ps); return 0;
    }
    if(msg==WM_MOUSEMOVE&&g_hot!=idx){
        g_hot=idx;
        TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hw,0}; TrackMouseEvent(&t);
        InvalidateRect(hw,nullptr,FALSE);
    }
    if(msg==WM_MOUSELEAVE){ g_hot=-1; InvalidateRect(hw,nullptr,FALSE); }
    return CallWindowProcW(g_origBtn,hw,msg,wp,lp);
}

// ── 主窗口 ───────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        g_fSmall=CreateFontW(-20,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
        g_fLarge=CreateFontW(-64,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
        g_fBtn  =CreateFontW(-30,0,0,0,FW_BOLD,  0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        g_hProc=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_RIGHT,8,8,384,32,hw,(HMENU)ID_PROC,nullptr,nullptr);
        g_hRes =CreateWindowExW(0,L"STATIC",L"0",WS_CHILD|WS_VISIBLE|SS_RIGHT,8,44,384,88,hw,(HMENU)ID_RES,nullptr,nullptr);
        SendMessageW(g_hProc,WM_SETFONT,(WPARAM)g_fSmall,TRUE);
        SendMessageW(g_hRes, WM_SETFONT,(WPARAM)g_fLarge,TRUE);
        const int BW=92,BH=90,GAP=4,OX=6,OY=142;
        for(int i=0;i<NBTN;i++){
            int x=OX+BTNS[i].col*(BW+GAP), y=OY+BTNS[i].row*(BH+GAP);
            g_hBtns[i]=CreateWindowExW(0,L"BUTTON",BTNS[i].lbl,WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                x,y,BW,BH,hw,(HMENU)(UINT_PTR)(ID_BTN+i),nullptr,nullptr);
            SetWindowLongPtrW(g_hBtns[i],GWLP_USERDATA,i);
            if(!g_origBtn) g_origBtn=(WNDPROC)GetWindowLongPtrW(g_hBtns[i],GWLP_WNDPROC);
            SetWindowLongPtrW(g_hBtns[i],GWLP_WNDPROC,(LONG_PTR)BtnProc);
        }
        ApplyTheme(); return 0;
    }
    case WM_DRAWITEM: return TRUE;
    case WM_CTLCOLORSTATIC:{
        HDC dc=(HDC)wp; HWND h=(HWND)lp; const Theme& t=THEMES[g_theme];
        SetBkColor(dc,t.dispBg);
        SetTextColor(dc,(h==g_hProc)?t.procFg:t.resFg);
        static HBRUSH br=nullptr; if(br)DeleteObject(br);
        br=CreateSolidBrush(t.dispBg); return (LRESULT)br;
    }
    case WM_ERASEBKGND:{
        RECT rc; GetClientRect(hw,&rc);
        HBRUSH br=CreateSolidBrush(THEMES[g_theme].bg);
        FillRect((HDC)wp,&rc,br); DeleteObject(br); return 1;
    }
    case WM_COMMAND:{
        int id=LOWORD(wp);
        if(id>=ID_BTN&&id<ID_BTN+NBTN) OnClick(BTNS[id-ID_BTN].lbl);
        return 0;
    }
    case WM_KEYDOWN:
        if(wp==VK_ESCAPE){DestroyWindow(hw);return 0;}
        if(wp==VK_BACK)  {OnClick(L"←");return 0;}
        if(wp==VK_RETURN){OnClick(L"="); return 0;}
        if(wp==VK_DELETE){OnClick(L"C"); return 0;}
        if(wp=='T'||wp=='t'){g_theme=(g_theme+1)%5;ApplyTheme();return 0;}
        return 0;
    case WM_CHAR:{
        wchar_t c=(wchar_t)wp;
        if(c>=L'0'&&c<=L'9'){wchar_t s[2]={c,0};OnClick(s);}
        else if(c==L'.') OnClick(L".");
        else if(c==L'+') OnClick(L"+");
        else if(c==L'-') OnClick(L"-");
        else if(c==L'*') OnClick(L"×");
        else if(c==L'/') OnClick(L"÷");
        else if(c==L'%') OnClick(L"%");
        else if(c==L'=') OnClick(L"=");
        else if(c==L'c'||c==L'C') OnClick(L"C");
        return 0;
    }
    case WM_DESTROY:
        if(GetKeyState(VK_NUMLOCK)&1){
            keybd_event(VK_NUMLOCK,0x45,0,0);
            keybd_event(VK_NUMLOCK,0x45,KEYEVENTF_KEYUP,0);
        }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw,msg,wp,lp);
}

// ── 入口 ─────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int){
    // 开启 NumLock
    if(!(GetKeyState(VK_NUMLOCK)&1)){
        keybd_event(VK_NUMLOCK,0x45,0,0);
        keybd_event(VK_NUMLOCK,0x45,KEYEVENTF_KEYUP,0);
    }
    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc);
    wc.lpfnWndProc=WndProc; wc.hInstance=hi;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hbrBackground=CreateSolidBrush(THEMES[g_theme].bg);
    wc.lpszClassName=L"Calc";
    HICON ico=nullptr;
    ExtractIconExW(L"shell32.dll",135,&ico,nullptr,1);
    wc.hIcon=ico?ico:LoadIconW(nullptr,IDI_APPLICATION);
    RegisterClassExW(&wc);

    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    int ww=408, wh=660;
    g_hWnd=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_TOPMOST,L"Calc",L"计算器",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        (sw-ww)/2,(sh-wh)/2,ww,wh,nullptr,nullptr,hi,nullptr);

    ShowWindow(g_hWnd,SW_SHOW); UpdateWindow(g_hWnd); SetForegroundWindow(g_hWnd);
    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    return (int)msg.wParam;
}
