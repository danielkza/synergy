/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2002 Chris Schoeneman
 * 
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file COPYING that should have accompanied this file.
 * 
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "CSynergyHook.h"
#include "ProtocolTypes.h"
#include <zmouse.h>

//
// debugging compile flag.  when not zero the server doesn't grab
// the keyboard when the mouse leaves the server screen.  this
// makes it possible to use the debugger (via the keyboard) when
// all user input would normally be caught by the hook procedures.
//
#define NO_GRAB_KEYBOARD 0

//
// debugging compile flag.  when not zero the server will not
// install low level hooks.
//
#define NO_LOWLEVEL_HOOKS 0

//
// extra mouse wheel stuff
//

enum EWheelSupport {
	kWheelNone,
	kWheelOld,
	kWheelWin2000,
	kWheelModern
};

// declare extended mouse hook struct.  useable on win2k
typedef struct tagMOUSEHOOKSTRUCTWin2000 {
	MOUSEHOOKSTRUCT mhs;
	DWORD mouseData;
} MOUSEHOOKSTRUCTWin2000;

#if !defined(SM_MOUSEWHEELPRESENT)
#define SM_MOUSEWHEELPRESENT 75
#endif

// X button stuff
#if !defined(WM_XBUTTONDOWN)
#define WM_XBUTTONDOWN		0x020B
#define WM_XBUTTONUP		0x020C
#define WM_XBUTTONDBLCLK	0x020D
#define WM_NCXBUTTONDOWN	0x00AB
#define WM_NCXBUTTONUP		0x00AC
#define WM_NCXBUTTONDBLCLK	0x00AD
#define MOUSEEVENTF_XDOWN	0x0100
#define MOUSEEVENTF_XUP		0x0200
#define XBUTTON1			0x0001
#define XBUTTON2			0x0002
#endif


//
// globals
//

#pragma comment(linker, "-section:shared,rws")
#pragma data_seg("shared")
// all data in this shared section *must* be initialized

static HINSTANCE		g_hinstance       = NULL;
static DWORD			g_processID       = 0;
static EWheelSupport	g_wheelSupport    = kWheelNone;
static UINT				g_wmMouseWheel    = 0;
static DWORD			g_threadID        = 0;
static HHOOK			g_keyboard        = NULL;
static HHOOK			g_mouse           = NULL;
static HHOOK			g_getMessage      = NULL;
static HHOOK			g_keyboardLL      = NULL;
static HHOOK			g_mouseLL         = NULL;
static bool				g_screenSaver     = false;
static EHookMode		g_mode            = kHOOK_DISABLE;
static UInt32			g_zoneSides       = 0;
static SInt32			g_zoneSize        = 0;
static SInt32			g_xScreen         = 0;
static SInt32			g_yScreen         = 0;
static SInt32			g_wScreen         = 0;
static SInt32			g_hScreen         = 0;
static WPARAM			g_deadVirtKey     = 0;
static LPARAM			g_deadLParam      = 0;
static BYTE				g_deadKeyState[256] = { 0 };
static DWORD			g_hookThread      = 0;
static DWORD			g_attachedThread  = 0;

#pragma data_seg()

// keep linker quiet about floating point stuff.  we don't use any
// floating point operations but our includes may define some
// (unused) floating point values.
#ifndef _DEBUG
extern "C" int _fltused=0;
#endif


//
// internal functions
//

static
void
attachThreadToForeground()
{
	// only attach threads if using low level hooks.  a low level hook
	// runs in the thread that installed the hook but we have to make
	// changes that require being attached to the target thread (which
	// should be the foreground window).  a regular hook runs in the
	// thread that just removed the event from its queue so we're
	// already in the right thread.
	if (g_hookThread != 0) {
		HWND window    = GetForegroundWindow();
		DWORD threadID = GetWindowThreadProcessId(window, NULL);
		// skip if no change
		if (g_attachedThread != threadID) {
			// detach from previous thread
			if (g_attachedThread != 0 && g_attachedThread != g_hookThread) {
				AttachThreadInput(g_hookThread, g_attachedThread, FALSE);
			}
			// attach to new thread
			g_attachedThread = threadID;
			if (g_attachedThread != 0 && g_attachedThread != g_hookThread) {
				AttachThreadInput(g_hookThread, g_attachedThread, TRUE);
			}
		}
	}
}

