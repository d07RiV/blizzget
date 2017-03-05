#include "window.h"

#include "frameui/fontsys.h"

#include <stdio.h>
#include <windowsx.h>

struct Window::TTData {
  HWND hTip;
  RECT tipRect;
  int hitCode;
  HFONT hFont;
  std::string fixedTip;
};
static LRESULT CALLBACK TooltipWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  if (uMsg == WM_PAINT) {
    std::string text = Window::getWindowText(hWnd);

    RECT rc;
    GetClientRect(hWnd, &rc);
    PAINTSTRUCT ps;
    HDC hDC = BeginPaint(hWnd, &ps);

    HFONT hFont = (HFONT)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    SelectObject(hDC, hFont);
    SetBkColor(hDC, 0xE1FFFF);
    SetTextColor(hDC, 0x000000);

    HPEN hPen = CreatePen(PS_SOLID, 0, 0x000000);
    HPEN hPrev = (HPEN)SelectObject(hDC, hPen);
    Rectangle(hDC, rc.left, rc.top, rc.right, rc.bottom);
    rc.left += 5;
    rc.top += 2;
    rc.bottom -= 2;
    rc.right -= 5;
    DrawText(hDC, text.c_str(), text.length(), &rc, DT_LEFT | DT_TOP);
    SelectObject(hDC, hPrev);
    DeleteObject(hPen);

    EndPaint(hWnd, &ps);
    return 0;
  }
  return DefWindowProc(hWnd, uMsg, wParam, lParam);;
}

ATOM Window::windowClass = NULL;
std::map<HWND, Window*> Window::handleMap;

Window* Window::fromHandle(HWND hWnd) {
  auto it = handleMap.find(hWnd);
  return (it == handleMap.end() ? nullptr : it->second);
}

WNDCLASSEX* Window::createclass(std::string const& wndClass) {
  regClass = wndClass;

  WNDCLASSEX* wcx = new WNDCLASSEX;
  HINSTANCE hInstance = GetModuleHandle(NULL);
  if (!GetClassInfoEx(hInstance, regClass.c_str(), wcx)) {
    memset(wcx, 0, sizeof(WNDCLASSEX));
    wcx->cbSize = sizeof(WNDCLASSEX);
    wcx->lpfnWndProc = WindowProc;
    wcx->hInstance = hInstance;
    wcx->lpszClassName = regClass.c_str();
    wcx->hCursor = LoadCursor(NULL, IDC_ARROW);
    return wcx;
  }
  delete wcx;
  return NULL;
}
void Window::create(int x, int y, int width, int height, std::string const& text, uint32 style, uint32 exStyle,
  HWND parent)
{
  if (!windowClass && regClass.empty()) {
    WNDCLASSEX wcex;
    memset(&wcex, 0, sizeof wcex);
    wcex.cbSize = sizeof wcex;
    wcex.lpfnWndProc = WindowProc;
    wcex.hInstance = GetModuleHandle(NULL);
    wcex.lpszClassName = "WUTILSWINDOW";
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass = RegisterClassEx(&wcex);
  }
  hWnd = CreateWindowEx(exStyle, regClass.empty() ? "WUTILSWINDOW" : regClass.c_str(), text.c_str(), style,
    x, y, width, height, parent, NULL, GetModuleHandle(NULL), this);
  handleMap[hWnd] = this;
}
void Window::create(std::string const& wndClass, int x, int y, int width, int height, std::string const& text, uint32 style,
    uint32 exStyle, HWND parent) {
  hWnd = CreateWindowEx(exStyle, wndClass.c_str(), text.c_str(), style, x, y, width, height,
    parent, NULL, GetModuleHandle(NULL), NULL);
  handleMap[hWnd] = this;
}
void Window::subclass(std::string const& wndClass, int x, int y, int width, int height, std::string const& text, uint32 style,
    uint32 exStyle, HWND parent) {
  hWnd = CreateWindowEx(exStyle, wndClass.c_str(), text.c_str(), style, x, y, width, height,
    parent, NULL, GetModuleHandle(NULL), NULL);
  handleMap[hWnd] = this;
  origProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR) WindowProc);
}

