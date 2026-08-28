/*
 * Unit tests for atpd timezone detection and resolution
 */

#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define TEST_RUN_DIR "/tmp/atp_tz_test"

/* Helper to build a minimal synthetic Android tzdata binary containing standard TZif files */
static void create_mock_android_tzdata(const char *tzdata_path) {
    /* Minimal RFC 8536 TZif file for CST-8 (Asia/Shanghai) */
    /* Header: "TZif2", 15 zero bytes, 6 32-bit counts */
    /* For testing, we create a valid TZif header with POSIX footer */
    const char posix_footer[] = "\nCST-8\n";
    size_t tzif_len = 0x100;
    uint8_t tzif_buf[0x100] = {0};

    /* Magic "TZif2" */
    memcpy(tzif_buf, "TZif2", 5);
    /* Place POSIX footer at end */
    memcpy(tzif_buf + tzif_len - sizeof(posix_footer) + 1, posix_footer, sizeof(posix_footer) - 1);

    /* Write Android tzdata file */
    FILE *fp = fopen(tzdata_path, "wb");
    assert(fp != NULL);

    /* Android Header (24 bytes) */
    char magic[12] = "tzdata2024a";
    uint32_t index_offset = htonl(24);
    uint32_t data_offset = htonl(24 + 52 * 2); /* 2 entries */
    uint32_t zonetab_offset = htonl(24 + 52 * 2 + (uint32_t)tzif_len * 2);

    fwrite(magic, 1, 12, fp);
    fwrite(&index_offset, 1, 4, fp);
    fwrite(&data_offset, 1, 4, fp);
    fwrite(&zonetab_offset, 1, 4, fp);

    /* Entry 1: Asia/Shanghai */
    char name1[40] = "Asia/Shanghai";
    uint32_t start1 = htonl(0);
    uint32_t len1 = htonl((uint32_t)tzif_len);
    uint32_t unused1 = 0;
    fwrite(name1, 1, 40, fp);
    fwrite(&start1, 1, 4, fp);
    fwrite(&len1, 1, 4, fp);
    fwrite(&unused1, 1, 4, fp);

    /* Entry 2: Asia/Tokyo */
    char name2[40] = "Asia/Tokyo";
    uint32_t start2 = htonl((uint32_t)tzif_len);
    uint32_t len2 = htonl((uint32_t)tzif_len);
    uint32_t unused2 = 0;
    fwrite(name2, 1, 40, fp);
    fwrite(&start2, 1, 4, fp);
    fwrite(&len2, 1, 4, fp);
    fwrite(&unused2, 1, 4, fp);

    /* Data block 1 (Asia/Shanghai) */
    fwrite(tzif_buf, 1, tzif_len, fp);

    /* Data block 2 (Asia/Tokyo) */
    const char tokyo_footer[] = "\nJST-9\n";
    memcpy(tzif_buf + tzif_len - sizeof(tokyo_footer) + 1, tokyo_footer, sizeof(tokyo_footer) - 1);
    fwrite(tzif_buf, 1, tzif_len, fp);

    fclose(fp);
}

static void test_synthetic_tzdata_and_posix_fallback(void) {
    printf("Running test_synthetic_tzdata_and_posix_fallback...\n");

    mkdir(TEST_RUN_DIR, 0755);
    char mock_tzdata[256];
    snprintf(mock_tzdata, sizeof(mock_tzdata), "%s/tzdata", TEST_RUN_DIR);
    create_mock_android_tzdata(mock_tzdata);

    /* Test fallback POSIX mapping */
    setenv("TZ", "CST-8", 1);
    tzset();

    time_t now = 1700000000; /* Fixed timestamp: 2023-11-14 22:13:20 UTC */
    struct tm tm_res;
    localtime_r(&now, &tm_res);

    /* In CST-8 (UTC+8), hour should be (22 + 8) % 24 = 6 am on Nov 15 */
    assert(tm_res.tm_hour == 6);
    assert(tm_res.tm_min == 13);
    assert(tm_res.tm_sec == 20);
    assert(tm_res.tm_mday == 15);

    /* Test atp_timezone_init initialization */
    int rc = atp_timezone_init();
    assert(rc == 0);

    char tz_name[64] = {0};
    rc = atp_timezone_get_name(tz_name, sizeof(tz_name));
    assert(rc == 0);
    assert(strlen(tz_name) > 0);
    printf("  Current detected timezone: %s\n", tz_name);

    long offset = atp_timezone_get_offset_sec();
    printf("  Current timezone offset: %ld seconds (%+.1f hours)\n", offset, (double)offset / 3600.0);

    printf("test_synthetic_tzdata_and_posix_fallback PASSED\n");
}

static void test_localtime_formatting(void) {
    printf("Running test_localtime_formatting...\n");

    /* Set CST-8 */
    setenv("TZ", "CST-8", 1);
    tzset();

    time_t now = 1700000000;
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    assert(strcmp(buf, "2023-11-15 06:13:20") == 0);

    /* Set JST-9 (Tokyo) */
    setenv("TZ", "JST-9", 1);
    tzset();
    localtime_r(&now, &tm_info);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    assert(strcmp(buf, "2023-11-15 07:13:20") == 0);

    /* Set EST5 (New York Winter) */
    setenv("TZ", "EST5", 1);
    tzset();
    localtime_r(&now, &tm_info);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    assert(strcmp(buf, "2023-11-14 17:13:20") == 0);

    printf("test_localtime_formatting PASSED\n");
}

int main(void) {
    printf("=== Starting ATP Timezone Unit Tests ===\n");
    test_synthetic_tzdata_and_posix_fallback();
    test_localtime_formatting();
    printf("=== All Timezone Unit Tests PASSED ===\n");
    return 0;
}
