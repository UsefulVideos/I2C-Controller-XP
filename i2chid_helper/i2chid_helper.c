#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WINVER 0x0501
#define _WIN32_WINNT 0x0501

#include <windows.h>
#include <strsafe.h>
#include <commctrl.h>   // for INITCOMMONCONTROLSEX, ICC_* constants, trackbar
#include <cpl.h>        // for CPL_INIT, CPL_GETCOUNT, CPL_INQUIRE, CPLINFO
#include "i2chid_helper_ioctl.h"
#include "resource.h"

// Global instance handle
HINSTANCE g_hInst;

static LONG OpenParamsKey(REGSAM sam, HKEY* phKey);
static BOOL WriteRegDword(LPCWSTR name, DWORD val);
static BOOL ValidateUint(HWND hDlg, int ctrlId, DWORD minVal, DWORD maxVal, DWORD* outVal);

/* ---------------------------------------------------------------------------
 * Device open/close
 * --------------------------------------------------------------------------- */
static HANDLE OpenI2cCtrlDevice(void)
{
    const WCHAR *path = L"\\\\.\\I2CCTRL";

    HANDLE h = CreateFileW(path,
                           GENERIC_READ | GENERIC_WRITE,
                           0,              /* exclusive */
                           NULL,           /* default security */
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    return h; /* INVALID_HANDLE_VALUE on error */
}

static void CloseI2cCtrlDevice(HANDLE h)
{
    if (h && h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
}

/* ---------------------------------------------------------------------------
 * Buffered IOCTL helper
 * --------------------------------------------------------------------------- */
static BOOL SendIoctlBuffered(
    HANDLE hDevice,
    DWORD  IoctlCode,
    const void *InputBuffer,
    DWORD  InputBufferSize,
    void  *OutputBuffer,
    DWORD  OutputBufferSize,
    DWORD *BytesReturned /* optional */
    )
{
    DWORD out = 0;
    BOOL ok;

    if (!hDevice || hDevice == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (InputBufferSize && !InputBuffer) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (OutputBufferSize && !OutputBuffer) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    ok = DeviceIoControl(hDevice,
                         IoctlCode,
                         (LPVOID)InputBuffer,
                         InputBufferSize,
                         OutputBuffer,
                         OutputBufferSize,
                         &out,
                         NULL);

    if (BytesReturned) {
        *BytesReturned = out;
    }
    return ok;
}

/* ---------------------------------------------------------------------------
 * Convenience wrappers
 * --------------------------------------------------------------------------- */
static BOOL SendIoctlInputOnly(HANDLE hDevice, DWORD IoctlCode,
                               const void *InputBuffer, DWORD InputBufferSize)
{
    return SendIoctlBuffered(hDevice, IoctlCode,
                             InputBuffer, InputBufferSize,
                             NULL, 0, NULL);
}

static BOOL SendIoctlOutputOnly(HANDLE hDevice, DWORD IoctlCode,
                                void *OutputBuffer, DWORD OutputBufferSize,
                                DWORD *BytesReturned)
{
    return SendIoctlBuffered(hDevice, IoctlCode,
                             NULL, 0,
                             OutputBuffer, OutputBufferSize,
                             BytesReturned);
}

/* ---------------------------------------------------------------------------
 * Example usage hook
 * --------------------------------------------------------------------------- */
static BOOL Example_QuerySomething(void *outBuf, DWORD outSize)
{
    HANDLE h;
    DWORD bytes;
    BOOL ok;
    const DWORD IOCTL_PLACEHOLDER = 0xFFFF0000;

    h = OpenI2cCtrlDevice();
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    bytes = 0;
    ok = SendIoctlOutputOnly(h, IOCTL_PLACEHOLDER, outBuf, outSize, &bytes);

    CloseI2cCtrlDevice(h);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Wrappers for controller IOCTLs
 * --------------------------------------------------------------------------- */
static BOOL I2cCtrl_PerformTransfer(const I2CCTRL_TRANSFER *xfer,
                                    DWORD xferSize,
                                    void *outBuf,
                                    DWORD outSize,
                                    DWORD *bytesReturned)
{
    HANDLE h;
    BOOL ok;

    h = OpenI2cCtrlDevice();
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    ok = SendIoctlBuffered(h,
                           IOCTL_I2CCTRL_TRANSFER,
                           xfer, xferSize,
                           outBuf, outSize,
                           bytesReturned);

    CloseI2cCtrlDevice(h);
    return ok;
}

static BOOL I2cCtrl_RunSelfTest(const I2CCTRL_SELFTEST *cfg,
                                DWORD cfgSize,
                                void *outBuf,
                                DWORD outSize,
                                DWORD *bytesReturned)
{
    HANDLE h;
    BOOL ok;

    h = OpenI2cCtrlDevice();
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    ok = SendIoctlBuffered(h,
                           IOCTL_I2CCTRL_SELFTEST,
                           cfg, cfgSize,
                           outBuf, outSize,
                           bytesReturned);

    CloseI2cCtrlDevice(h);
    return ok;
}

static BOOL I2cCtrl_ForceCrash(void)
{
    HANDLE h;
    BOOL ok;

    h = OpenI2cCtrlDevice();
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    ok = SendIoctlInputOnly(h,
                            IOCTL_I2C_FORCE_CRASH,
                            NULL, 0);

    CloseI2cCtrlDevice(h);
    return ok;
}

static INT_PTR CALLBACK HelperDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);


/* ---------------------------------------------------------------------------
 * Control Panel entry point
 * --------------------------------------------------------------------------- */
LONG CALLBACK CPlApplet(HWND hwndCPL, UINT uMsg, LPARAM lParam1, LPARAM lParam2)
{
    switch (uMsg) {
    case CPL_INIT: {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC  = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
        return TRUE;
    }
    case CPL_GETCOUNT:
        return 1;

    case CPL_INQUIRE: {
        CPLINFO* info = (CPLINFO*)lParam2;
        info->idIcon = IDI_APPLET;
        info->idName = IDS_NAME;
        info->idInfo = IDS_INFO;
        info->lData  = 0;
        return 0;
    }

    case CPL_DBLCLK:
        DialogBoxParamW(g_hInst,
                        MAKEINTRESOURCEW(IDD_DIALOG),
                        hwndCPL,
                        (DLGPROC)HelperDlgProc,
                        0);
        return 0;

    case CPL_EXIT:
        return 0;
    }
    return 0;
}

static DWORD ReadRegDword(LPCWSTR name, DWORD defVal)
{
    HKEY hKey;
    DWORD val;
    DWORD size;
    DWORD type;

    hKey = NULL;
    val = defVal;
    size = sizeof(DWORD);
    type = 0;

    if (OpenParamsKey(KEY_READ, &hKey) == ERROR_SUCCESS && hKey) {
        if (RegQueryValueExW(hKey, name, NULL, &type, (LPBYTE)&val, &size) != ERROR_SUCCESS || type != REG_DWORD) {
            val = defVal;
        }
        RegCloseKey(hKey);
    }
    return val;
}

static void UpdateArbControls(HWND hDlg)
{
    BOOL enabled;
    enabled = (IsDlgButtonChecked(hDlg, IDC_ARB_ENABLE) == BST_CHECKED);
    EnableWindow(GetDlgItem(hDlg, IDC_ARB_BASE), enabled);
    EnableWindow(GetDlgItem(hDlg, IDC_ARB_MAX), enabled);
    EnableWindow(GetDlgItem(hDlg, IDC_ARB_JITTER), enabled);
}

// ---------------------------------------------------------------------------
// Configuration load/save
// ---------------------------------------------------------------------------
static void LoadConfiguration(HWND hDlg)
{
    HWND hSpeed;
    DWORD sel;

    // Touchpad checkboxes
    CheckDlgButton(hDlg, IDC_MULTITOUCH,
                   ReadRegDword(L"EnableMultitouch", 1) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_TAPTOCLICK,
                   ReadRegDword(L"TapToClick", 1) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_SCROLL,
                   ReadRegDword(L"TwoFingerScroll", 1) ? BST_CHECKED : BST_UNCHECKED);

    // Sensitivity slider
    SendDlgItemMessageW(hDlg, IDC_SENSITIVITY, TBM_SETRANGE, TRUE, MAKELONG(1, 10));
    SendDlgItemMessageW(hDlg, IDC_SENSITIVITY, TBM_SETPOS, TRUE,
                        ReadRegDword(L"Sensitivity", 5));

    // Bus speed combo
    hSpeed = GetDlgItem(hDlg, IDC_BUSSPEED);
    SendMessageW(hSpeed, CB_RESETCONTENT, 0, 0);
    SendMessageW(hSpeed, CB_ADDSTRING, 0, (LPARAM)L"Standard (100 kHz)");
    SendMessageW(hSpeed, CB_ADDSTRING, 0, (LPARAM)L"Fast (400 kHz)");
    SendMessageW(hSpeed, CB_ADDSTRING, 0, (LPARAM)L"High-speed (3.4 MHz)");
    sel = ReadRegDword(L"BusSpeedMode", 1);
    if (sel > 2) sel = 1;
    SendMessageW(hSpeed, CB_SETCURSEL, sel, 0);

    // Arbitration
    CheckDlgButton(hDlg, IDC_ARB_ENABLE,
                   ReadRegDword(L"MultiMasterEnabled", 1) ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(hDlg, IDC_ARB_BASE,   ReadRegDword(L"ArbBackoffBaseUs",   100), FALSE);
    SetDlgItemInt(hDlg, IDC_ARB_MAX,    ReadRegDword(L"ArbBackoffMaxUs",   5000), FALSE);
    SetDlgItemInt(hDlg, IDC_ARB_JITTER, ReadRegDword(L"ArbBackoffJitterUs",  50), FALSE);
    UpdateArbControls(hDlg);

    // Wake capability
    CheckDlgButton(hDlg, IDC_WAKECAPABLE,
                   ReadRegDword(L"WakeCapable", 0) ? BST_CHECKED : BST_UNCHECKED);

    // Diagnostics
    CheckDlgButton(hDlg, IDC_FORCECRASH,
                   ReadRegDword(L"ForceCrashOnError", 0) ? BST_CHECKED : BST_UNCHECKED);
}

static void SaveConfiguration(HWND hDlg)
{
    DWORD sens;
    DWORD baseUs;
    DWORD maxUs;
    DWORD jitUs;

    // Sensitivity slider
    sens = (DWORD)SendDlgItemMessageW(hDlg, IDC_SENSITIVITY, TBM_GETPOS, 0, 0);

    // Touchpad checkboxes
    WriteRegDword(L"EnableMultitouch",
                  IsDlgButtonChecked(hDlg, IDC_MULTITOUCH) == BST_CHECKED);
    WriteRegDword(L"TapToClick",
                  IsDlgButtonChecked(hDlg, IDC_TAPTOCLICK) == BST_CHECKED);
    WriteRegDword(L"TwoFingerScroll",
                  IsDlgButtonChecked(hDlg, IDC_SCROLL) == BST_CHECKED);

    // Sensitivity slider
    WriteRegDword(L"Sensitivity", sens);

    // Bus speed
    WriteRegDword(L"BusSpeedMode",
                  (DWORD)SendDlgItemMessageW(hDlg, IDC_BUSSPEED, CB_GETCURSEL, 0, 0));

    // Arbitration checkbox
    WriteRegDword(L"MultiMasterEnabled",
                  IsDlgButtonChecked(hDlg, IDC_ARB_ENABLE) == BST_CHECKED);

    // Arbitration numeric fields (validated)
    baseUs = 100;
    maxUs  = 5000;
    jitUs  = 50;
    ValidateUint(hDlg, IDC_ARB_BASE,   1, 1000000, &baseUs);
    ValidateUint(hDlg, IDC_ARB_MAX,    1, 1000000, &maxUs);
    ValidateUint(hDlg, IDC_ARB_JITTER, 0,   10000, &jitUs);

    WriteRegDword(L"ArbBackoffBaseUs",   baseUs);
    WriteRegDword(L"ArbBackoffMaxUs",    maxUs);
    WriteRegDword(L"ArbBackoffJitterUs", jitUs);

    // Wake capability
    WriteRegDword(L"WakeCapable",
                  IsDlgButtonChecked(hDlg, IDC_WAKECAPABLE) == BST_CHECKED);

    // Diagnostics
    WriteRegDword(L"ForceCrashOnError",
                  IsDlgButtonChecked(hDlg, IDC_FORCECRASH) == BST_CHECKED);

    // Broadcast settings change
    SendMessageTimeoutW(HWND_BROADCAST,
                        WM_SETTINGCHANGE,
                        0,
                        (LPARAM)REG_PATH_I2CHID,
                        SMTO_ABORTIFHUNG,
                        5000,
                        NULL);
}


// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK HelperDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (message) {
    case WM_INITDIALOG:
        // Load registry values into dialog controls
        LoadConfiguration(hDlg);

        // Initialize touchpad sensitivity slider
        SendDlgItemMessageW(hDlg, IDC_SENSITIVITY, TBM_SETRANGE, TRUE, MAKELONG(1, 10));
        SendDlgItemMessageW(hDlg, IDC_SENSITIVITY, TBM_SETPOS, TRUE,
                            ReadRegDword(L"Sensitivity", 5));
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_ARB_ENABLE:
            UpdateArbControls(hDlg);
            return (INT_PTR)TRUE;

        case IDOK:
            // Save registry values from dialog controls
            SaveConfiguration(hDlg);

            // Touchpad settings: commit checkboxes and slider
            WriteRegDword(L"EnableMultitouch",
                          IsDlgButtonChecked(hDlg, IDC_MULTITOUCH) == BST_CHECKED);
            WriteRegDword(L"TapToClick",
                          IsDlgButtonChecked(hDlg, IDC_TAPTOCLICK) == BST_CHECKED);
            WriteRegDword(L"TwoFingerScroll",
                          IsDlgButtonChecked(hDlg, IDC_SCROLL) == BST_CHECKED);
            WriteRegDword(L"Sensitivity",
                          (DWORD)SendDlgItemMessageW(hDlg, IDC_SENSITIVITY, TBM_GETPOS, 0, 0));

            EndDialog(hDlg, IDOK);
            return (INT_PTR)TRUE;

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return (INT_PTR)TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return (INT_PTR)TRUE;
    }
    return (INT_PTR)FALSE;
}

// ---------------------------------------------------------------------------
// Registry helpers
// ---------------------------------------------------------------------------
static LONG OpenParamsKey(REGSAM sam, HKEY* phKey)
{
    if (!phKey) return ERROR_INVALID_PARAMETER;
    *phKey = NULL;
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_PATH_I2CHID, 0, sam, phKey);
}

static BOOL WriteRegDword(LPCWSTR name, DWORD val)
{
    HKEY hKey;
    LONG st;

    hKey = NULL;
    st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_PATH_I2CHID, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (st == ERROR_SUCCESS && hKey) {
        st = RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
        return (st == ERROR_SUCCESS);
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

static BOOL ValidateUint(HWND hDlg, int ctrlId, DWORD minVal, DWORD maxVal, DWORD* outVal)
{
    BOOL ok;
    UINT v;

    ok = FALSE;
    v = GetDlgItemInt(hDlg, ctrlId, &ok, FALSE);
    if (!ok) return FALSE;
    if (v < minVal) v = minVal;
    if (v > maxVal) v = maxVal;
    if (outVal) *outVal = v;
    return TRUE;
}

// ---------------------------------------------------------------------------
// DLL entry point
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(lpvReserved);

    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        // Save instance handle for later use (dialogs, resources)
        g_hInst = hinstDLL;

        // Disable thread attach/detach notifications to reduce overhead
        DisableThreadLibraryCalls(hinstDLL);
        break;

    case DLL_PROCESS_DETACH:
        // Nothing special to clean up
        break;
    }

    return TRUE;
}
