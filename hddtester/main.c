
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <unistd.h>
#include <stdlib.h>
#include <debug.h>
#include <timer.h>
#include <libpad.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

#include "ata_identify.h"
#include "opl-hdd-ioctl.h"
#include "hddtests.h"

// Irx module buffers:
extern unsigned char iomanX_irx[];
extern unsigned int size_iomanX_irx;

extern unsigned char fileXio_irx[];
extern unsigned int size_fileXio_irx;

extern unsigned char ps2dev9_irx[];
extern unsigned int size_ps2dev9_irx;

extern unsigned char ps2atad_irx[];
extern unsigned int size_ps2atad_irx;

extern unsigned char xhdd_irx[];
extern unsigned int size_xhdd_irx;

static IDENTIFY_DEVICE_DATA ata_identify_data;
static char serialNumber[21] = {0};
static char modelNumber[41] = {0};
static int ata_identify_data_valid = 0;
static int device = 0;
static void *iop_io_buffer = NULL;

// Disable kernel patches and extra stuff we don't want.
DISABLE_PATCHED_FUNCTIONS();

// Remove libc glue that we don't need.
void _libcglue_timezone_update() {}
void _libcglue_init() {}
void _libcglue_deinit() {}

#define U64_2XU32(val) ((u32 *)val)[1], ((u32 *)val)[0]

// Exception handler functions:
void installExceptionHandlers(void);
void restoreExceptionHandlers();

// DMA buffer for pad input state:
static char pad_state[256] __attribute__((aligned(64)));
static int pad_buttons_raw = 0;
static int pad_buttons_current = 0;
static int pad_buttons_previous = 0;

int PollPadState(int port, int slot)
{
    struct padButtonStatus buttons;

    // Wait until the pad is ready.
    int state = padGetState(port, slot);
    while (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1 && state != PAD_STATE_DISCONN) {
        // Retry polling...
        state = padGetState(port, slot);
    }

    // Get pad input state.
    state = padRead(0, 0, &buttons);
    if (state != 0) {
        // Update button state.
        pad_buttons_raw = 0xFFFF ^ buttons.btns;
        pad_buttons_current = pad_buttons_raw & ~pad_buttons_previous;
        pad_buttons_previous = pad_buttons_raw;
    }

    return state;
}

// Menu state:
#define MAIN_MENU    0
#define SPEED_TEST_1 1
#define SPEED_TEST_2 2
#define SPEED_TEST_3 3
#define SPEED_TEST_4 4
#define SPEED_TEST_5 5
#define SPEED_TEST_6 6
#define SPEED_TEST_7 7

int menu_id = MAIN_MENU;
int menu_option_index = 0;

struct menu_option_info
{
    const char *option_text;
    int menu_id;
};

// Main menu options:
struct menu_option_info main_menu_options[] =
    {
        {"#1 Sequential raw read 64MB UDMA 4+ HDD->IOP", SPEED_TEST_1},
        {"#2 Sequential raw read 64MB UDMA 4+ HDD->IOP->EE", SPEED_TEST_2},
        {"#3 Random raw read 6MB UDMA 4+ HDD->IOP", SPEED_TEST_3},
        {"#4 Random raw read 6MB UDMA 4+ HDD->IOP->EE", SPEED_TEST_4},
        {"#5 Sequential raw read 16MB UDMA 0-4 HDD->IOP->EE", SPEED_TEST_5},
        {"#6 Random raw read 6MB UDMA 0-4 HDD->IOP->EE", SPEED_TEST_6},
        {"#7 Sequential raw read 64MB in 512kb blocks UDMA 0+ HDD->IOP", SPEED_TEST_7},
};
static int main_menu_option_count = sizeof(main_menu_options) / sizeof(struct menu_option_info);

struct iop_module_info
{
    const char *module_name;
    const u8 *module_buffer;
    u32 *module_size;
};

// List of embedded IOP modules to load:
struct iop_module_info iop_modules[] =
    {
        {"iomanx.irx", iomanX_irx, &size_iomanX_irx},
        {"fileXio.irx", fileXio_irx, &size_fileXio_irx},

        // HDD modules:
        {"ps2dev9.irx", ps2dev9_irx, &size_ps2dev9_irx},
        {"ps2atad.irx", ps2atad_irx, &size_ps2atad_irx},
        {"xhdd.irx", xhdd_irx, &size_xhdd_irx},
};
static int iop_modules_count = sizeof(iop_modules) / sizeof(struct iop_module_info);

