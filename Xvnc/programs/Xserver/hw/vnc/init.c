/*
 * init.c
 */

/*
 *  Copyright (C) 2002-2003 RealVNC Ltd.
 *  Copyright (C) 1999 AT&T Laboratories Cambridge.  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

/*

Copyright (c) 1993  X Consortium

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE X CONSORTIUM BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of the X Consortium shall
not be used in advertising or otherwise to promote the sale, use or
other dealings in this Software without prior written authorization
from the X Consortium.

*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include "X11/X.h"
#define NEED_EVENTS
#include "X11/Xproto.h"
#include "X11/Xos.h"
#include "scrnintstr.h"
#include "servermd.h"
#define PSZ 8
#include "cfb.h"
#include "mi.h"
#include "mibstore.h"
#include "colormapst.h"
#include "gcstruct.h"
#include "input.h"
#include "mipointer.h"
#include "dixstruct.h"
#include "propertyst.h"
#include <Xatom.h>
#include <errno.h>
#include <sys/param.h>
#include "dix.h"
#include "rfb.h"
#include "dispcur.h"

extern Bool cfb16ScreenInit(ScreenPtr, pointer, int, int, int, int, int);
extern Bool cfb32ScreenInit(ScreenPtr, pointer, int, int, int, int, int);

#define RFB_DEFAULT_WIDTH  640
#define RFB_DEFAULT_HEIGHT 480
#define RFB_DEFAULT_DEPTH  8
#define RFB_DEFAULT_WHITEPIXEL 0
#define RFB_DEFAULT_BLACKPIXEL 1

rfbScreenInfo rfbScreen;
int rfbGCIndex;

static Bool initOutputCalled = FALSE;
static Bool noCursor = FALSE;
char *desktopName = "x11";
Bool rfbTrace = FALSE;

char rfbThisHost[256];

Atom VNC_LAST_CLIENT_ID;
Atom VNC_CONNECT;
Atom VNC_DEFER_UPDATE;

static HWEventQueueType alwaysCheckForInput[2] = { 0, 1 };
static HWEventQueueType *mieqCheckForInput[2];

static char primaryOrder[4] = "";
static int redBits, greenBits, blueBits;


static Bool rfbScreenInit(int index, ScreenPtr pScreen, int argc,
			  char **argv);
static int rfbKeybdProc(DeviceIntPtr pDevice, int onoff);
static int rfbMouseProc(DeviceIntPtr pDevice, int onoff);
static Bool CheckDisplayNumber(int n);

static Bool rfbAlwaysTrue();
static char *rfbAllocateFramebufferMemory(rfbScreenInfoPtr prfb);
static Bool rfbCursorOffScreen(ScreenPtr *ppScreen, int *x, int *y);
static void rfbCrossScreen(ScreenPtr pScreen, Bool entering);
static void rfbClientStateChange(CallbackListPtr *, pointer myData,
				 pointer client);

static miPointerScreenFuncRec rfbPointerCursorFuncs = {
    rfbCursorOffScreen,
    rfbCrossScreen,
    miPointerWarpCursor
};


int inetdSock = -1;
static char inetdDisplayNumStr[10];

extern char buildtime[];


/*
 * ddxProcessArgument is our first entry point and will be called at the
 * very start for each argument.  It is not called again on server reset.
 */