static
void
detachThread()
{
	if (g_attachedThread != 0) {
		AttachThreadInput(g_hookThread, g_attachedThread, FALSE);
		g_attachedThread = 0;
	}
}

#if !NO_GRAB_KEYBOARD
static
WPARAM
makeKeyMsg(UINT virtKey, char c)
{
	return MAKEWPARAM(MAKEWORD(virtKey & 0xff, (BYTE)c), 0);
}

static
void
keyboardGetState(BYTE keys[256])
{
	if (g_hookThread != 0) {
		GetKeyboardState(keys);
	}
	else {
		SHORT key;
		for (int i = 0; i < 256; ++i) {
			key     = GetAsyncKeyState(i);
			keys[i] = (BYTE)((key < 0) ? 0x80u : 0);
		}
		key = GetKeyState(VK_CAPITAL);
		keys[VK_CAPITAL] = (BYTE)(((key < 0) ? 0x80 : 0) | (key & 1));
	}
}

static
bool
keyboardHookHandler(WPARAM wParam, LPARAM lParam)
{
	attachThreadToForeground();

	// check for dead keys.  we don't forward those to our window.
	// instead we'll leave the key in the keyboard layout (a buffer
	// internal to the system) for translation when the next key is
	// pressed.
	UINT c = MapVirtualKey(wParam, 2);
	if ((c & 0x80000000u) != 0) {
		if ((lParam & 0x80000000u) == 0) {
			if (g_deadVirtKey == 0) {
				// dead key press, no dead key in the buffer
				g_deadVirtKey = wParam;
				g_deadLParam  = lParam;
				keyboardGetState(g_deadKeyState);
				return false;
			}
			// second dead key press in a row so let it pass
		}
		else {
			// dead key release
			return false;
		}
	}

	// convert key to a character.  this combines a saved dead key,
	// if any, with this key.  however, the dead key must remain in
	// the keyboard layout for the application receiving this event
	// so it can also convert the key to a character.  we only do
	// this on a key press.
	WPARAM charAndVirtKey = (wParam & 0xffu);
	if (c != 0) {
		// we need the keyboard state for ToAscii()
		BYTE keys[256];
		keyboardGetState(keys);

		// ToAscii() maps ctrl+letter to the corresponding control code
		// and ctrl+backspace to delete.  we don't want those translations
		// so clear the control modifier state.  however, if we want to
		// simulate AltGr (which is ctrl+alt) then we must not clear it.
		BYTE control = keys[VK_CONTROL];
		BYTE menu    = keys[VK_MENU];
		if ((control & 0x80) == 0 || (menu & 0x80) == 0) {
			keys[VK_LCONTROL] = 0;
			keys[VK_RCONTROL] = 0;
			keys[VK_CONTROL]  = 0;
		}
		else {
			keys[VK_LCONTROL] = 0x80;
			keys[VK_CONTROL]  = 0x80;
			keys[VK_LMENU]    = 0x80;
			keys[VK_MENU]     = 0x80;
		}

		// ToAscii() needs to know if a menu is active for some reason.
		// we don't know and there doesn't appear to be any way to find
		// out.  so we'll just assume a menu is active if the menu key
		// is down.
		// XXX -- figure out some way to check if a menu is active
		UINT flags = 0;
		if ((menu & 0x80) != 0)
			flags |= 1;

		// map the key event to a character.  this has the side
		// effect of removing the dead key from the system's keyboard
		// layout buffer.
		WORD c        = 0;
		UINT scanCode = ((lParam & 0x00ff0000u) >> 16);
		int n         = ToAscii(wParam, scanCode, keys, &c, flags);

		// if mapping failed and ctrl and alt are pressed then try again
		// with both not pressed.  this handles the case where ctrl and
		// alt are being used as individual modifiers rather than AltGr.
		// we have to put the dead key back first, if there was one.
		if (n == 0 && (control & 0x80) != 0 && (menu & 0x80) != 0) {
			if (g_deadVirtKey != 0) {
				ToAscii(g_deadVirtKey, (g_deadLParam & 0x00ff0000u) >> 16,
							g_deadKeyState, &c, flags);
			}
			keys[VK_LCONTROL] = 0;
			keys[VK_RCONTROL] = 0;
			keys[VK_CONTROL]  = 0;
			keys[VK_LMENU]    = 0;
			keys[VK_RMENU]    = 0;
			keys[VK_MENU]     = 0;
			n = ToAscii(wParam, scanCode, keys, &c, flags);
		}

		switch (n) {
		default:
			// key is a dead key;  we're not expecting this since we
			// bailed out above for any dead key.
			g_deadVirtKey = wParam;
			g_deadLParam  = lParam;
			break;

		case 0:
			// key doesn't map to a character.  this can happen if
			// non-character keys are pressed after a dead key.
			break;

		case 1:
			// key maps to a character composed with dead key
			charAndVirtKey = makeKeyMsg(wParam, (char)LOBYTE(c));
			break;

		case 2: {
			// previous dead key not composed.  send a fake key press
			// and release for the dead key to our window.
			WPARAM deadCharAndVirtKey =
							makeKeyMsg(g_deadVirtKey, (char)LOBYTE(c));
			PostThreadMessage(g_threadID, SYNERGY_MSG_KEY,
							deadCharAndVirtKey, g_deadLParam & 0x7fffffffu);
			PostThreadMessage(g_threadID, SYNERGY_MSG_KEY,
							deadCharAndVirtKey, g_deadLParam | 0x80000000u);

			// use uncomposed character
			charAndVirtKey = makeKeyMsg(wParam, (char)HIBYTE(c));
			break;
		}
		}

		// put back the dead key, if any, for the application to use
		if (g_deadVirtKey != 0) {
			ToAscii(g_deadVirtKey, (g_deadLParam & 0x00ff0000u) >> 16,
							g_deadKeyState, &c, flags);
		}

		// clear out old dead key state
		g_deadVirtKey = 0;
		g_deadLParam  = 0;
	}

	// forward message to our window.  do this whether or not we're
	// forwarding events to clients because this'll keep our thread's
	// key state table up to date.  that's important for querying
	// the scroll lock toggle state.
	PostThreadMessage(g_threadID, SYNERGY_MSG_KEY, charAndVirtKey, lParam);

	// send fake key release if the user just pressed two dead keys
	// in a row, otherwise we'll lose the release because we always
	// return from the top of this function for all dead key releases.
	if ((c & 0x80000000u) != 0) {
		PostThreadMessage(g_threadID, SYNERGY_MSG_KEY,
							charAndVirtKey, lParam | 0x80000000u);
	}

	if (g_mode == kHOOK_RELAY_EVENTS) {
		// let certain keys pass through
		switch (wParam) {
		case VK_CAPITAL:
		case VK_NUMLOCK:
		case VK_SCROLL:
			// pass event on.  we want to let these through to
			// the window proc because otherwise the keyboard
			// lights may not stay synchronized.
			break;

		case VK_SHIFT:
		case VK_LSHIFT:
		case VK_RSHIFT:
		case VK_CONTROL:
		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_MENU:
		case VK_LMENU:
		case VK_RMENU:
		case VK_HANGUL:
			// always pass the shift modifiers
			break;

		default:
			// discard
			return true;
		}
	}

	return false;
}
#endif

