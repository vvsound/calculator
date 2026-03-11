/*
 * 计算器 - 单文件 C++ / Win32 API
 * 编译（MinGW）:
 *   g++ JSQ.cpp -o JSQ.exe -mwindows -lwinmm -municode -std=c++17
 *
 * 特性：
 *   - 零外部依赖，纯 Win32 API
 *   - 数字键发音：1→一  2→二 … 9→九  0→零
 *   - 运算符 / 等号 / 清除各有独立音色
 *   - 五套主题（快捷键 T 切换）
 *   - 支持键盘 & 小键盘输入
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES   // 让 MSVC/MinGW 都能用 M_PI

#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <map>
#include <thread>
#include <cstring>
#include <functional>       // std::function

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#pragma comment(lib,"winmm.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(linker,"/subsystem:windows")

// ════════════════════════════════════════════════════════
//  音效：WAV 文件通过 RC 资源嵌入 .exe，运行时从内存播放
//  资源 ID 约定：
//    100=0.wav 101=1.wav ... 109=9.wav
//    110=add  111=sub  112=mul  113=div  114=eq  115=clr
// ════════════════════════════════════════════════════════

// 从 exe 资源中读取 WAV 数据并播放（异步）
static void PlayWavResource(int resId)
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(resId), L"WAVE");
    if (!hRes) return;
    HGLOBAL hGlob = LoadResource(hInst, hRes);
    if (!hGlob) return;
    void* pData = LockResource(hGlob);
    DWORD size  = SizeofResource(hInst, hRes);
    if (!pData || size == 0) return;
    // SND_MEMORY | SND_ASYNC: 从内存异步播放
    PlaySoundA((LPCSTR)pData, nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

static void PlayDigitChinese(int digit)
{
    // 0→ID 100, 1→101, ..., 9→109
    PlayWavResource(100 + (digit % 10));
}

static void PlayOpSound(wchar_t op)
{
    switch(op){
        case L'+': PlayWavResource(110); break; // add.wav
        case L'-': PlayWavResource(111); break; // sub.wav
        case L'*': PlayWavResource(112); break; // mul.wav
        case L'/': PlayWavResource(113); break; // div.wav
    }
}

static void PlayEqualSound()  { PlayWavResource(114); } // eq.wav
static void PlayDeleteSound() { PlayWavResource(115); } // clr.wav
static void PlayErrorSound()  { PlayWavResource(114); } // 复用 eq.wav

// ════════════════════════════════════════════════════════
//  表达式求值（手写递归下降）
// ════════════════════════════════════════════════════════
struct Parser {
    std::wstring s;
    size_t pos = 0;

    Parser(const std::wstring& expr) : s(expr) {}

    void skip() { while (pos < s.size() && s[pos] == L' ') ++pos; }

    double parse() {
        double v = addSub();
        skip();
        if (pos < s.size()) throw std::runtime_error("syntax");
        return v;
    }

    double addSub() {
        double l = mulDiv();
        for (;;) {
            skip();
            if (pos >= s.size()) break;
            wchar_t op = s[pos];
            if (op != L'+' && op != L'-') break;
            ++pos;
            double r = mulDiv();
            l = (op == L'+') ? l + r : l - r;
        }
        return l;
    }

    double mulDiv() {
        double l = unary();
        for (;;) {
            skip();
            if (pos >= s.size()) break;
            wchar_t op = s[pos];
            if (op != L'*' && op != L'/') break;
            ++pos;
            double r = unary();
            if (op == L'/' && r == 0.0) throw std::runtime_error("div0");
            l = (op == L'*') ? l * r : l / r;
        }
        return l;
    }

    double unary() {
        skip();
        if (pos < s.size() && s[pos] == L'-') { ++pos; return -primary(); }
        if (pos < s.size() && s[pos] == L'+') { ++pos; }
        return primary();
    }

    double primary() {
        skip();
        if (pos < s.size() && s[pos] == L'(') {
            ++pos;
            double v = addSub();
            skip();
            if (pos < s.size() && s[pos] == L')') ++pos;
            return v;
        }
        size_t start = pos;
        while (pos < s.size() && (iswdigit(s[pos]) || s[pos] == L'.')) ++pos;
        if (pos == start) throw std::runtime_error("syntax");
        return std::stod(s.substr(start, pos - start));
    }
};

static std::wstring PrepareExpr(std::wstring expr) {
    // 替换显示符号为 ASCII
    for (auto& c : expr) {
        if (c == L'×') c = L'*';
        if (c == L'÷') c = L'/';
    }
    // 处理百分号：把 \d+%  → (\1/100)
    std::wstring out;
    for (size_t i = 0; i < expr.size(); ++i) {
        if (expr[i] == L'%' && i > 0) {
            // 往前找数字串
            size_t j = i - 1;
            while (j > 0 && (iswdigit(expr[j]) || expr[j] == L'.')) --j;
            if (!iswdigit(expr[j])) ++j;
            std::wstring num = out.substr(j);
            out.erase(j);
            out += L'(' + num + L"/100)";
        } else {
            out += expr[i];
        }
    }
    return out;
}

static std::wstring FormatResult(double v) {
    if (v == (long long)v && std::abs(v) < 1e15) {
        return std::to_wstring((long long)v);
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"%.10g", v);
    return buf;
}

// ════════════════════════════════════════════════════════
//  主题
// ════════════════════════════════════════════════════════
struct Theme {
    const wchar_t* name;
    COLORREF bg, dispBg, procFg, resFg;
    COLORREF num, op, eq, del, pct;
    COLORREF btnFg;
};

static const Theme THEMES[] = {
    {L"🌙深空",   0x2E1E1E,0x3E2A2A,0x888888,0xFFFFFF, 0x442D2D,0xDD78C6,0x79C398,0x756CE0,0xEFAF61,0xFFFFFF},
    {L"☀️米白",   0xE8F0F5,0xDCE8ED,0x889999,0x222222, 0xC4D3D9,0xF65C8B,0x5EC222,0x4444EF,0xF68200,0x222222},
    {L"🌿护眼绿", 0x20241A,0x282D1F,0x809A6A,0xD8F0C8, 0x28331A,0x82AF4C,0x6ABB66,0x7373E5,0xACB64D,0xDDEEDD},
    {L"🎰赛博",   0x1A0D0D,0x28111A,0x884444,0xCCFF00, 0x3A1A1A,0xFF00FF,0x99FF00,0x6633FF,0xFFCC00,0xEEEEFF},
    {L"🍊暖橙",   0x0E1A2B,0x10233A,0x5588CC,0xCCE8FF, 0x10253D,0x42A0FF,0x30C4F4,0x50E0B0,0x5088FF,0xE0F0FF},
};
static int g_themeIdx = 2;

static COLORREF Dim(COLORREF c, double f = 0.70) {
    int r = (int)(GetRValue(c) * f);
    int g = (int)(GetGValue(c) * f);
    int b = (int)(GetBValue(c) * f);
    return RGB(r, g, b);
}

// ════════════════════════════════════════════════════════
//  全局控件 ID
// ════════════════════════════════════════════════════════
#define ID_PROC   100
#define ID_RES    101
#define ID_BTN    200   // 200..219

enum BtnKind { BK_NUM, BK_OP, BK_EQ, BK_DEL, BK_PCT };
struct BtnInfo { const wchar_t* label; int row, col; BtnKind kind; };
static const BtnInfo BUTTONS[] = {
    {L"C",  0,0,BK_DEL},{L"←",0,1,BK_DEL},{L"%",0,2,BK_PCT},{L"÷",0,3,BK_OP},
    {L"7",  1,0,BK_NUM},{L"8",1,1,BK_NUM},{L"9",1,2,BK_NUM},{L"×",1,3,BK_OP},
    {L"4",  2,0,BK_NUM},{L"5",2,1,BK_NUM},{L"6",2,2,BK_NUM},{L"-",2,3,BK_OP},
    {L"1",  3,0,BK_NUM},{L"2",3,1,BK_NUM},{L"3",3,2,BK_NUM},{L"+",3,3,BK_OP},
    {L"±",  4,0,BK_NUM},{L"0",4,1,BK_NUM},{L".",4,2,BK_NUM},{L"=",4,3,BK_EQ},
};
static const int BTN_COUNT = 20;

// ════════════════════════════════════════════════════════
//  计算器状态
// ════════════════════════════════════════════════════════
static std::wstring g_expr;
static HWND g_hWnd   = nullptr;
static HWND g_hProc  = nullptr;
static HWND g_hRes   = nullptr;
static HWND g_hBtns[BTN_COUNT] = {};
static HFONT g_fontSmall = nullptr, g_fontLarge = nullptr, g_fontBtn = nullptr;
static HBRUSH g_brBtns[BTN_COUNT] = {};
static int    g_hotBtn = -1;  // 鼠标悬停的按钮索引

static void SetProc(const std::wstring& s) { SetWindowTextW(g_hProc, s.c_str()); }
static void SetRes (const std::wstring& s) { SetWindowTextW(g_hRes,  s.c_str()); }

// ════════════════════════════════════════════════════════
//  主题应用
// ════════════════════════════════════════════════════════
static COLORREF BtnColor(int idx) {
    const Theme& t = THEMES[g_themeIdx];
    switch (BUTTONS[idx].kind) {
        case BK_NUM: return t.num;
        case BK_OP:  return t.op;
        case BK_EQ:  return t.eq;
        case BK_DEL: return t.del;
        case BK_PCT: return t.pct;
    }
    return t.num;
}

static void ApplyTheme() {
    const Theme& t = THEMES[g_themeIdx];
    // 窗口背景
    SetClassLongPtrW(g_hWnd, GCLP_HBRBACKGROUND,
        (LONG_PTR)CreateSolidBrush(t.bg));
    InvalidateRect(g_hWnd, nullptr, TRUE);

    for (int i = 0; i < BTN_COUNT; i++) {
        if (g_brBtns[i]) DeleteObject(g_brBtns[i]);
        g_brBtns[i] = CreateSolidBrush(BtnColor(i));
        InvalidateRect(g_hBtns[i], nullptr, TRUE);
    }
    // 刷新显示区
    InvalidateRect(g_hProc, nullptr, TRUE);
    InvalidateRect(g_hRes,  nullptr, TRUE);
}

// ════════════════════════════════════════════════════════
//  按钮点击处理
// ════════════════════════════════════════════════════════
static void HandleClick(const wchar_t* label)
{
    // --- 音效（异步，拷贝 label 字符串避免指针悬空）---
    std::wstring lbl(label);
    std::thread([lbl]() {
        wchar_t c0 = lbl.empty() ? 0 : lbl[0];
        if (c0 >= L'0' && c0 <= L'9' && lbl.size() == 1) {
            PlayDigitChinese(c0 - L'0');
        } else if (lbl == L".")  { PlayTone(370, 65, 0.40f);
        } else if (lbl == L"+")  { PlayTone(520, 90);
        } else if (lbl == L"-")  { PlayTone(440, 90);
        } else if (lbl == L"×")  { PlayTone(600, 90);
        } else if (lbl == L"÷")  { PlayTone(380, 90);
        } else if (lbl == L"%")  { PlayTone(350, 80);
        } else if (lbl == L"=")  { PlayEqualSound();
        } else if (lbl == L"←" || lbl == L"C") { PlayDeleteSound();
        } else if (lbl == L"±")  { PlayTone(480, 80);
        }
    }).detach();

    // --- 逻辑 ---
    std::wstring& expr = g_expr;

    if (wcscmp(label, L"C") == 0) {
        expr.clear();
        SetProc(L"");  SetRes(L"0");
    }
    else if (wcscmp(label, L"←") == 0) {
        if (!expr.empty()) expr.pop_back();
        SetProc(expr);
        SetRes(expr.empty() ? L"0" : expr);
    }
    else if (wcscmp(label, L"=") == 0) {
        if (expr.empty()) return;
        try {
            std::wstring prepared = PrepareExpr(expr);
            Parser p(prepared);
            double r = p.parse();
            std::wstring out = FormatResult(r);
            SetProc(expr + L" =");
            expr = out;
            SetRes(out);
        } catch (...) {
            AsyncPlay(PlayErrorSound);
            SetProc(expr + L" = ?");
            SetRes(L"错误");
            expr.clear();
        }
    }
    else if (wcscmp(label, L"+") == 0 || wcscmp(label, L"-") == 0 ||
             wcscmp(label, L"×") == 0 || wcscmp(label, L"÷") == 0) {
        if (!expr.empty()) {
            wchar_t last = expr.back();
            if (last == L'+' || last == L'-' || last == L'×' || last == L'÷')
                expr.pop_back();
        }
        expr += label[0];
        SetProc(expr); SetRes(expr);
    }
    else if (wcscmp(label, L"±") == 0) {
        // 翻转最后一个数字的符号
        size_t opPos = expr.find_last_of(L"+-×÷");
        size_t numStart = (opPos == std::wstring::npos) ? 0 : opPos + 1;
        std::wstring num = expr.substr(numStart);
        if (!num.empty()) {
            std::wstring prefix = expr.substr(0, numStart);
            if (!num.empty() && num[0] == L'-')
                expr = prefix + num.substr(1);
            else
                expr = prefix + L'-' + num;
            SetProc(expr); SetRes(expr);
        }
    }
    else if (wcscmp(label, L"%") == 0) {
        expr += L'%';
        SetProc(expr); SetRes(expr);
    }
    else if (wcscmp(label, L".") == 0) {
        // 找最后一段数字，没有小数点才加
        size_t opPos = expr.find_last_of(L"+-×÷");
        std::wstring seg = (opPos == std::wstring::npos) ? expr : expr.substr(opPos + 1);
        if (seg.find(L'.') == std::wstring::npos) {
            if (seg.empty()) expr += L'0';
            expr += L'.';
            SetProc(expr); SetRes(expr);
        }
    }
    else {
        expr += label[0];
        SetProc(expr); SetRes(expr);
    }
}

// ════════════════════════════════════════════════════════
//  自绘按钮子类化
// ════════════════════════════════════════════════════════
static WNDPROC g_origBtnProc = nullptr;

static LRESULT CALLBACK BtnSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int idx = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        bool hot = (g_hotBtn == idx);
        COLORREF base = BtnColor(idx);
        COLORREF fill = hot ? Dim(base) : base;

        HBRUSH br = CreateSolidBrush(fill);
        // 圆角
        HRGN rgn = CreateRoundRectRgn(0,0,rc.right+1,rc.bottom+1,10,10);
        FillRgn(hdc, rgn, br);
        DeleteObject(rgn);
        DeleteObject(br);

        // 文字
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, THEMES[g_themeIdx].btnFg);
        SelectObject(hdc, g_fontBtn);
        wchar_t txt[8]; GetWindowTextW(hwnd, txt, 8);
        DrawTextW(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_hotBtn != idx) {
            g_hotBtn = idx;
            // 追踪鼠标离开
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        g_hotBtn = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }
    return CallWindowProcW(g_origBtnProc, hwnd, msg, wp, lp);
}

// ════════════════════════════════════════════════════════
//  显示区自绘（WM_CTLCOLORSTATIC / WM_CTLCOLOREDIT）
// ════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════
//  主窗口消息处理
// ════════════════════════════════════════════════════════
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE: {
        // 字体
        g_fontSmall = CreateFontW(-24,0,0,0,FW_BOLD,0,0,0,
            DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
        g_fontLarge = CreateFontW(-64,0,0,0,FW_BOLD,0,0,0,
            DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
        g_fontBtn   = CreateFontW(-30,0,0,0,FW_BOLD,0,0,0,
            DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");

        // 过程行
        g_hProc = CreateWindowExW(0,L"STATIC",L"",
            WS_CHILD|WS_VISIBLE|SS_RIGHT,
            10,10,380,36,hwnd,(HMENU)ID_PROC,nullptr,nullptr);
        SendMessageW(g_hProc, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

        // 结果行
        g_hRes = CreateWindowExW(0,L"STATIC",L"0",
            WS_CHILD|WS_VISIBLE|SS_RIGHT,
            10,50,380,90,hwnd,(HMENU)ID_RES,nullptr,nullptr);
        SendMessageW(g_hRes, WM_SETFONT, (WPARAM)g_fontLarge, TRUE);

        // 按钮  (每格 95×88，间距 5)
        const int BW=93, BH=86, GAP=5, OX=10, OY=158;
        for (int i = 0; i < BTN_COUNT; i++) {
            auto& b = BUTTONS[i];
            int x = OX + b.col * (BW + GAP);
            int y = OY + b.row * (BH + GAP);
            g_hBtns[i] = CreateWindowExW(0,L"BUTTON",b.label,
                WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                x,y,BW,BH,hwnd,(HMENU)(UINT_PTR)(ID_BTN+i),nullptr,nullptr);
            // 子类化以便自绘圆角
            SetWindowLongPtrW(g_hBtns[i], GWLP_USERDATA, i);
            if (!g_origBtnProc)
                g_origBtnProc = (WNDPROC)GetWindowLongPtrW(g_hBtns[i], GWLP_WNDPROC);
            SetWindowLongPtrW(g_hBtns[i], GWLP_WNDPROC, (LONG_PTR)BtnSubclassProc);
        }
        ApplyTheme();
        return 0;
    }

    // 自绘按钮（BS_OWNERDRAW 回调，但我们已在子类化里处理，这里只需返回）
    case WM_DRAWITEM:
        return TRUE;

    // 静态控件颜色
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        HWND hw  = (HWND)lp;
        const Theme& t = THEMES[g_themeIdx];
        SetBkColor(hdc, t.dispBg);
        if (hw == g_hProc) SetTextColor(hdc, t.procFg);
        else                SetTextColor(hdc, t.resFg);
        static HBRUSH br = nullptr;
        if (br) DeleteObject(br);
        br = CreateSolidBrush(t.dispBg);
        return (LRESULT)br;
    }

    // 背景
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(THEMES[g_themeIdx].bg);
        FillRect((HDC)wp, &rc, br);
        DeleteObject(br);
        return 1;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_BTN && id < ID_BTN + BTN_COUNT) {
            HandleClick(BUTTONS[id - ID_BTN].label);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        // T = 切换主题
        if (wp == 'T' || wp == 't') {
            g_themeIdx = (g_themeIdx + 1) % 5;
            ApplyTheme();
            return 0;
        }
        // Escape = 退出
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        // 退格
        if (wp == VK_BACK)   { HandleClick(L"←"); return 0; }
        // Enter / 小键盘Enter
        if (wp == VK_RETURN) { HandleClick(L"="); return 0; }
        // Delete = Clear
        if (wp == VK_DELETE) { HandleClick(L"C"); return 0; }
        return 0;
    }

    case WM_CHAR: {
        wchar_t c = (wchar_t)wp;
        if (c >= L'0' && c <= L'9') { wchar_t s[2]={c,0}; HandleClick(s); }
        else if (c == L'.') HandleClick(L".");
        else if (c == L'+') HandleClick(L"+");
        else if (c == L'-') HandleClick(L"-");
        else if (c == L'*') HandleClick(L"×");
        else if (c == L'/') HandleClick(L"÷");
        else if (c == L'%') HandleClick(L"%");
        else if (c == L'=') HandleClick(L"=");
        else if (c == L'c' || c == L'C') HandleClick(L"C");
        return 0;
    }

    case WM_DESTROY:
        // 退出时关闭 NumLock
        if (GetKeyState(VK_NUMLOCK) & 1) {
            keybd_event(VK_NUMLOCK, 0x45, 0, 0);
            keybd_event(VK_NUMLOCK, 0x45, KEYEVENTF_KEYUP, 0);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ════════════════════════════════════════════════════════
//  入口
// ════════════════════════════════════════════════════════
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // 开启 NumLock
    if (!(GetKeyState(VK_NUMLOCK) & 1)) {
        keybd_event(VK_NUMLOCK, 0x45, 0, 0);
        keybd_event(VK_NUMLOCK, 0x45, KEYEVENTF_KEYUP, 0);
    }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(THEMES[g_themeIdx].bg);
    wc.lpszClassName = L"CalcWnd";
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"CalcWnd", L"计算器",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        1000, 200, 460, 700,
        nullptr, nullptr, hInst, nullptr
    );

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);
    SetForegroundWindow(g_hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