LRESULT CALLBACK Window::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  Window* wnd = fromHandle(hWnd);
  if (wnd == NULL && uMsg == WM_CREATE) {
    CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
    wnd = (Window*)cs->lpCreateParams;
    if (wnd) wnd->hWnd = hWnd;
  }
  if (wnd) {
    bool send = true;
    if (wnd->ttData) {
      TRACKMOUSEEVENT tme;
      memset(&tme, 0, sizeof tme);
      tme.cbSize = sizeof tme;
      tme.hwndTrack = wnd->hWnd;
      if (uMsg == WM_MOUSEMOVE) {
        POINT pt;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        if (wnd->ttData->hitCode >= 0) {
          if (pt.x < wnd->ttData->tipRect.left || pt.x >= wnd->ttData->tipRect.right ||
              pt.y < wnd->ttData->tipRect.top || pt.y >= wnd->ttData->tipRect.bottom) {
            wnd->ttData->hitCode = -1;
          }
        }
        if (wnd->ttData->hitCode < 0) {
          ToolInfo ti;
          wnd->ttData->hitCode = wnd->toolHitTest(pt, &ti);
          if (wnd->ttData->hitCode >= 0) {
            tme.dwFlags = TME_HOVER | TME_LEAVE;
            tme.dwHoverTime = HOVER_DEFAULT;
            TrackMouseEvent(&tme);

            wnd->ttData->tipRect = ti.rc;
            SetWindowText(wnd->ttData->hTip, ti.text.c_str());

            HDC hDC = GetDC(wnd->ttData->hTip);
            SelectObject(hDC, wnd->ttData->hFont);
            RECT rc = { 0, 0, 1024, 1024 };
            int ht = DrawText(hDC, ti.text.c_str(), ti.text.length(), &rc, DT_LEFT | DT_TOP | DT_CALCRECT);
            ReleaseDC(wnd->ttData->hTip, hDC);

            POINT ptTL;
            ptTL.x = wnd->ttData->tipRect.left;
            ptTL.y = wnd->ttData->tipRect.bottom + 5;
            ClientToScreen(wnd->hWnd, &ptTL);

            SetWindowLongPtr(wnd->ttData->hTip, GWLP_USERDATA, (LONG_PTR)wnd->ttData->hFont);

            InvalidateRect(wnd->ttData->hTip, NULL, TRUE);
            SetWindowPos(wnd->ttData->hTip, HWND_TOPMOST,
              ptTL.x, ptTL.y, rc.right + 10, ht + 5,
              SWP_NOACTIVATE);
          } else {
            tme.dwFlags = TME_CANCEL;
            TrackMouseEvent(&tme);
            ShowWindow(wnd->ttData->hTip, SW_HIDE);
          }
        }
      } else if (uMsg == WM_MOUSELEAVE) {
        ShowWindow(wnd->ttData->hTip, SW_HIDE);
        wnd->ttData->hitCode = -1;
        send = false;
      } else if (uMsg == WM_MOUSEHOVER) {
        tme.dwFlags = TME_CANCEL;
        TrackMouseEvent(&tme);

        if (wnd->ttData->hitCode >= 0) ShowWindow(wnd->ttData->hTip, SW_SHOWNA);
        send = false;
      } else if (uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN ||
          uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL || uMsg == WM_KEYDOWN) {
        ShowWindow(wnd->ttData->hTip, SW_HIDE);
        wnd->ttData->hitCode = -1;
      }
    }
    if (send) {
      if (uMsg == WM_CLOSE || uMsg == WM_DESTROY) wnd->endModal();
      uint32 result = wnd->onWndMessage(uMsg, wParam, lParam);
      if (uMsg == WM_DESTROY) wnd->hWnd = NULL;
      return result;
    } else {
      return 0;
    }
  } else {
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
  }
}
LRESULT Window::onWndMessage(uint32 message, WPARAM wParam, LPARAM lParam) {
  if (origProc) return CallWindowProc(origProc, hWnd, message, wParam, lParam);
  else return DefWindowProc(hWnd, message, wParam, lParam);
}

Window::Window() {
  hWnd = NULL;
  origProc = NULL;
  ttData = NULL;
}
Window::~Window() {
  if (hWnd) {
    if (origProc) SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)origProc);
    handleMap.erase(hWnd);
    DestroyWindow(hWnd);
  }
  if (ttData) delete ttData;
}