static
bool
mouseHookHandler(WPARAM wParam, SInt32 x, SInt32 y, SInt32 data)
{
	attachThreadToForeground();

	switch (wParam) {
	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_XBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_MBUTTONDBLCLK:
	case WM_RBUTTONDBLCLK:
	case WM_XBUTTONDBLCLK:
	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
	case WM_XBUTTONUP:
	case WM_NCLBUTTONDOWN:
	case WM_NCMBUTTONDOWN:
	case WM_NCRBUTTONDOWN:
	case WM_NCXBUTTONDOWN:
	case WM_NCLBUTTONDBLCLK:
	case WM_NCMBUTTONDBLCLK:
	case WM_NCRBUTTONDBLCLK:
	case WM_NCXBUTTONDBLCLK:
	case WM_NCLBUTTONUP:
	case WM_NCMBUTTONUP:
	case WM_NCRBUTTONUP:
	case WM_NCXBUTTONUP:
		// always relay the event.  eat it if relaying.
		PostThreadMessage(g_threadID, SYNERGY_MSG_MOUSE_BUTTON, wParam, data);
		return (g_mode == kHOOK_RELAY_EVENTS);

	case WM_MOUSEWHEEL:
		if (g_mode == kHOOK_RELAY_EVENTS) {
			// relay event
			PostThreadMessage(g_threadID, SYNERGY_MSG_MOUSE_WHEEL, data, 0);
		}
		return (g_mode == kHOOK_RELAY_EVENTS);

	case WM_NCMOUSEMOVE:
	case WM_MOUSEMOVE:
		if (g_mode == kHOOK_RELAY_EVENTS) {
			// relay and eat event
			PostThreadMessage(g_threadID, SYNERGY_MSG_MOUSE_MOVE, x, y);
			return true;
		}
		else if (g_mode == kHOOK_WATCH_JUMP_ZONE) {
			// check for mouse inside jump zone
			bool inside = false;
			if (!inside && (g_zoneSides & kLeftMask) != 0) {
				inside = (x < g_xScreen + g_zoneSize);
			}
			if (!inside && (g_zoneSides & kRightMask) != 0) {
				inside = (x >= g_xScreen + g_wScreen - g_zoneSize);
			}
			if (!inside && (g_zoneSides & kTopMask) != 0) {
				inside = (y < g_yScreen + g_zoneSize);
			}
			if (!inside && (g_zoneSides & kBottomMask) != 0) {
				inside = (y >= g_yScreen + g_hScreen - g_zoneSize);
			}

			// relay the event
			PostThreadMessage(g_threadID, SYNERGY_MSG_MOUSE_MOVE, x, y);

			// if inside then eat the event
			return inside;
		}
	}

	// pass the event
	return false;
}