int
ddxProcessArgument (argc, argv, i)
    int argc;
    char *argv[];
    int i;
{
    static Bool firstTime = TRUE;

    if (firstTime)
    {
	rfbScreen.width  = RFB_DEFAULT_WIDTH;
	rfbScreen.height = RFB_DEFAULT_HEIGHT;
	rfbScreen.depth  = RFB_DEFAULT_DEPTH;
	rfbScreen.blackPixel = RFB_DEFAULT_BLACKPIXEL;
	rfbScreen.whitePixel = RFB_DEFAULT_WHITEPIXEL;
	rfbScreen.pfbMemory = NULL;
	gethostname(rfbThisHost, 255);
        firstTime = FALSE;
    }

    if (strcasecmp (argv[i], "-geometry") == 0)	/* -geometry WxH */
    {
	if (i + 1 >= argc) UseMsg();
	if (sscanf(argv[i+1],"%dx%d",
		   &rfbScreen.width,&rfbScreen.height) != 2) {
	    ErrorF("Invalid geometry %s\n", argv[i+1]);
	    UseMsg();
	}
	return 2;
    }

    if (strcasecmp (argv[i], "-depth") == 0)	/* -depth D */
    {
	if (i + 1 >= argc) UseMsg();
	rfbScreen.depth = atoi(argv[i+1]);
	return 2;
    }

    if (strcasecmp (argv[i], "-pixelformat") == 0) {
	if (i + 1 >= argc) UseMsg();
	if (sscanf(argv[i+1], "%3s%1d%1d%1d", primaryOrder,
		   &redBits, &greenBits, &blueBits) < 4) {
	    ErrorF("Invalid pixel format %s\n", argv[i+1]);
	    UseMsg();
	}

	if (strcasecmp(primaryOrder, "bgr") == 0) {
	    int tmp = redBits;
	    redBits = blueBits;
	    blueBits = tmp;
	} else if (strcasecmp(primaryOrder, "rgb") != 0) {
	    ErrorF("Invalid pixel format %s\n", argv[i+1]);
	    UseMsg();
	}

	return 2;
    }

    if (strcasecmp (argv[i], "-blackpixel") == 0) {	/* -blackpixel n */
	if (i + 1 >= argc) UseMsg();
	rfbScreen.blackPixel = atoi(argv[i+1]);
	return 2;
    }

    if (strcasecmp (argv[i], "-whitepixel") == 0) {	/* -whitepixel n */
	if (i + 1 >= argc) UseMsg();
	rfbScreen.whitePixel = atoi(argv[i+1]);
	return 2;
    }

    if (strcasecmp(argv[i], "-rfbport") == 0) {	/* -rfbport port */
	if (i + 1 >= argc) UseMsg();
	rfbPort = atoi(argv[i+1]);
	return 2;
    }

    if (strcasecmp(argv[i], "-rfbwait") == 0) {	/* -rfbwait ms */
	if (i + 1 >= argc) UseMsg();
	rfbMaxClientWait = atoi(argv[i+1]);
	return 2;
    }

    if (strcasecmp(argv[i], "-nocursor") == 0) {
	noCursor = TRUE;
	return 1;
    }

    if (strcasecmp(argv[i], "-rfbauth") == 0) {	/* -rfbauth passwd-file */
	if (i + 1 >= argc) UseMsg();
	rfbAuthPasswdFile = argv[i+1];
	return 2;
    }

    if (strcasecmp(argv[i], "-httpd") == 0) {
	if (i + 1 >= argc) UseMsg();
	httpDir = argv[i+1];
	return 2;
    }

    if (strcasecmp(argv[i], "-httpport") == 0) {
	if (i + 1 >= argc) UseMsg();
	httpPort = atoi(argv[i+1]);
	return 2;
    }

    if (strcasecmp(argv[i], "-deferupdate") == 0) {	/* -deferupdate ms */
	if (i + 1 >= argc) UseMsg();
	rfbDeferUpdateTime = atoi(argv[i+1]);
	return 2;
    }

    if (strcasecmp(argv[i], "-economictranslate") == 0) {
	rfbEconomicTranslate = TRUE;
	return 1;
    }

    if (strcasecmp(argv[i], "-maxrects") == 0) {
	if (i + 1 >= argc) UseMsg();
	rfbMaxRects = atoi(argv[i+1]);
	return 2;
    }

    if (strcasecmp(argv[i], "-trace") == 0) {
        rfbTrace = TRUE;
	return 1;
    }

    if (strcasecmp(argv[i], "-desktop") == 0) {	/* -desktop desktop-name */
	if (i + 1 >= argc) UseMsg();
	desktopName = argv[i+1];
	return 2;
    }

    if (strcasecmp(argv[i], "-alwaysshared") == 0) {
	rfbAlwaysShared = TRUE;
	return 1;
    }

    if (strcasecmp(argv[i], "-nevershared") == 0) {
	rfbNeverShared = TRUE;
	return 1;
    }

    if (strcasecmp(argv[i], "-dontdisconnect") == 0) {
	rfbDontDisconnect = TRUE;
	return 1;
    }

    if (strcasecmp(argv[i], "-localhost") == 0) {
	rfbLocalhostOnly = TRUE;
	return 1;
    }

    if (strcasecmp(argv[i], "-inetd") == 0) {	/* -inetd */ 
	int n;
	for (n = 1; n < 100; n++) {
	    if (CheckDisplayNumber(n))
		break;
	}

	if (n >= 100)
	    FatalError("-inetd: couldn't find free display number");

	sprintf(inetdDisplayNumStr, "%d", n);
	display = inetdDisplayNumStr;

	/* fds 0, 1 and 2 (stdin, out and err) are all the same socket to the
           RFB client.  OsInit() closes stdout and stdin, and we don't want
           stderr to go to the RFB client, so make the client socket 3 and
           close stderr.  OsInit() will redirect stderr logging to an
           appropriate log file or /dev/null if that doesn't work. */

	dup2(0,3);
	inetdSock = 3;
	close(2);

	return 1;
    }

    if (inetdSock != -1 && argv[i][0] == ':') {
	FatalError("can't specify both -inetd and :displaynumber");
    }

    return 0;
}


