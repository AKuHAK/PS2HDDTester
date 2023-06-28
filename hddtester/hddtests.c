
#include "hddtests.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <unistd.h>
#include <stdlib.h>
#include <timer.h>
#include <debug.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

#include "ata_identify.h"
#include "opl-hdd-ioctl.h"

#define U64_2XU32(val) ((u32 *)val)[1], ((u32 *)val)[0]

int SequentialRawReadTest(int mbToRead, int blockSizeKb, void *pIopBuffer, int fullPass, u64 *pCrcErrorCount, u64 *pElapsedTimeEE, u64 *pElapsedTimeIOP)
{
    hddAtaReadTest_t testInfo;
    int result = 0;

    // Open the hdd for raw read access.
    int fd = fileXioOpen("xhdd0:", FIO_O_RDONLY);
    if (fd < 0) {
        // Failed to open hdd.
        return -HDD_ERR_FAILED_TO_OPEN;
    }

    // Allocate some scratch memory for read operations.
    u8 *pBuffer = (u8 *)malloc(blockSizeKb * 1024);
    if (pBuffer == NULL) {
        // Failed to allocate scratch buffer.
        return -HDD_ERR_OUT_OF_MEMORY;
    }

    // Calculate the number of sectors to read and reset the CRC error count on the drive.
    int blockCount = (mbToRead * ONE_MB_IN_BYTES) / (blockSizeKb * 1024);
    fileXioDevctl("xhdd0:", ATA_DEVCTL_RESET_CRC_ERROR_COUNT, NULL, 0, NULL, 0);
    fileXioDevctl("xhdd0:", ATA_DEVCTL_FLUSH_CACHE, NULL, 0, NULL, 0);
    // fileXioDevctl("xhdd0:", ATA_DEVCTL_START_TIMER, NULL, 0, NULL, 0);

    // Setup the transfer info request.
    testInfo.copy_to_ee = fullPass;
    testInfo.block_size_in_bytes = blockSizeKb * 1024;
    testInfo.block_size_in_sectors = testInfo.block_size_in_bytes / 512;
    testInfo.ee_buffer = pBuffer;
    testInfo.iop_buffer = pIopBuffer;

    // Get the start time.
    clock_t startTime = clock();

    // Loop and read the specified amount of data from the HDD.
    for (int i = 0; i < blockCount; i++) {
        if ((result = fileXioDevctl("xhdd0:", ATA_DEVCTL_READ_BLOCK, &testInfo, sizeof(testInfo), NULL, 0)) < 0) {
            // Abort for anything other than CRC errors.
            if (result != ATA_RES_ERR_ICRC)
                break;
        }
    }

    // Get the stop time and calculate how long the reads took.
    clock_t elapsedTime = clock() - startTime;
    *pElapsedTimeEE = elapsedTime / (CLOCKS_PER_SEC / 1000);

    // Get the IO time from the IOP side.
    // fileXioDevctl("xhdd0:", ATA_DEVCTL_GET_TIMER, NULL, 0, pElapsedTimeIOP, sizeof(u64));
    //_print("pElapsedTimeIOP: 0x%08x%08x\n", U64_2XU32(pElapsedTimeIOP));

    // Get the number of CRC errors.
    fileXioDevctl("xhdd0:", ATA_DEVCTL_GET_CRC_ERROR_COUNT, NULL, 0, pCrcErrorCount, sizeof(u64));

    // Close the hdd and free scratch memory.
    fileXioClose(fd);
    free(pBuffer);

    return result;
}

int RandomRawReadTest(int mbToRead, int blockSizeKb, void *pIopBuffer, int fullPass, u64 hddMaxLBA, u64 *pCrcErrorCount, u64 *pElapsedTimeEE, u64 *pElapsedTimeIOP)
{
    hddAtaReadTest_t testInfo;
    u32 sectorsInOneGB = (1024 * ONE_MB_IN_BYTES) / HDD_SECTOR_SIZE;
    int result = 0;

    // Open the hdd for raw read access.
    int fd = fileXioOpen("xhdd0:", FIO_O_RDONLY);
    if (fd < 0) {
        // Failed to open hdd.
        return -HDD_ERR_FAILED_TO_OPEN;
    }

    // Allocate some scratch memory for read operations.
    u8 *pBuffer = (u8 *)malloc(blockSizeKb * 1024);
    if (pBuffer == NULL) {
        // Failed to allocate scratch buffer.
        return -HDD_ERR_OUT_OF_MEMORY;
    }

    // Calculate the number of sectors to read and reset the CRC error count on the drive.
    int blockCount = (mbToRead * ONE_MB_IN_BYTES) / (blockSizeKb * 1024);
    fileXioDevctl("xhdd0:", ATA_DEVCTL_RESET_CRC_ERROR_COUNT, NULL, 0, NULL, 0);
    fileXioDevctl("xhdd0:", ATA_DEVCTL_FLUSH_CACHE, NULL, 0, NULL, 0);
    // fileXioDevctl("xhdd0:", ATA_DEVCTL_START_TIMER, NULL, 0, NULL, 0);

    // Setup the transfer info request.
    testInfo.copy_to_ee = fullPass;
    testInfo.block_size_in_bytes = blockSizeKb * 1024;
    testInfo.block_size_in_sectors = testInfo.block_size_in_bytes / 512;
    testInfo.ee_buffer = pBuffer;
    testInfo.iop_buffer = pIopBuffer;

    // Seed RGN using the current time?
    srand((u32)clock());

    // Get the start time.
    clock_t startTime = clock();

    // Loop and read the specified amount of data from the HDD.
    u64 lba_pos = 0;
    for (int i = 0; i < blockCount; i++) {
        // Randomly seek in max 1GB increments, with a 10% chance to move backwards.
        if ((rand() % 100) > 90)
            lba_pos -= rand() % sectorsInOneGB;
        else
            lba_pos += rand() % sectorsInOneGB;

        if (lba_pos > hddMaxLBA)
            lba_pos = 0;

        // Read the next block from the HDD.
        fileXioLseek64(fd, lba_pos, FIO_SEEK_SET);
        if ((result = fileXioDevctl("xhdd0:", ATA_DEVCTL_READ_BLOCK, &testInfo, sizeof(testInfo), NULL, 0)) < 0) {
            // Abort for anything other than CRC errors.
            if (result != ATA_RES_ERR_ICRC)
                break;
        }
    }

    // Get the stop time and calculate how long the reads took.
    clock_t elapsedTime = clock() - startTime;
    *pElapsedTimeEE = elapsedTime / (CLOCKS_PER_SEC / 1000);

    // Get the IO time from the IOP side.
    // fileXioDevctl("xhdd0:", ATA_DEVCTL_GET_TIMER, NULL, 0, pElapsedTimeIOP, sizeof(u64));

    // Get the number of CRC errors.
    fileXioDevctl("xhdd0:", ATA_DEVCTL_GET_CRC_ERROR_COUNT, NULL, 0, pCrcErrorCount, sizeof(u64));

    // Close the hdd and free scratch memory.
    fileXioClose(fd);
    free(pBuffer);

    return result;
}