#if !NO_GRAB_KEYBOARD
static
LRESULT CALLBACK
keyboardHook(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0) {
		// handle the message
		if (keyboardHookHandler(wParam, lParam)) {
			return 1;
		}
	}

	return CallNextHookEx(g_keyboard, code, wParam, lParam);
}
#endif

static
LRESULT CALLBACK
mouseHook(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0) {
		// decode message
		const MOUSEHOOKSTRUCT* info = (const MOUSEHOOKSTRUCT*)lParam;
		SInt32 x = (SInt32)info->pt.x;
		SInt32 y = (SInt32)info->pt.y;
		SInt32 w = 0;
		if (wParam == WM_MOUSEWHEEL) {
			// win2k and other systems supporting WM_MOUSEWHEEL in
			// the mouse hook are gratuitously different (and poorly
			// documented).  if a low-level mouse hook is in place
			// it should capture these events so we'll never see
			// them.
			switch (g_wheelSupport) {
			case kWheelModern:
				w = static_cast<SInt32>(LOWORD(info->dwExtraInfo));
				break;

			case kWheelWin2000: {
				const MOUSEHOOKSTRUCTWin2000* info2k =
						(const MOUSEHOOKSTRUCTWin2000*)lParam;
				w = static_cast<SInt32>(HIWORD(info2k->mouseData));
				break;
			}
			}
		}

		// handle the message.  note that we don't handle X buttons
		// here.  that's okay because they're only supported on
		// win2k and winxp and up and on those platforms we'll get
		// get the mouse events through the low level hook.
		if (mouseHookHandler(wParam, x, y, w)) {
			return 1;
		}
	}

	return CallNextHookEx(g_mouse, code, wParam, lParam);
}