/*
 * InitOutput is called every time the server resets.  It should call
 * AddScreen for each screen (but we only ever have one), and in turn this
 * will call rfbScreenInit.
 */

void
InitOutput(screenInfo, argc, argv)
    ScreenInfo *screenInfo;
    int argc;
    char **argv;
{
    initOutputCalled = TRUE;

    rfbLog("Xvnc version %s - built %s\n", XVNCRELEASE, buildtime);
    rfbLog("Copyright (C) 2002-2003 RealVNC Ltd.\n");
    rfbLog("Copyright (C) 1994-2000 AT&T Laboratories Cambridge.\n");
    rfbLog("All Rights Reserved.\n");
    rfbLog("See http://www.realvnc.com for information on VNC\n");
    rfbLog("Desktop name '%s' (%s:%s)\n",desktopName,rfbThisHost,display);
    rfbLog("Protocol version supported %d.%d\n", rfbProtocolMajorVersion,
	   rfbProtocolMinorVersion);

    VNC_LAST_CLIENT_ID = MakeAtom("VNC_LAST_CLIENT_ID",
				  strlen("VNC_LAST_CLIENT_ID"), TRUE);
    VNC_CONNECT = MakeAtom("VNC_CONNECT", strlen("VNC_CONNECT"), TRUE);
    VNC_DEFER_UPDATE = MakeAtom("VNC_DEFER_UPDATE",
                                strlen("VNC_DEFER_UPDATE"), TRUE);
    rfbInitSockets();
    if (inetdSock == -1)
	httpInitSockets();
   

    /* initialize pixmap formats */

    screenInfo->imageByteOrder = IMAGE_BYTE_ORDER;
    screenInfo->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screenInfo->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screenInfo->bitmapBitOrder = BITMAP_BIT_ORDER;
    screenInfo->numPixmapFormats = 2;

    screenInfo->formats[0].depth = 1;
    screenInfo->formats[0].bitsPerPixel = 1;
    screenInfo->formats[0].scanlinePad = BITMAP_SCANLINE_PAD;

    screenInfo->formats[1].depth = rfbScreen.depth;
    screenInfo->formats[1].bitsPerPixel = rfbBitsPerPixel(rfbScreen.depth);
    screenInfo->formats[1].scanlinePad = BITMAP_SCANLINE_PAD;

    rfbGCIndex = AllocateGCPrivateIndex();
    if (rfbGCIndex < 0) {
	FatalError("InitOutput: AllocateGCPrivateIndex failed\n");
    }

    if (!AddCallback(&ClientStateCallback, rfbClientStateChange, NULL)) {
	rfbLog("InitOutput: AddCallback failed\n");
	return;
    }

    /* initialize screen */

    if (AddScreen(rfbScreenInit, argc, argv) == -1) {
	FatalError("Couldn't add screen");
    }
}


