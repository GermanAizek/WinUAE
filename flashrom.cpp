/*
* UAE - The Un*x Amiga Emulator
*
* Simple 29F010 flash ROM chip emulator
*
* (c) 2014 Toni Wilen
*/

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "zfile.h"
#include "flashrom.h"
#include "memory.h"
#include "newcpu.h"
#include "debug.h"

#define FLASH_LOG 0

struct flashrom_data
{
	uae_u8 *rom;
	int flashsize;
	int allocsize;
	int mask;
	int state;
	int modified;
	int sectorsize;
	uae_u8 devicecode;
	struct zfile *zf;
};

void *flash_new(uae_u8 *rom, int flashsize, int allocsize, uae_u8 devicecode, struct zfile *zf)
{
	struct flashrom_data *fd = xcalloc(struct flashrom_data, 1);
	fd->flashsize = flashsize;
	fd->allocsize = allocsize;
	fd->mask = fd->flashsize - 1;
	fd->zf = zf;
	fd->rom = rom;
	fd->devicecode = devicecode;
	fd->sectorsize = devicecode == 0x20 ? 16384 : 65536;
	return fd;
}

void flash_free(void *fdv)
{
	struct flashrom_data *fd = (struct flashrom_data*)fdv;
	if (!fd)
		return;
	if (fd->zf && fd->modified) {
		zfile_fseek(fd->zf, 0, SEEK_SET);
		zfile_fwrite(fd->rom, fd->allocsize, 1, fd->zf);
	}
	xfree(fdv);
}

int flash_size(void *fdv)
{
	struct flashrom_data *fd = (struct flashrom_data*)fdv;
	if (!fd)
		return 0;
	return fd->flashsize;
}

bool flash_active(void *fdv, uaecptr addr)
{
	struct flashrom_data *fd = (struct flashrom_data*)fdv;
	if (!fd)
		return false;
	return fd->state != 0;
}

bool flash_write(void *fdv, uaecptr addr, uae_u8 v)
{
	struct flashrom_data *fd = (struct flashrom_data*)fdv;
	int oldstate;
	uae_u32 addr2;

	if (!fd)
		return false;
	oldstate = fd->state;
#if FLASH_LOG > 1
	write_log(_T("flash write %08x %02x (%d) PC=%08x\n"), addr, v, fd->state, m68k_getpc());
#endif

	addr &= fd->mask;
	addr2 = addr & 0xffff;

	if (fd->state == 7) {
		fd->state = 100;
		if (addr >= fd->allocsize)
			return false;
		if (fd->rom[addr] != v)
			fd->modified = 1;
		fd->rom[addr] = v;
		return true;
	}

	if (v == 0xf0) {
		fd->state = 0;
		return false;
	}

	// unlock
	if (addr2 == 0x5555 && fd->state <= 2 && v == 0xaa)
		fd->state = 1;
	if (addr2 == 0x2aaa && fd->state == 1 && v == 0x55)
		fd->state = 2;

	// autoselect
	if (addr2 == 0x5555 && fd->state == 2 && v == 0x90)
		fd->state = 3;

	// program
	if (addr2 == 0x5555 && fd->state == 2 && v == 0xa0)
		fd->state = 7;

	// chip/sector erase
	if (addr2 == 0x5555 && fd->state == 2 && v == 0x80)
		fd->state = 4;
	if (addr2 == 0x5555 && fd->state == 4 && v == 0xaa)
		fd->state = 5;
	if (addr2 == 0x2aaa && fd->state == 5 && v == 0x55)
		fd->state = 6;
	if (addr2 == 0x5555 && fd->state == 6 && v == 0x10) {
		memset(fd->rom, 0xff, fd->allocsize);
		fd->state = 200;
		fd->modified = 1;
#if FLASH_LOG
		write_log(_T("flash chip erased\n"), addr);
#endif
		return true;
	} else if (fd->state == 6 && v == 0x30) {
		int saddr = addr & ~(fd->sectorsize - 1);
		if (saddr < fd->allocsize)
			memset(fd->rom + saddr, 0xff, fd->sectorsize);
		fd->state = 200;
		fd->modified = 1;
#if FLASH_LOG
		write_log(_T("flash sector %d erased (%08x)\n"), saddr / fd->sectorsize, addr);
#endif
		return true;
	}

	if (fd->state == oldstate)
		fd->state = 0;
	return false;
}

uae_u32 flash_read(void *fdv, uaecptr addr)
{
	struct flashrom_data *fd = (struct flashrom_data*)fdv;
	uae_u8 v = 0xff;

	if (!fd)
		return 0;

	addr &= fd->mask;
	if (fd->state == 3) {
		uae_u8 a = addr & 0xff;
		if (a == 0)
			v = 0x01;
		if (a == 1)
			v = fd->devicecode;
		if (a == 2)
			v = 0x00;
	} else if (fd->state >= 200) {
		v = 0;
		if (fd->state & 1)
			v ^= 0x40;
		fd->state++;
		if (fd->state >= 210)
			fd->state = 0;
		v |= 0x08;
	} else if (fd->state >= 100) {
		v = (fd->rom[addr] & 0x80) ^ 0x80;
		if (fd->state & 1)
			v ^= 0x40;
		fd->state++;
		if (fd->state >= 110)
			fd->state = 0;
	} else {
		fd->state = 0;
		if (addr >= fd->allocsize)
			v = 0xff;
		else
			v = fd->rom[addr];
	}
#if FLASH_LOG > 1
	write_log(_T("flash read %08x = %02X (%d) PC=%08x\n"), addr, v, fd->state, m68k_getpc());
#endif

	return v;
}
