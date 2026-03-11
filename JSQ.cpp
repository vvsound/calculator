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

#include <windows.h>
#include <mmsystem.h>       // PlaySound / waveOut
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

#pragma comment(lib,"winmm.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(linker,"/subsystem:windows")

// ════════════════════════════════════════════════════════
//  PCM 正弦波合成 & 播放（不需要任何音频文件）
// ════════════════════════════════════════════════════════
static void PlayTone(int freqHz, int durationMs, float volume = 0.45f)
{
    const int SAMPLE_RATE = 44100;
    int numSamples = SAMPLE_RATE * durationMs / 1000;
    std::vector<short> buf(numSamples);

    // 正弦波 + 淡出包络，使声音圆润
    for (int i = 0; i < numSamples; i++) {
        double t   = (double)i / SAMPLE_RATE;
        double env = 1.0;
        // 后 20% 线性淡出
        if (i > numSamples * 0.80)
            env = (double)(numSamples - i) / (numSamples * 0.20);
        buf[i] = (short)(32767 * volume * env * sin(2.0 * M_PI * freqHz * t));
    }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 2;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * 2;

    HWAVEOUT hwo = nullptr;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return;

    WAVEHDR hdr{};
    hdr.lpData         = reinterpret_cast<LPSTR>(buf.data());
    hdr.dwBufferLength = (DWORD)(numSamples * sizeof(short));
    waveOutPrepareHeader(hwo, &hdr, sizeof(hdr));
    waveOutWrite(hwo, &hdr, sizeof(hdr));

    // 等待播放完毕
    while (!(hdr.dwFlags & WHDR_DONE))
        Sleep(5);

    waveOutUnprepareHeader(hwo, &hdr, sizeof(hdr));
    waveOutClose(hwo);
}

// 播放和弦（多音依次快速叠加，模拟和声感）
static void PlayChord(const std::vector<std::pair<int,int>>& notes)
{
    for (auto& [f, d] : notes)
        PlayTone(f, d, 0.38f);
}

// 异步播放（不阻塞 UI 线程）
static void AsyncPlay(std::function<void()> fn)
{
    std::thread([fn]{ fn(); }).detach();
}

// ════════════════════════════════════════════════════════
//  中文数字朗读（用音调序列模拟）
//  每个汉字用一段特征音调+短停顿表现节奏感
// ════════════════════════════════════════════════════════
struct SyllableTone { int freq; int ms; };

// 仿普通话四声的特征频率序列
// 一(yī) 二(èr) 三(sān) 四(sì) 五(wǔ) 六(liù) 七(qī) 八(bā) 九(jiǔ) 零(líng)
static const std::vector<SyllableTone> CN_TONES[] = {
    /*零0*/ {{330,55},{340,55},{330,40}},   // 二声: 中升
    /*一1*/ {{440,130}},                     // 一声: 高平
    /*二2*/ {{300,45},{380,100}},            // 二声: 低升
    /*三3*/ {{400,120}},                     // 一声: 高平
    /*四4*/ {{420,40},{280,90}},             // 四声: 高降
    /*五5*/ {{280,50},{300,60},{340,50}},    // 三声: 低降升
    /*六6*/ {{380,40},{260,90}},             // 四声: 高降
    /*七7*/ {{440,130}},                     // 一声: 高平
    /*八8*/ {{420,130}},                     // 一声: 高平
    /*九9*/ {{300,50},{380,90}},             // 二声: 低升
};

static void PlayDigitChinese(int digit)
{
    // digit 0-9
    const auto& seq = CN_TONES[digit % 10];
    for (auto& s : seq)
        PlayTone(s.freq, s.ms, 0.50f);
}

// 运算符音效
static void PlayOpSound(wchar_t op)
{
    switch(op) {
        case L'+': PlayTone(520, 80); break;
        case L'-': PlayTone(440, 80); break;
        case L'*': PlayTone(600, 80); break;
        case L'/': PlayTone(380, 80); break;
        case L'%': PlayTone(350, 70); break;
    }
}

static void PlayEqualSound()
{
    PlayChord({{523,70},{659,70},{784,140}});
}

static void PlayDeleteSound()
{
    PlayTone(260, 90);
}

static void PlayErrorSound()
{
    PlayTone(200,120); PlayTone(180,200);
}

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
    // --- 音效（异步）---
    AsyncPlay([label]() {
        if (label[0] >= L'0' && label[0] <= L'9' && label[1] == 0) {
            PlayDigitChinese(label[0] - L'0');
        } else if (label[0] == L'.' && label[1] == 0) {
            PlayTone(370, 65, 0.40f);
        } else if (label[0] == L'+' || label[0] == L'-' ||
                   label[0] == L'×' || label[0] == L'÷' ||
                   label[0] == L'%') {
            wchar_t op = label[0];
            if (op == L'×') op = L'*';
            if (op == L'÷') op = L'/';
            PlayOpSound(op);
        } else if (label[0] == L'=') {
            PlayEqualSound();
        } else if (label[0] == L'←' || label[0] == L'C') {
            PlayDeleteSound();
        } else if (label[0] == L'±') {
            PlayTone(480, 75);
        }
    });

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
        g_fontSmall = CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,
            DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
        g_fontLarge = CreateFontW(-48,0,0,0,FW_BOLD,0,0,0,
            DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
        g_fontBtn   = CreateFontW(-22,0,0,0,FW_BOLD,0,0,0,
            DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");

        // 过程行
        g_hProc = CreateWindowExW(0,L"STATIC",L"",
            WS_CHILD|WS_VISIBLE|SS_RIGHT,
            10,10,380,28,hwnd,(HMENU)ID_PROC,nullptr,nullptr);
        SendMessageW(g_hProc, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

        // 结果行
        g_hRes = CreateWindowExW(0,L"STATIC",L"0",
            WS_CHILD|WS_VISIBLE|SS_RIGHT,
            10,42,380,70,hwnd,(HMENU)ID_RES,nullptr,nullptr);
        SendMessageW(g_hRes, WM_SETFONT, (WPARAM)g_fontLarge, TRUE);

        // 按钮  (每格 95×88，间距 5)
        const int BW=93, BH=86, GAP=5, OX=10, OY=125;
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
        1000, 500, 410, 580,
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