static Bool
rfbScreenInit(index, pScreen, argc, argv)
    int index;
    ScreenPtr pScreen;
    int argc;
    char ** argv;
{
    rfbScreenInfoPtr prfb = &rfbScreen;
    int dpix = 75, dpiy = 75;
    int ret;
    char *pbits;
    VisualPtr vis;
    extern int monitorResolution;

    if (monitorResolution != 0) {
	dpix = monitorResolution;
	dpiy = monitorResolution;
    }

    prfb->paddedWidthInBytes = PixmapBytePad(prfb->width, prfb->depth);
    prfb->bitsPerPixel = rfbBitsPerPixel(prfb->depth);
    pbits = rfbAllocateFramebufferMemory(prfb);
    if (!pbits) return FALSE;

    if (prfb->bitsPerPixel > 1) {
	extern int defaultColorVisualClass;
	if (defaultColorVisualClass != -1) {
	    cfbSetVisualTypes(prfb->depth, (1 << defaultColorVisualClass), 8);
	} else {
	    cfbSetVisualTypes(prfb->depth, (1 << TrueColor), 8);
	}
    }

    switch (prfb->bitsPerPixel)
    {
    case 1:
	ret = mfbScreenInit(pScreen, pbits, prfb->width, prfb->height,
			    dpix, dpiy, prfb->paddedWidthInBytes * 8);
	break;
    case 8:
	ret = cfbScreenInit(pScreen, pbits, prfb->width, prfb->height,
			    dpix, dpiy, prfb->paddedWidthInBytes);
	break;
    case 16:
	ret = cfb16ScreenInit(pScreen, pbits, prfb->width, prfb->height,
			      dpix, dpiy, prfb->paddedWidthInBytes / 2);
	break;
    case 32:
	ret = cfb32ScreenInit(pScreen, pbits, prfb->width, prfb->height,
			      dpix, dpiy, prfb->paddedWidthInBytes / 4);
	break;
    default:
	return FALSE;
    }

    if (!ret) return FALSE;

    if (!AllocateGCPrivate(pScreen, rfbGCIndex, sizeof(rfbGCRec))) {
	FatalError("rfbScreenInit: AllocateGCPrivate failed\n");
    }

    prfb->cursorIsDrawn = FALSE;
    prfb->dontSendFramebufferUpdate = FALSE;

    prfb->CloseScreen = pScreen->CloseScreen;
    prfb->CreateGC = pScreen->CreateGC;
    prfb->PaintWindowBackground = pScreen->PaintWindowBackground;
    prfb->PaintWindowBorder = pScreen->PaintWindowBorder;
    prfb->CopyWindow = pScreen->CopyWindow;
    prfb->ClearToBackground = pScreen->ClearToBackground;
    prfb->RestoreAreas = pScreen->RestoreAreas;

    pScreen->CloseScreen = rfbCloseScreen;
    pScreen->CreateGC = rfbCreateGC;
    pScreen->PaintWindowBackground = rfbPaintWindowBackground;
    pScreen->PaintWindowBorder = rfbPaintWindowBorder;
    pScreen->CopyWindow = rfbCopyWindow;
    pScreen->ClearToBackground = rfbClearToBackground;
    pScreen->RestoreAreas = rfbRestoreAreas;

    pScreen->InstallColormap = rfbInstallColormap;
    pScreen->UninstallColormap = rfbUninstallColormap;
    pScreen->ListInstalledColormaps = rfbListInstalledColormaps;
    pScreen->StoreColors = rfbStoreColors;

    pScreen->SaveScreen = rfbAlwaysTrue;

    rfbDCInitialize(pScreen, &rfbPointerCursorFuncs);

    if (noCursor) {
	pScreen->DisplayCursor = rfbAlwaysTrue;
	prfb->cursorIsDrawn = TRUE;
    }

    pScreen->blackPixel = prfb->blackPixel;
    pScreen->whitePixel = prfb->whitePixel;

    for (vis = pScreen->visuals; vis->vid != pScreen->rootVisual; vis++)
	;

    if (!vis) {
	rfbLog("rfbScreenInit: couldn't find root visual\n");
	exit(1);
    }

    if (strcmp(primaryOrder, "") == 0) {
      if (prfb->depth == 16) { /* use rgb565 for depth 16 */
        strcpy(primaryOrder, "rgb");
        redBits = 5;
        greenBits = 6;
        blueBits = 5;
      } else if (prfb->depth == 24) { /* use rgb888 for depth 24 */
        strcpy(primaryOrder, "rgb");
        redBits = 8;
        greenBits = 8;
        blueBits = 8;
      }
    }

    if (strcasecmp(primaryOrder, "rgb") == 0) {
	vis->offsetBlue = 0;
	vis->blueMask = (1 << blueBits) - 1;
	vis->offsetGreen = blueBits;
	vis->greenMask = ((1 << greenBits) - 1) << vis->offsetGreen;
	vis->offsetRed = vis->offsetGreen + greenBits;
	vis->redMask = ((1 << redBits) - 1) << vis->offsetRed;
    } else if (strcasecmp(primaryOrder, "bgr") == 0) {
	rfbLog("BGR format %d %d %d\n", blueBits, greenBits, redBits);
	vis->offsetRed = 0;
	vis->redMask = (1 << redBits) - 1;
	vis->offsetGreen = redBits;
	vis->greenMask = ((1 << greenBits) - 1) << vis->offsetGreen;
	vis->offsetBlue = vis->offsetGreen + greenBits;
	vis->blueMask = ((1 << blueBits) - 1) << vis->offsetBlue;
    }

    rfbServerFormat.bitsPerPixel = prfb->bitsPerPixel;
    rfbServerFormat.depth = prfb->depth;
    rfbServerFormat.bigEndian = !(*(char *)&rfbEndianTest);
    rfbServerFormat.trueColour = (vis->class == TrueColor);
    if (rfbServerFormat.trueColour) {
	rfbServerFormat.redMax = vis->redMask >> vis->offsetRed;
	rfbServerFormat.greenMax = vis->greenMask >> vis->offsetGreen;
	rfbServerFormat.blueMax = vis->blueMask >> vis->offsetBlue;
	rfbServerFormat.redShift = vis->offsetRed;
	rfbServerFormat.greenShift = vis->offsetGreen;
	rfbServerFormat.blueShift = vis->offsetBlue;
    } else {
	rfbServerFormat.redMax
	    = rfbServerFormat.greenMax = rfbServerFormat.blueMax = 0;
	rfbServerFormat.redShift
	    = rfbServerFormat.greenShift = rfbServerFormat.blueShift = 0;
    }

    if (prfb->bitsPerPixel == 1)
    {
	ret = mfbCreateDefColormap(pScreen);
    }
    else
    {
	ret = cfbCreateDefColormap(pScreen);
    }

    return ret;

} /* end rfbScreenInit */