void LoadIOPModules()
{
    int id;
    int ret;

    // Load required modules for pad input.
    ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    if (ret < 0) {
        _print("Failed to load SIO2MAN %d\n", ret);
        SleepThread();
    }

    ret = SifLoadModule("rom0:PADMAN", 0, NULL);
    if (ret < 0) {
        _print("Failed to load PADMAN %d\n", ret);
        SleepThread();
    }

    // Loop and load required IOP modules.
    for (int i = 0; i < iop_modules_count; i++) {
        _print("\t * Loading '%s' (%d)...\n", iop_modules[i].module_name, *iop_modules[i].module_size);
        if ((id = SifExecModuleBuffer((void *)iop_modules[i].module_buffer, *iop_modules[i].module_size, 0, NULL, &ret)) < 0 || ret != 0) {
            // Failing to load a required IOP module is a fatal error.
            _print("Failed to load '%s' %d (%d)\n", iop_modules[i].module_name, id, ret);
            SleepThread();
        }
    }
}

u32 GetHighestUDMAMode()
{
    // Check the highest UDMA mode supported.
    for (int i = 7; i >= 0; i--) {
        // Check if the current UDMA mode is supported.
        if ((ata_identify_data.UltraDMASupport & (1 << i)) != 0)
            return i;
    }

    return 0xFFFFFFFF;
}

u32 GetSelectedUDMAMode()
{
    // Check the highest UDMA mode selected.
    for (int i = 7; i >= 0; i--) {
        // Check if the current UDMA mode is selected.
        if ((ata_identify_data.UltraDMAActive & (1 << i)) != 0)
            return i;
    }

    return 0xFFFFFFFF;
}

u32 GetSectorSize()
{
    // Check if the physical to logical sector size data is valid.
    if (ata_identify_data.PhysicalLogicalSectorSize.Reserved1 == 1) {
        // Check if there are multiple logical sectors per physical sector.
        if (ata_identify_data.PhysicalLogicalSectorSize.LogicalSectorLongerThan256Words == 1) {
            u32 sectorSize = ((u32)ata_identify_data.WordsPerLogicalSector[0] << 16) | (u32)ata_identify_data.WordsPerLogicalSector[1];
            sectorSize *= 2;
            _print("WordsPerLogicalSector: 0x%08x\n", sectorSize);
            return sectorSize;
        } else {
            // Drive uses 512 byte sectors.
            return 512;
        }
    } else {
        // Assume 512 byte sector size?
        return 512;
    }
}

const char *SIZE_STRINGS[] =
    {
        "B",
        "KB",
        "MB",
        "GB",
        "TB",
        "PB"};

void GetHDDCapacity(u32 *pSize, u32 *pSizeStringIndex)
{
    *pSizeStringIndex = 0;

    // Get the capacity of the HDD in bytes.
    u64 hddCapacityInSectors = ((u64)ata_identify_data.Max48BitLBA[1] << 32) | (u64)ata_identify_data.Max48BitLBA[0];
    u32 sectorSize = GetSectorSize();
    u64 hddCapacityInBytes = hddCapacityInSectors * sectorSize;

    // Loop and find the highest denominator for capacity.
    while (hddCapacityInBytes > 1000) {
        hddCapacityInBytes /= 1000;
        *pSizeStringIndex = *pSizeStringIndex + 1;
    }

    // Set the final drive size in terms of units.
    *pSize = (u32)hddCapacityInBytes;
}

void ByteFlipString(u16 *pBuffer, int count)
{
    for (int i = 0; i < count; i++)
        pBuffer[i] = (u16)(((pBuffer[i] >> 8) & 0xFF) | ((pBuffer[i] << 8) & 0xFF00));
}

