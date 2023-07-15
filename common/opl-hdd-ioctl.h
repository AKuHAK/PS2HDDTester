#ifndef OPL_HDD_IOCTL_H
#define OPL_HDD_IOCTL_H

// Commands and structures for XATAD.IRX
#define ATA_DEVCTL_IS_48BIT              0x6840
#define ATA_DEVCTL_SET_TRANSFER_MODE     0x6841
#define ATA_DEVCTL_READ_PARTITION_SECTOR 0x6842
#define ATA_DEVCTL_IDENTIFY              0x6843
#define ATA_DEVCTL_GET_CRC_ERROR_COUNT   0x6844
#define ATA_DEVCTL_RESET_CRC_ERROR_COUNT 0x6845
#define ATA_DEVCTL_START_TIMER           0x6846
#define ATA_DEVCTL_GET_TIMER             0x6847

#define ATA_DEVCTL_READ_BLOCK            0x6848

#define ATA_DEVCTL_FLUSH_CACHE           0x6849
#define ATA_DEVCTL_GET_ATA_ERROR         0x684A
#define ATA_DEVCTL_DEVICE_SMART_STATUS   0x684B
#define ATA_DEVCTL_SCE_IDENTIFY          0x684C
#define ATA_DEVCTL_IDLEIMM               0x684D

#define ATA_XFER_MODE_PIO                0x08
#define ATA_XFER_MODE_MDMA               0x20
#define ATA_XFER_MODE_UDMA               0x40

// structs for DEVCTL commands

typedef struct
{
    int type;
    int mode;
} hddAtaSetMode_t __attribute__((aligned(16)));

typedef struct
{
    int copy_to_ee;
    int block_size_in_sectors;
    int block_size_in_bytes;
    void *ee_buffer;
    void *iop_buffer;
} hddAtaReadTest_t __attribute__((aligned(16)));

typedef struct
{
    int status;
    int error;
} hddAtaError_t __attribute__((aligned(16)));

#endif