/*
 * InitInput is also called every time the server resets.  It is called after
 * InitOutput so we can assume that rfbInitSockets has already been called.
 */

void
InitInput(argc, argv)
    int argc;
    char *argv[];
{
    DeviceIntPtr p, k;
    k = AddInputDevice(rfbKeybdProc, TRUE);
    p = AddInputDevice(rfbMouseProc, TRUE);
    RegisterKeyboardDevice(k);
    RegisterPointerDevice(p);
    miRegisterPointerDevice(screenInfo.screens[0], p);
    mieqInit((DevicePtr)k, (DevicePtr)p);
    mieqCheckForInput[0] = checkForInput[0];
    mieqCheckForInput[1] = checkForInput[1];
    SetInputCheck(&alwaysCheckForInput[0], &alwaysCheckForInput[1]);
}


static int
rfbKeybdProc(pDevice, onoff)
    DeviceIntPtr pDevice;
    int onoff;
{
    KeySymsRec		keySyms;
    CARD8 		modMap[MAP_LENGTH];
    DevicePtr pDev = (DevicePtr)pDevice;

    switch (onoff)
    {
    case DEVICE_INIT: 
	KbdDeviceInit(pDevice, &keySyms, modMap);
	InitKeyboardDeviceStruct(pDev, &keySyms, modMap,
				 (BellProcPtr)rfbSendBell,
				 (KbdCtrlProcPtr)NoopDDA);
	    break;
    case DEVICE_ON: 
	pDev->on = TRUE;
	KbdDeviceOn();
	break;
    case DEVICE_OFF: 
	pDev->on = FALSE;
	KbdDeviceOff();
	break;
    case DEVICE_CLOSE:
	if (pDev->on)
	    KbdDeviceOff();
	break;
    }
    return Success;
}

