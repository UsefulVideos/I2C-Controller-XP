/* i2cctrl_smbus.h - C89 compliant SMBus API */

#ifndef _I2cCtrl_SMBUS_H_
#define _I2cCtrl_SMBUS_H_

#include <ntddk.h>
#include "i2cctrl_hw.h"
#include "i2cctrl_ext.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional Packet Error Code (PEC) */
typedef enum _I2cCtrl_SMBUS_PEC {
    SmBusPecDisabled = 0,
    SmBusPecEnabled  = 1
} I2CCTRL_SMBUS_PEC;

/* Result structure */
typedef struct _I2cCtrl_SMBUS_RESULT {
    NTSTATUS Status;
    UCHAR    Data[32];
    UCHAR    Length;
} I2CCTRL_SMBUS_RESULT, *PI2CCTRL_SMBUS_RESULT;

/* Synchronous helpers */
NTSTATUS I2cCtrl_SmbusQuick(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, BOOLEAN ReadOperation);
NTSTATUS I2cCtrl_SmbusSendByte(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, UCHAR Command);
NTSTATUS I2cCtrl_SmbusReceiveByte(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, UCHAR *OutByte);
NTSTATUS I2cCtrl_SmbusWriteByte(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, UCHAR Command, UCHAR Value);
NTSTATUS I2cCtrl_SmbusReadByte(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, UCHAR Command, UCHAR *OutByte);
NTSTATUS I2cCtrl_SmbusWriteWord(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, UCHAR Command, USHORT Value);
NTSTATUS I2cCtrl_SmbusReadWord(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, UCHAR Command, USHORT *OutValue);
NTSTATUS I2cCtrl_SmbusBlockWrite(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, UCHAR Command, const UCHAR *Buffer, UCHAR Length, I2CCTRL_SMBUS_PEC PecMode);
NTSTATUS I2cCtrl_SmbusBlockRead(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, UCHAR Command, PI2CCTRL_SMBUS_RESULT Out, I2CCTRL_SMBUS_PEC PecMode);

/* Extensions */
NTSTATUS I2cCtrl_SmbusBlockProcessCall(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, UCHAR Command, const UCHAR *Buffer, UCHAR Length, PI2CCTRL_SMBUS_RESULT Out);
NTSTATUS I2cCtrl_SmbusAlertResponse(PI2CCTRL_FDO Dx, UCHAR *AlertingAddress);

#ifdef __cplusplus
}
#endif

#endif /* _I2cCtrl_SMBUS_H_ */
