/*
 * vita_runtime.c - PS Vita startup settings for VitaMail.
 */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>

#define VITA_DATA_DIR "ux0:data/VitaMail"

unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;
unsigned int sceLibcHeapSize = 4 * 1024 * 1024;
const unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
const int sceUserMainThreadPriority = 0x60;

static void ensure_data_dir(void)
{
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir(VITA_DATA_DIR, 0777);
	sceIoMkdir(VITA_DATA_DIR "/cache", 0777);
}

static void set_clocks(void)
{
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
}

void vita_runtime_init(void)
{
	set_clocks();
	ensure_data_dir();
	sceClibPrintf("VitaMail: heap=%u MiB stack=%u MiB\n",
	              _newlib_heap_size_user >> 20, sceUserMainThreadStackSize >> 20);
}