static int
rfbMouseProc(pDevice, onoff)
    DeviceIntPtr pDevice;
    int onoff;
{
    BYTE map[6];
    DevicePtr pDev = (DevicePtr)pDevice;

    switch (onoff)
    {
    case DEVICE_INIT:
	PtrDeviceInit();
	map[1] = 1;
	map[2] = 2;
	map[3] = 3;
	map[4] = 4;
	map[5] = 5;
	InitPointerDeviceStruct(pDev, map, 5, miPointerGetMotionEvents,
				PtrDeviceControl,
				miPointerGetMotionBufferSize());
	break;

    case DEVICE_ON:
	pDev->on = TRUE;
	PtrDeviceOn(pDevice);
        break;

    case DEVICE_OFF:
	pDev->on = FALSE;
	PtrDeviceOff();
	break;

    case DEVICE_CLOSE:
	if (pDev->on)
	    PtrDeviceOff();
	break;
    }
    return Success;
}


Bool
LegalModifier(key, pDev)
    unsigned int key;
    DevicePtr	pDev;
{
    return TRUE;
}


void
ProcessInputEvents()
{
    rfbCheckFds();
    httpCheckFds();
    if (*mieqCheckForInput[0] != *mieqCheckForInput[1]) {
	mieqProcessInputEvents();
	miPointerUpdate();
    }
}


static Bool CheckDisplayNumber(int n)
{
    char fname[32];
    int sock;
    struct sockaddr_in addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(6000+n);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
	close(sock);
	return FALSE;
    }
    close(sock);

    sprintf(fname, "/tmp/.X%d-lock", n);
    if (access(fname, F_OK) == 0)
	return FALSE;

    sprintf(fname, "/tmp/.X11-unix/X%d", n);
    if (access(fname, F_OK) == 0)
	return FALSE;

    sprintf(fname, "/usr/spool/sockets/X11/%d", n);
    if (access(fname, F_OK) == 0)
	return FALSE;

    return TRUE;
}


void
rfbRootPropertyChange(PropertyPtr pProp)
{
    if ((pProp->propertyName == XA_CUT_BUFFER0) && (pProp->type == XA_STRING)
	&& (pProp->format == 8))
    {
	rfbGotXCutText(pProp->data, pProp->size);
    }
    else if ((pProp->propertyName == VNC_CONNECT) && (pProp->type == XA_STRING)
             && (pProp->format == 8))
    {
	if (pProp->size == 0) {
          rfbClientPtr cl, nextCl;
          rfbLog("VNC_CONNECT message: disconnecting all clients\n");
          for (cl = rfbClientHead; cl; cl = nextCl) {
            nextCl = cl->next;
            rfbCloseSock(cl->sock);
          }
        } else {
          int i;
          rfbClientPtr cl;
          int port = 5500;
          char *host = (char *)Xalloc(pProp->size+1);
          memcpy(host, pProp->data, pProp->size);
          host[pProp->size] = 0;
          for (i = 0; i < pProp->size; i++) {
	    if (host[i] == ':') {
              port = atoi(&host[i+1]);
              host[i] = 0;
	    }
          }

          cl = rfbReverseConnection(host, port);

          free(host);
        }
        return;
    }
    else if ((pProp->propertyName == VNC_DEFER_UPDATE) &&
             (pProp->type == XA_STRING) && (pProp->format == 8))
    {
      char *str = (char *)Xalloc(pProp->size+1);
      memcpy(str, pProp->data, pProp->size);
      str[pProp->size] = 0;
      rfbDeferUpdateTime = atoi(str);
      free(str);
    }
}