void PrintHDDInfo()
{
    char deviceString[7];
    sprintf(deviceString, "xhdd%d:", device);

    // Check if we need to query the data identify data.
    if (ata_identify_data_valid == 0) {
        // Run the ATA_IDENTIFY command to get device info.
        int ret = fileXioDevctl(deviceString, ATA_DEVCTL_IDENTIFY, NULL, 0, &ata_identify_data, sizeof(ata_identify_data));
        if (ret != 0) {
            // No HDD detected.
            scr_printf("No HDD %d detected!\n", device);
            SleepThread();
        }

        // Check for an incomplete reply.
        if (ata_identify_data.GeneralConfiguration.ResponseIncomplete != 0) {
            // Response is incomplete.
            scr_printf("Received incomplete response from HDD %d!\n", device);
            SleepThread();
        }

        // Byte flip the serial and model number strings.
        ByteFlipString((u16 *)&ata_identify_data.SerialNumber, sizeof(ata_identify_data.SerialNumber) / sizeof(u16));
        ByteFlipString((u16 *)&ata_identify_data.ModelNumber, sizeof(ata_identify_data.ModelNumber) / sizeof(u16));

        strncpy(serialNumber, ata_identify_data.SerialNumber, sizeof(ata_identify_data.SerialNumber));
        strncpy(modelNumber, ata_identify_data.ModelNumber, sizeof(ata_identify_data.ModelNumber));

        ata_identify_data_valid = 1;
    }

    // Print model and serial numbers:
    scr_printf("HDD %d detected: %s - %s\n", device, modelNumber, serialNumber);

    // Print command set/supported features info:
    u8 lba48Supported = ata_identify_data.CommandSetSupport.BigLba;
    scr_printf("\tLBA command set: %s\n", (lba48Supported == 0 ? "LBA28" : "LBA48"));

    u32 highestUDMAMode = GetHighestUDMAMode();
    u32 selectedUDMAMode = GetSelectedUDMAMode();

    if (highestUDMAMode != 0xFFFFFFFF) {
        if (selectedUDMAMode != 0xFFFFFFFF)
            scr_printf("\tUDMA mode: %d / %d\n", selectedUDMAMode, highestUDMAMode);
        else
            scr_printf("\tUDMA mode: ? / %d\n", highestUDMAMode);
    } else {
        if (selectedUDMAMode != 0xFFFFFFFF)
            scr_printf("\tUDMA mode: %d / ?\n", selectedUDMAMode);
        else
            scr_printf("\tUDMA mode: ? / ?\n");
    }


    // Print capacity and sector size:
    u32 hddCapacity, hddSizeStringIndex;
    GetHDDCapacity(&hddCapacity, &hddSizeStringIndex);
    scr_printf("\tDrive capacity: %d%s | Sector size: %dbytes\n", hddCapacity, (hddSizeStringIndex < 5 ? SIZE_STRINGS[hddSizeStringIndex] : " UNK"), GetSectorSize());
}

typedef struct
{
    u64 value;
} AlignedBlock __attribute__((aligned(16)));