void Window::setText(std::string const& text)
{
  SetWindowText(hWnd, text.c_str());
}
std::string Window::getText() const {
  return getWindowText(hWnd);
}
std::string Window::getWindowText(HWND hWnd) {
  size_t length = GetWindowTextLength(hWnd);
  std::string str;
  str.resize(length + 1);
  GetWindowText(hWnd, &str[0], str.length());
  str.resize(length);
  return str;
}

void Window::setFont(HFONT hFont) {
  SendMessage(hWnd, WM_SETFONT, (WPARAM)hFont, TRUE);
}
HFONT Window::getFont() const {
  return (HFONT)SendMessage(hWnd, WM_GETFONT, NULL, NULL);
}

void Window::enable(bool e) {
  EnableWindow(hWnd, e);
}
void Window::showWindow(bool s) {
  ShowWindow(hWnd, s ? SW_SHOW : SW_HIDE);
}

int Window::id() const {
  return GetWindowLong(hWnd, GWL_ID);
}
void Window::setId(int id) {
  SetWindowLong(hWnd, GWL_ID, id);
}

void Window::invalidate(bool erase) {
  InvalidateRect(hWnd, NULL, erase ? TRUE : FALSE);
}

void Window::setRedraw(bool r) {
  SendMessage(hWnd, WM_SETREDRAW, r ? TRUE : FALSE, 0);
  if (r) RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE);
}

int Window::toolHitTest(POINT pt, ToolInfo* ti) {
  if (ttData && ttData->fixedTip.length()) {
    GetClientRect(hWnd, &ti->rc);
    ti->text = ttData->fixedTip;
    return 0;
  }
  return -1;
}
void Window::enableTooltips(bool enable) {
  if (hWnd == NULL) return;
  if (enable && ttData == NULL) {
    ttData = new TTData;
    ttData->hFont = FontSys::getSysFont();
    WNDCLASSEX wcx;
    HINSTANCE hInstance = GetModuleHandle(NULL);
    if (!GetClassInfoEx(hInstance, "DRTooltip", &wcx)) {
      memset(&wcx, 0, sizeof wcx);
      wcx.cbSize = sizeof wcx;
      wcx.lpfnWndProc = TooltipWindowProc;
      wcx.hInstance = hInstance;
      wcx.lpszClassName = "DRTooltip";
      wcx.hbrBackground = CreateSolidBrush(0xE1FFFF);
      RegisterClassEx(&wcx);
    }
    ttData->hTip = CreateWindowEx(WS_EX_TOPMOST, "DRTooltip",
      NULL, WS_POPUP, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, hWnd, NULL,
      hInstance, NULL);

    ttData->hitCode = -1;
  } else if (!enable && ttData != NULL) {
    DestroyWindow(ttData->hTip);
    delete ttData;
    ttData = NULL;
  }
}
void Window::setTooltipFont(HFONT hFont) {
  if (ttData) ttData->hFont = hFont;
}
HFONT Window::getTooltipFont() {
  return (ttData ? ttData->hFont : NULL);
}

void Window::endModal() {
  if (continueModal) {
    continueModal = false;
    SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOACTIVATE |
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    HWND hParent = GetWindow(hWnd, GW_OWNER);
    if (hParent) {
      EnableWindow(hParent, TRUE);
      if (GetActiveWindow() == hWnd) SetActiveWindow(hParent);
    }
    PostMessage(hWnd, WM_CLOSE, 0, 0);
  }
}
int Window::doModal() {
  if (hWnd == NULL) return 0;
  ShowWindow(hWnd, SW_SHOW);
  HWND hParent = GetWindow(hWnd, GW_OWNER);
  if (hParent) EnableWindow(hParent, FALSE);
  MSG msg;
  continueModal = true;
  while (continueModal) {
    if (!GetMessage(&msg, NULL, 0, 0)) {
      PostQuitMessage(msg.wParam);
      return 0;
    }
    if (!IsDialogMessage(hWnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }
  return 0;
}
void Window::setTooltip(char const* tip) {
  if (tip && *tip) {
    enableTooltips(true);
    ttData->fixedTip = tip;
  } else {
    enableTooltips(false);
  }
}
