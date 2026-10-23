/* Host harness for muse_msc_fat.c: geometry, 8.3 naming, INDEX.TXT and a
 * full FAT-chain reconstruction of every file from raw sector reads. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "muse_msc_fat.h"

static uint8_t s_files[3][9000];
static size_t s_lens[3];
static size_t mem_read(void *ctx, int idx, uint32_t off, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (off >= s_lens[idx]) return 0;
    size_t n = s_lens[idx] - off;
    if (n > len) n = len;
    memcpy(buf, s_files[idx] + off, n);
    return n;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int main(void)
{
    char name[16];
    muse_msc_trip_name(5, name); assert(strcmp(name, "TRIP0005.CSV") == 0);
    muse_msc_trip_name(1234567, name); assert(strcmp(name, "TRIP1234567.CSV") == 0);
    muse_msc_file_t trips[2] = { {5, 5000, 42, 0xDEADBEEF}, {12, 100, 3, 0x12345678} };
    char index[512];
    size_t ilen = muse_msc_index_build(index, sizeof(index), trips, 2, "abc1234");
    assert(ilen > 0);
    assert(strcmp(index, "x") && strncmp(index, "Muse trip logs (firmware abc1234)\n", strlen("Muse trip logs (firmware abc1234)\n")) == 0);
    assert(strstr(index, "TRIP0005.CSV id=5 bytes=5000 samples=42 crc32=deadbeef\n"));
    assert(strstr(index, "TRIP0012.CSV id=12 bytes=100 samples=3 crc32=12345678\n"));
    assert(muse_msc_index_build(index, 10, trips, 2, "x") == 0); /* too small: no partial */

    for (int i = 0; i < 3; i++) for (size_t k = 0; k < sizeof(s_files[i]); k++) s_files[i][k] = (uint8_t)(i * 31 + k);
    memcpy(s_files[0], index, ilen); s_lens[0] = ilen; s_lens[1] = 5000; s_lens[2] = 100;

    muse_msc_snapshot_t snap;
    assert(muse_msc_snapshot_init(&snap, trips, 2, (uint32_t)ilen));
    assert(snap.count == 3 && snap.total_sectors > 0);
    uint8_t sec[512];
    assert(muse_msc_read_sector(&snap, 0, sec, mem_read, NULL));
    assert(sec[510] == 0x55 && sec[511] == 0xAA && rd16(sec + 11) == 512 && sec[13] == 8);
    assert(memcmp(sec + 43, "MUSE LOGS  ", 11) == 0 && memcmp(sec + 54, "FAT16   ", 8) == 0);
    assert(!muse_msc_read_sector(&snap, snap.total_sectors, sec, mem_read, NULL));

    /* Reconstruct each file through dir entry + FAT chain. */
    uint32_t root = 1 + 2 * snap.fat_sectors;
    for (int f = 0; f < snap.count; f++) {
        uint8_t dir[512];
        assert(muse_msc_read_sector(&snap, root, dir, mem_read, NULL));
        const uint8_t *d = dir + (f + 1) * 32;
        uint32_t size = rd32(d + 28), cl = rd16(d + 26);
        assert(size == s_lens[f]);
        size_t got = 0;
        while (cl >= 2 && cl < 0xFFF8 && got < size) {
            for (uint32_t s = 0; s < MUSE_MSC_CLUSTER_SECTORS && got < size; s++) {
                uint32_t lba = root + MUSE_MSC_ROOT_SECTORS + (cl - 2) * MUSE_MSC_CLUSTER_SECTORS + s;
                uint8_t data[512];
                assert(muse_msc_read_sector(&snap, lba, data, mem_read, NULL));
                size_t n = size - got; if (n > 512) n = 512;
                assert(memcmp(data, s_files[f] + got, n) == 0);
                got += n;
            }
            uint8_t fatsec[512];
            assert(muse_msc_read_sector(&snap, 1 + (cl * 2) / 512, fatsec, mem_read, NULL));
            cl = rd16(fatsec + (cl * 2) % 512);
        }
        assert(got == size);
    }
    printf("msc_fat harness PASS sectors=%lu fat=%lu\n", (unsigned long)snap.total_sectors, (unsigned long)snap.fat_sectors);
    return 0;
}