int
rfbBitsPerPixel(depth)
    int depth;
{
    if (depth == 1) return 1;
    else if (depth <= 8) return 8;
    else if (depth <= 16) return 16;
    else return 32;
}


static Bool
rfbAlwaysTrue()
{
    return TRUE;
}


static char *
rfbAllocateFramebufferMemory(prfb)
    rfbScreenInfoPtr prfb;
{
    if (prfb->pfbMemory) return prfb->pfbMemory; /* already done */

    prfb->sizeInBytes = (prfb->paddedWidthInBytes * prfb->height);

    prfb->pfbMemory = (char *)Xalloc(prfb->sizeInBytes);

    return prfb->pfbMemory;
}


static Bool
rfbCursorOffScreen (ppScreen, x, y)
    ScreenPtr   *ppScreen;
    int         *x, *y;
{
    return FALSE;
}

static void
rfbCrossScreen (pScreen, entering)
    ScreenPtr   pScreen;
    Bool        entering;
{
}

static void
rfbClientStateChange(cbl, myData, clt)
    CallbackListPtr *cbl;
    pointer myData;
    pointer clt;
{
    dispatchException &= ~DE_RESET;	/* hack - force server not to reset */
}

void
ddxGiveUp()
{
    Xfree(rfbScreen.pfbMemory);
    if (initOutputCalled) {
	char unixSocketName[256];
	sprintf(unixSocketName,"/tmp/.X11-unix/X%s",display);
	unlink(unixSocketName);
	sprintf(unixSocketName,"/usr/spool/sockets/X11/%s",display);
	unlink(unixSocketName);
    }
}

void
AbortDDX()
{
    ddxGiveUp();
}

void
OsVendorInit()
{
}

void
OsVendorFatalError()
{
}

#ifdef DDXTIME /* from ServerOSDefines */
CARD32
GetTimeInMillis()
{
    struct timeval  tp;

    X_GETTIMEOFDAY(&tp);
    return(tp.tv_sec * 1000) + (tp.tv_usec / 1000);
}
#endif

void
ddxUseMsg()
{
    ErrorF("\nXvnc version %s - built %s\n\n", XVNCRELEASE, buildtime);
    ErrorF("-geometry WxH          set framebuffer width & height\n");
    ErrorF("-depth D               set framebuffer depth\n");
    ErrorF("-pixelformat format    set pixel format (BGRnnn or RGBnnn)\n");
    ErrorF("-rfbport port          TCP port for RFB protocol\n");
    ErrorF("-rfbwait time          max time in ms to wait for RFB client\n");
    ErrorF("-nocursor              don't put up a cursor\n");
    ErrorF("-rfbauth passwd-file   use authentication on RFB protocol\n");
    ErrorF("-httpd dir             serve files via HTTP from here\n");
    ErrorF("-httpport port         port for HTTP\n");
    ErrorF("-deferupdate time      time in ms to defer updates "
							     "(default 40)\n");
    ErrorF("-economictranslate     less memory-hungry translation\n");
    ErrorF("-maxrects num          max number of rectangles in an update "
							     "(default 50)\n");
    ErrorF("-desktop name          VNC desktop name (default x11)\n");
    ErrorF("-alwaysshared          always treat new clients as shared\n");
    ErrorF("-nevershared           never treat new clients as shared\n");
    ErrorF("-dontdisconnect        don't disconnect existing clients when a "
                                                             "new non-shared\n"
	   "                       connection comes in (refuse new connection "
								 "instead)\n");
    ErrorF("-localhost             only allow connections from localhost\n");
    ErrorF("-inetd                 Xvnc is launched by inetd\n");
    exit(1);
}

/*
 * rfbLog prints a time-stamped message to the log file (stderr).
 */

void rfbLog(char *format, ...)
{
    va_list args;
    char buf[256];
    time_t clock;

    va_start(args, format);

    time(&clock);
    strftime(buf, 255, "%d/%m/%y %T ", localtime(&clock));
    fprintf(stderr, buf);

    vfprintf(stderr, format, args);
    fflush(stderr);

    va_end(args);
}

void rfbLogPerror(char *str)
{
    rfbLog("");
    perror(str);
}