static
LRESULT CALLBACK
getMessageHook(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0) {
		if (g_screenSaver) {
			MSG* msg = reinterpret_cast<MSG*>(lParam);
			if (msg->message == WM_SYSCOMMAND &&
				msg->wParam  == SC_SCREENSAVE) {
				// broadcast screen saver started message
				PostThreadMessage(g_threadID,
								SYNERGY_MSG_SCREEN_SAVER, TRUE, 0);
			}
		}
		if (g_mode == kHOOK_RELAY_EVENTS) {
			MSG* msg = reinterpret_cast<MSG*>(lParam);
			if (msg->message == g_wmMouseWheel) {
				// post message to our window
				PostThreadMessage(g_threadID,
								SYNERGY_MSG_MOUSE_WHEEL, msg->wParam, 0);

				// zero out the delta in the message so it's (hopefully)
				// ignored
				msg->wParam = 0;
			}
		}
	}

	return CallNextHookEx(g_getMessage, code, wParam, lParam);
}

#if (_WIN32_WINNT >= 0x0400) && !NO_LOWLEVEL_HOOKS

//
// low-level keyboard hook -- this allows us to capture and handle
// alt+tab, alt+esc, ctrl+esc, and windows key hot keys.  on the down
// side, key repeats are not reported to us.
//

#if !NO_GRAB_KEYBOARD
static
LRESULT CALLBACK
keyboardLLHook(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0) {
		// decode the message
		KBDLLHOOKSTRUCT* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
		WPARAM wParam = info->vkCode;
		LPARAM lParam = 1;							// repeat code
		lParam      |= (info->scanCode << 16);		// scan code
		if (info->flags & LLKHF_EXTENDED) {
			lParam  |= (1lu << 24);					// extended key
		}
		if (info->flags & LLKHF_ALTDOWN) {
			lParam  |= (1lu << 29);					// context code
		}
		if (info->flags & LLKHF_UP) {
			lParam  |= (1lu << 31);					// transition
		}
		// FIXME -- bit 30 should be set if key was already down but
		// we don't know that info.  as a result we'll never generate
		// key repeat events.

		// handle the message
		if (keyboardHookHandler(wParam, lParam)) {
			return 1;
		}
	}

	return CallNextHookEx(g_keyboardLL, code, wParam, lParam);
}
#endif

//
// low-level mouse hook -- this allows us to capture and handle mouse
// events very early.  the earlier the better.
//

static
LRESULT CALLBACK
mouseLLHook(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0) {
		// decode the message
		MSLLHOOKSTRUCT* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
		SInt32 x = (SInt32)info->pt.x;
		SInt32 y = (SInt32)info->pt.y;
		SInt32 w = (SInt32)HIWORD(info->mouseData);

		// handle the message
		if (mouseHookHandler(wParam, x, y, w)) {
			return 1;
		}
	}

	return CallNextHookEx(g_mouseLL, code, wParam, lParam);
}

#endif

static
EWheelSupport
getWheelSupport()
{
	// get operating system
	OSVERSIONINFO info;
	info.dwOSVersionInfoSize = sizeof(info);
	if (!GetVersionEx(&info)) {
		return kWheelNone;
	}

	// see if modern wheel is present
	if (GetSystemMetrics(SM_MOUSEWHEELPRESENT)) {
		// note if running on win2k
		if (info.dwPlatformId   == VER_PLATFORM_WIN32_NT &&
			info.dwMajorVersion == 5 &&
			info.dwMinorVersion == 0) {
			return kWheelWin2000;
		}
		return kWheelModern;
	}

	// not modern.  see if we've got old-style support.
	UINT wheelSupportMsg    = RegisterWindowMessage(MSH_WHEELSUPPORT);
	HWND wheelSupportWindow = FindWindow(MSH_WHEELMODULE_CLASS,
										MSH_WHEELMODULE_TITLE);
	if (wheelSupportWindow != NULL && wheelSupportMsg != 0) {
		if (SendMessage(wheelSupportWindow, wheelSupportMsg, 0, 0) != 0) {
			g_wmMouseWheel = RegisterWindowMessage(MSH_MOUSEWHEEL);
			if (g_wmMouseWheel != 0) {
				return kWheelOld;
			}
		}
	}

	// assume modern.  we don't do anything special in this case
	// except respond to WM_MOUSEWHEEL messages.  GetSystemMetrics()
	// can apparently return FALSE even if a mouse wheel is present
	// though i'm not sure exactly when it does that (WinME returns
	// FALSE for my logitech USB trackball).
	return kWheelModern;
}


