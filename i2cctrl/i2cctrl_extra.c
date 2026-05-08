#include "i2cctrl_ext.h"
#include "i2cctrl_extra.h"

ULONG
I2cCtrl_GetTransferBytesWritten(
    I2C_TRANSFER_CONTEXT *xc
    )
{
    if (xc == NULL) {
        return 0U;
    }
    if (xc->Direction == I2cDirRead) {
        return xc->RxIndex;
    } else {
        return xc->TxIndex;
    }
}

ULONG
I2cCtrl_GetSequenceBytesWritten(
    I2C_TRANSFER_CONTEXT *xc
    )
{
    if (xc == NULL) {
        return 0U;
    }
    /* If your context doesn’t have SeqIndex, use RxIndex/TxIndex */
    if (xc->Direction == I2cDirRead) {
        return xc->RxIndex;
    } else {
        return xc->TxIndex;
    }
}