void RunSequentialRawReadTest(u32 sizeInMb, u32 blockSizeInKb, int fullpass, int udmaStart, int udmaEnd)
{
    hddAtaError_t errorInfo = {0};
    hddAtaSetMode_t setMode;
    setMode.type = ATA_XFER_MODE_UDMA;
    char deviceString[7];
    sprintf(deviceString, "xhdd%d:", device);

    // Print the test banner.
    scr_printf("Sequential Raw Read %dMB block size %dKB HDD -> %s\n", sizeInMb, blockSizeInKb, (fullpass != 0 ? "EE" : "IOP"));

    // Get the highest UDMA mode to try.
    int highestUDMAMode = GetHighestUDMAMode();
    if (udmaEnd != -1)
        highestUDMAMode = udmaEnd;

    // Loop and try to perform the test on every UDMA mode requested.
    for (int i = udmaStart; i <= highestUDMAMode; i++) {
        AlignedBlock crcErrorCount;
        u64 elapsedTimeMsecEE;
        AlignedBlock elapsedTimeMsecIOP;

        // Set the UDMA mode.
        setMode.mode = i;
        fileXioDevctl(deviceString, ATA_DEVCTL_SET_TRANSFER_MODE, &setMode, sizeof(setMode), NULL, 0);

        // Refresh the identify data so we can get an accurate UDMA speed.
        fileXioDevctl(deviceString, ATA_DEVCTL_IDENTIFY, NULL, 0, &ata_identify_data, sizeof(ata_identify_data));
        u32 udmaModeUsed = GetSelectedUDMAMode();

        // Run the read test.
        int result = SequentialRawReadTest(device, sizeInMb, blockSizeInKb, iop_io_buffer, fullpass, &crcErrorCount.value, &elapsedTimeMsecEE, &elapsedTimeMsecIOP.value);

        // Calculate read stats.
        float timeInSecsEE = (float)((u32)elapsedTimeMsecEE) / 1000.0f;
        // float timeInSecsIOP = (float)elapsedTimeMsecIOP.value;
        u8 useTimeInMs = timeInSecsEE < 0.1f ? 1 : 0;
        float transferSpeed = (float)sizeInMb / timeInSecsEE;

        float timeValueEE = useTimeInMs == 1 ? (float)elapsedTimeMsecEE : timeInSecsEE;
        // float timeValueIOP = useTimeInMs == 1 ? (float)elapsedTimeMsecIOP.value : timeInSecsIOP;
        const char *timeUnits = useTimeInMs == 1 ? "ms" : "s";

        if (crcErrorCount.value > 0xFFFFFFFF)
            crcErrorCount.value = 0xFFFFFFFF;

        // Print the results.
        if (result == 0 || result == ATA_RES_ERR_ICRC) {
            scr_printf("\tUDMA %d: %d\tTime: %.2f%s - %.2fMB/%s\tCRC Errors: %d\tStatus: %s\n",
                       i, udmaModeUsed, timeValueEE, timeUnits, transferSpeed, timeUnits, (u32)crcErrorCount.value, (crcErrorCount.value == 0 ? "PASSED" : "FAILED"));
        } else {
            // Get extended ATA error info.
            fileXioDevctl(deviceString, ATA_DEVCTL_GET_ATA_ERROR, NULL, 0, &errorInfo, sizeof(errorInfo));
            if (result == ATA_RES_ERR_IO)
                scr_printf("\tIO Error: Status=0x%x Error=0x%x CRC Errors: %d\n", errorInfo.status, errorInfo.error, (u32)crcErrorCount.value);
            else if (result == ATA_RES_ERR_TIMEOUT)
                scr_printf("\tIO Timeout: Status=0x%x Error=0x%x CRC Errors: %d\n", errorInfo.status, errorInfo.error, (u32)crcErrorCount.value);
            else
                scr_printf("\tError %d Status=0x%x Error=0x%x CRC Errors: %d\n", result, errorInfo.status, errorInfo.error, (u32)crcErrorCount.value);
        }
    }
}

void RunRandomRawReadTest(u32 sizeInMb, u32 blockSizeInKb, int fullpass, int udmaStart, int udmaEnd)
{
    hddAtaError_t errorInfo = {0};
    hddAtaSetMode_t setMode;
    setMode.type = ATA_XFER_MODE_UDMA;
    char deviceString[7];
    sprintf(deviceString, "xhdd%d:", device);

    // Print the test banner.
    scr_printf("Random Raw Read %dMB block size %dKB HDD -> %s\n", sizeInMb, blockSizeInKb, (fullpass != 0 ? "EE" : "IOP"));

    // Get the highest UDMA mode to try.
    int highestUDMAMode = GetHighestUDMAMode();
    if (udmaEnd != -1)
        highestUDMAMode = udmaEnd;

    // Get the max LBA of the HDD.
    u64 hddCapacityInSectors = ((u64)ata_identify_data.Max48BitLBA[1] << 32) | (u64)ata_identify_data.Max48BitLBA[0];

    // Loop and try to perform the test on every UDMA mode requested.
    for (int i = udmaStart; i <= highestUDMAMode; i++) {
        AlignedBlock crcErrorCount;
        u64 elapsedTimeMsecEE;
        AlignedBlock elapsedTimeMsecIOP;

        // Set the UDMA mode.
        setMode.mode = i;
        fileXioDevctl(deviceString, ATA_DEVCTL_SET_TRANSFER_MODE, &setMode, sizeof(setMode), NULL, 0);

        // Refresh the identify data so we can get an accurate UDMA speed.
        fileXioDevctl(deviceString, ATA_DEVCTL_IDENTIFY, NULL, 0, &ata_identify_data, sizeof(ata_identify_data));
        u32 udmaModeUsed = GetSelectedUDMAMode();

        // Run the read test.
        int result = RandomRawReadTest(device, sizeInMb, blockSizeInKb, iop_io_buffer, fullpass, hddCapacityInSectors, &crcErrorCount.value, &elapsedTimeMsecEE, &elapsedTimeMsecIOP.value);

        // Calculate read stats.
        float timeInSecsEE = (float)((u32)elapsedTimeMsecEE) / 1000.0f;
        // float timeInSecsIOP = (float)elapsedTimeMsecIOP.value;
        u8 useTimeInMs = timeInSecsEE < 0.1f ? 1 : 0;
        float transferSpeed = (float)sizeInMb / timeInSecsEE;

        float timeValueEE = useTimeInMs == 1 ? (float)elapsedTimeMsecEE : timeInSecsEE;
        // float timeValueIOP = useTimeInMs == 1 ? (float)elapsedTimeMsecIOP.value : timeInSecsIOP;
        const char *timeUnits = useTimeInMs == 1 ? "ms" : "s";

        if (crcErrorCount.value > 0xFFFFFFFF)
            crcErrorCount.value = 0xFFFFFFFF;

        // Print the results.
        if (result == 0 || result == ATA_RES_ERR_ICRC) {
            scr_printf("\tUDMA %d: %d\tTime: %.2f%s - %.2fMB/%s\tCRC Errors: %d\tStatus: %s\n",
                       i, udmaModeUsed, timeValueEE, timeUnits, transferSpeed, timeUnits, (u32)crcErrorCount.value, (crcErrorCount.value == 0 ? "PASSED" : "FAILED"));
        } else {
            // Get extended ATA error info.
            fileXioDevctl(deviceString, ATA_DEVCTL_GET_ATA_ERROR, NULL, 0, &errorInfo, sizeof(errorInfo));
            if (result == ATA_RES_ERR_IO)
                scr_printf("\tIO Error: Status=0x%x Error=0x%x CRC Errors: %d\n", errorInfo.status, errorInfo.error, (u32)crcErrorCount.value);
            else if (result == ATA_RES_ERR_TIMEOUT)
                scr_printf("\tIO Timeout: Status=0x%x Error=0x%x CRC Errors: %d\n", errorInfo.status, errorInfo.error, (u32)crcErrorCount.value);
            else
                scr_printf("\tError %d Status=0x%x Error=0x%x CRC Errors: %d\n", result, errorInfo.status, errorInfo.error, (u32)crcErrorCount.value);
        }
    }
}