//
// external functions
//

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(instance);
		if (g_processID == 0) {
			g_hinstance = instance;
			g_processID = GetCurrentProcessId();
		}
	}
	else if (reason == DLL_PROCESS_DETACH) {
		if (g_processID == GetCurrentProcessId()) {
			uninstall();
			uninstallScreenSaver();
			g_processID = 0;
			g_hinstance = NULL;
		}
	}
	return TRUE;
}

extern "C" {

int
init(DWORD threadID)
{
	assert(g_hinstance != NULL);

	// try to open process that last called init() to see if it's
	// still running or if it died without cleaning up.
	if (g_processID != 0 && g_processID != GetCurrentProcessId()) {
		HANDLE process = OpenProcess(STANDARD_RIGHTS_REQUIRED,
								FALSE, g_processID);
		if (process != NULL) {
			// old process (probably) still exists so refuse to
			// reinitialize this DLL (and thus steal it from the
			// old process).
			CloseHandle(process);
			return 0;
		}

		// clean up after old process.  the system should've already
		// removed the hooks so we just need to reset our state.
		g_hinstance       = GetModuleHandle("synrgyhk");
		g_processID       = GetCurrentProcessId();
		g_wheelSupport    = kWheelNone;
		g_threadID        = 0;
		g_keyboard        = NULL;
		g_mouse           = NULL;
		g_getMessage      = NULL;
		g_keyboardLL      = NULL;
		g_mouseLL         = NULL;
		g_screenSaver     = false;
	}

	// save thread id.  we'll post messages to this thread's
	// message queue.
	g_threadID     = threadID;

	// set defaults
	g_mode      = kHOOK_DISABLE;
	g_zoneSides = 0;
	g_zoneSize  = 0;
	g_xScreen   = 0;
	g_yScreen   = 0;
	g_wScreen   = 0;
	g_hScreen   = 0;

	return 1;
}

int
cleanup(void)
{
	assert(g_hinstance != NULL);

	if (g_processID == GetCurrentProcessId()) {
		g_threadID = 0;
	}

	return 1;
}

EHookResult
install()
{
	assert(g_hinstance  != NULL);
	assert(g_keyboard   == NULL);
	assert(g_mouse      == NULL);
	assert(g_getMessage == NULL || g_screenSaver);

	// must be initialized
	if (g_threadID == 0) {
		return kHOOK_FAILED;
	}

	// discard old dead keys
	g_deadVirtKey = 0;
	g_deadLParam  = 0;

	// check for mouse wheel support
	g_wheelSupport = getWheelSupport();

	// install GetMessage hook (unless already installed)
	if (g_wheelSupport == kWheelOld && g_getMessage == NULL) {
		g_getMessage = SetWindowsHookEx(WH_GETMESSAGE,
								&getMessageHook,
								g_hinstance,
								0);
	}

	// install low-level hooks.  we require that they both get installed.
#if (_WIN32_WINNT >= 0x0400) && !NO_LOWLEVEL_HOOKS
	g_mouseLL = SetWindowsHookEx(WH_MOUSE_LL,
								&mouseLLHook,
								g_hinstance,
								0);
#if !NO_GRAB_KEYBOARD
	g_keyboardLL = SetWindowsHookEx(WH_KEYBOARD_LL,
								&keyboardLLHook,
								g_hinstance,
								0);
	if (g_mouseLL == NULL || g_keyboardLL == NULL) {
		if (g_keyboardLL != NULL) {
			UnhookWindowsHookEx(g_keyboardLL);
			g_keyboardLL = NULL;
		}
		if (g_mouseLL != NULL) {
			UnhookWindowsHookEx(g_mouseLL);
			g_mouseLL = NULL;
		}
	}
#endif
#endif

	// install regular hooks
	if (g_mouseLL == NULL) {
		g_mouse = SetWindowsHookEx(WH_MOUSE,
								&mouseHook,
								g_hinstance,
								0);
	}
	if (g_keyboardLL == NULL) {
		g_keyboard = SetWindowsHookEx(WH_KEYBOARD,
								&keyboardHook,
								g_hinstance,
								0);
	}

	// check that we got all the hooks we wanted
	if ((g_getMessage == NULL && g_wheelSupport == kWheelOld) ||
#if !NO_GRAB_KEYBOARD
		(g_keyboardLL == NULL && g_keyboard     == NULL) ||
#endif
		(g_mouseLL    == NULL && g_mouse        == NULL)) {
		uninstall();
		return kHOOK_FAILED;
	}

	if (g_keyboardLL != NULL || g_mouseLL != NULL) {
		g_hookThread = GetCurrentThreadId();
		return kHOOK_OKAY_LL;
	}

	return kHOOK_OKAY;
}

int
uninstall(void)
{
	assert(g_hinstance != NULL);

	// discard old dead keys
	g_deadVirtKey = 0;
	g_deadLParam  = 0;

	// detach from thread
	detachThread();

	// uninstall hooks
	if (g_keyboardLL != NULL) {
		UnhookWindowsHookEx(g_keyboardLL);
		g_keyboardLL = NULL;
	}
	if (g_mouseLL != NULL) {
		UnhookWindowsHookEx(g_mouseLL);
		g_mouseLL = NULL;
	}
	if (g_keyboard != NULL) {
		UnhookWindowsHookEx(g_keyboard);
		g_keyboard = NULL;
	}
	if (g_mouse != NULL) {
		UnhookWindowsHookEx(g_mouse);
		g_mouse = NULL;
	}
	if (g_getMessage != NULL && !g_screenSaver) {
		UnhookWindowsHookEx(g_getMessage);
		g_getMessage = NULL;
	}
	g_wheelSupport = kWheelNone;

	return 1;
}

int
installScreenSaver(void)
{
	assert(g_hinstance != NULL);

	// must be initialized
	if (g_threadID == 0) {
		return 0;
	}

	// generate screen saver messages
	g_screenSaver = true;

	// install hook unless it's already installed
	if (g_getMessage == NULL) {
		g_getMessage = SetWindowsHookEx(WH_GETMESSAGE,
								&getMessageHook,
								g_hinstance,
								0);
	}

	return (g_getMessage != NULL) ? 1 : 0;
}

int
uninstallScreenSaver(void)
{
	assert(g_hinstance != NULL);

	// uninstall hook unless the mouse wheel hook is installed
	if (g_getMessage != NULL && g_wheelSupport != kWheelOld) {
		UnhookWindowsHookEx(g_getMessage);
		g_getMessage = NULL;
	}

	// screen saver hook is no longer installed
	g_screenSaver = false;

	return 1;
}

void
setSides(UInt32 sides)
{
	g_zoneSides = sides;
}

void
setZone(SInt32 x, SInt32 y, SInt32 w, SInt32 h, SInt32 jumpZoneSize)
{
	g_zoneSize = jumpZoneSize;
	g_xScreen  = x;
	g_yScreen  = y;
	g_wScreen  = w;
	g_hScreen  = h;
}

void
setMode(EHookMode mode)
{
	if (mode == g_mode) {
		// no change
		return;
	}
	g_mode = mode;
}

}
