#ifndef RESOURCE_H
#define RESOURCE_H

// ---------------------------------------------------------------------------
// Icon
// ---------------------------------------------------------------------------
#define IDI_APPLET        101

// ---------------------------------------------------------------------------
// String table IDs
// ---------------------------------------------------------------------------
#define IDS_NAME          102
#define IDS_INFO          103
#define IDS_ERR_REGREAD   104
#define IDS_ERR_REGWRITE  105
#define IDS_OK_APPLY      106
#define IDS_ABOUT_TITLE   107
#define IDS_ABOUT_TEXT    108

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------
#define IDD_DIALOG        200
#define IDD_ABOUTBOX      201

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------
#ifndef IDC_STATIC
#define IDC_STATIC        -1
#endif

// Touchpad features
#define IDC_MULTITOUCH    1001
#define IDC_TAPTOCLICK    1002
#define IDC_SCROLL        1003
#define IDC_SENSITIVITY   1004

// Controller features
#define IDC_BUSSPEED      1005
#define IDC_ARB_ENABLE    1006
#define IDC_ARB_BASE      1007
#define IDC_ARB_MAX       1008
#define IDC_ARB_JITTER    1009
#define IDC_WAKECAPABLE   1010
#define IDC_FORCECRASH    1011

// ---------------------------------------------------------------------------
// Menu / Accelerator
// ---------------------------------------------------------------------------
#define IDR_ACCELERATOR   300
#define IDR_MENU          301
#define IDM_ABOUT         400

// ---------------------------------------------------------------------------
// Registry path constants
// ---------------------------------------------------------------------------
// For HID-over-I²C miniport parameters
#define REG_PATH_I2CHID   L"SYSTEM\\CurrentControlSet\\Services\\i2chid\\Parameters"
// For I²C controller driver parameters
#define REG_PATH_I2CCTRL  L"SYSTEM\\CurrentControlSet\\Services\\i2cctrl\\Parameters"

// ---------------------------------------------------------------------------
// Global instance handle (declared in DllMain, used in CPlApplet)
// ---------------------------------------------------------------------------
extern HINSTANCE g_hInst;

#endif // RESOURCE_H