void TestEndCommon()
{
    // Print return message.
    scr_printf(" \n");
    scr_printf("Press triangle (^) to return to the main menu...\n");
    while (1) {
        // Update input.
        int ret = PollPadState(0, 0);
        if (ret != 0 && (pad_buttons_current & PAD_TRIANGLE) != 0)
            break;
    }

    // Switch back to the main menu.
    menu_id = MAIN_MENU;
    menu_option_index = 0;
}

int main(int argc, char *argv[])
{
    int ret;

    // Patch the kernel to re-enable uart.
    InitDebug();

    // Install exception handler hooks.
    installExceptionHandlers();

    // Initialize the timer system.
    StartTimerSystemTime();

    // Initialize screen printf.
    _print(" * Initializing screen output...\n");
    init_scr();

    // Reboot the IOP and clear memory.
    _print(" * Rebooting IOP...\n");
    while (!SifIopReset("", 0))
        ;
    while (!SifIopSync())
        ;

    // Initialize RPC layer and patch modload to let us load IOP modules from memory.
    _print(" * Initializing sif rpc...\n");
    SifInitRpc(0);
    SifInitIopHeap();
    SifLoadFileInit();
    sbv_patch_enable_lmb();

    // Load required IOP modules.
    _print(" * Loading IOP modules...\n");
    LoadIOPModules();
    fileXioInit();

    // Initialize input.
    padInit(0);
    if ((ret = padPortOpen(0, 0, pad_state)) == 0) {
        // Failed to open pad port.
        _print("Failed to open pad port 0: %d\n", ret);
        SleepThread();
    }

    // Allocate a buffer on the IOP side for hdd IO operations.
    iop_io_buffer = SifAllocIopHeap(1024 * 512);
    if (iop_io_buffer == NULL) {
        // Failed to allocate IOP memory.
        scr_printf("ERROR: Failed to allocate IOP memory\n");
        SleepThread();
    }

    // Main loop:
    while (1) {
        // Clear the screen.
        scr_clear();

        // Print the banner message.
        scr_printf(" \n");
        scr_printf("HDD Tester v1.1 dual\n\n");

        // Check the menu id and handle accordingly.
        switch (menu_id) {
            case MAIN_MENU: {
                // Print HDD information.
                PrintHDDInfo();
                scr_printf(" \n");
                scr_printf(" \n");

                // Print hdd test options:
                scr_printf("Select a test to run on HDD %d:\n", device);
                for (int i = 0; i < main_menu_option_count; i++) {
                    // Check if this is the currently selected option and handle accordingly.
                    if (menu_option_index == i)
                        scr_printf("\t---> %s\n", main_menu_options[i].option_text);
                    else
                        scr_printf("\t     %s\n", main_menu_options[i].option_text);
                }
                scr_printf(" \n");
                scr_printf(" \n");
                scr_printf(" \n");

                // Print help text.
                scr_printf("Press UP/DOWN on DPAD to change options\n");
                scr_printf("Press SQUARE button to switch hdd for test\n");
                scr_printf("Press CROSS (X) button to run test\n");
                scr_printf("Press START + SELECT to exit\n");

                // Handle input.
                int pollInput = 1;
                while (pollInput != 0) {
                    // Update input state and handle accordingly.
                    ret = PollPadState(0, 0);
                    if (ret != 0) {
                        pollInput = 0;

                        if ((pad_buttons_current & PAD_UP) != 0)
                            menu_option_index = menu_option_index - 1 >= 0 ? menu_option_index - 1 : main_menu_option_count - 1;
                        else if ((pad_buttons_current & PAD_DOWN) != 0)
                            menu_option_index = menu_option_index + 1 < main_menu_option_count ? menu_option_index + 1 : 0;
                        else if ((pad_buttons_current & PAD_SQUARE) != 0) {
                            device = (device ^ 1) & 1;
                            ata_identify_data_valid = 0;
                            _print("switch HDD %d to %d", (device ^ 1) & 1, device);
                        } else if ((pad_buttons_current & PAD_CROSS) != 0)
                            menu_id = main_menu_options[menu_option_index].menu_id;
                        else if ((pad_buttons_raw & PAD_START) != 0 && (pad_buttons_raw & PAD_SELECT) != 0) {
                            // Restore exception handlers and exit.
                            restoreExceptionHandlers();
                            Exit(0);
                        } else
                            pollInput = 1;
                    }
                }
                break;
            }
            case SPEED_TEST_1:
            case SPEED_TEST_2: {
                // Sequential raw read 64MB UDMA 4+
                int fullPass = menu_id == SPEED_TEST_2 ? 1 : 0;
                RunSequentialRawReadTest(64, 2, fullPass, 4, -1);
                RunSequentialRawReadTest(64, 4, fullPass, 4, -1);
                RunSequentialRawReadTest(64, 16, fullPass, 4, -1);
                RunSequentialRawReadTest(64, 32, fullPass, 4, -1);

                // Print return message and handle input.
                TestEndCommon();
                break;
            }
            case SPEED_TEST_3:
            case SPEED_TEST_4: {
                // Random raw read 6MB UDMA 4+
                int fullPass = menu_id == SPEED_TEST_4 ? 1 : 0;
                RunRandomRawReadTest(6, 2, fullPass, 4, -1);
                RunRandomRawReadTest(6, 4, fullPass, 4, -1);
                RunRandomRawReadTest(6, 16, fullPass, 4, -1);
                RunRandomRawReadTest(6, 32, fullPass, 4, -1);

                // Print return message and handle input.
                TestEndCommon();
                break;
            }
            case SPEED_TEST_5: {
                // Sequential raw read 16MB UDMA 0-4 HDD->IOP->EE
                RunSequentialRawReadTest(16, 2, 1, 0, 4);
                RunSequentialRawReadTest(16, 4, 1, 0, 4);
                RunSequentialRawReadTest(16, 16, 1, 0, 4);
                RunSequentialRawReadTest(16, 32, 1, 0, 4);
                TestEndCommon();
                break;
            }
            case SPEED_TEST_6: {
                // Random raw read 6MB UDMA 0-4 HDD->IOP->EE
                RunRandomRawReadTest(6, 2, 1, 0, 4);
                RunRandomRawReadTest(6, 4, 1, 0, 4);
                RunRandomRawReadTest(6, 16, 1, 0, 4);
                RunRandomRawReadTest(6, 32, 1, 0, 4);
                TestEndCommon();
                break;
            }
            case SPEED_TEST_7: {
                // Sequential raw read 64MB in 512kb blocks UDMA 0+ HDD->IOP
                RunSequentialRawReadTest(64, 512, 0, 0, -1);
                TestEndCommon();
                break;
            }
        }
    }

    // We should never get here, but just in case.
    restoreExceptionHandlers();
    return 0;
}
