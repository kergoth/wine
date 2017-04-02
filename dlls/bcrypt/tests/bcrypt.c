/*
 * Unit test for bcrypt functions
 *
 * Copyright 2014 Bruno Jesus
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#include <bcrypt.h>

#include "wine/test.h"

static NTSTATUS (WINAPI *pBCryptOpenAlgorithmProvider)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
static NTSTATUS (WINAPI *pBCryptCloseAlgorithmProvider)(BCRYPT_ALG_HANDLE, ULONG);
static NTSTATUS (WINAPI *pBCryptGetFipsAlgorithmMode)(BOOLEAN *);
static NTSTATUS (WINAPI *pBCryptCreateHash)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, PUCHAR, ULONG, PUCHAR,
                                            ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptHash)(BCRYPT_ALG_HANDLE, UCHAR *, ULONG, UCHAR *, ULONG, UCHAR *, ULONG);
static NTSTATUS (WINAPI *pBCryptHashData)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptDuplicateHash)(BCRYPT_HASH_HANDLE, BCRYPT_HASH_HANDLE *, UCHAR *, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptFinishHash)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptDestroyHash)(BCRYPT_HASH_HANDLE);
static NTSTATUS (WINAPI *pBCryptGenRandom)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptGetProperty)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG *, ULONG);
static NTSTATUS (WINAPI *pBCryptSetProperty)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptGenerateSymmetricKey)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE *, PUCHAR, ULONG,
                                                      PUCHAR, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptEncrypt)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID *, PUCHAR, ULONG, PUCHAR, ULONG,
                                      ULONG *, ULONG);
static NTSTATUS (WINAPI *pBCryptDecrypt)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID *, PUCHAR, ULONG, PUCHAR, ULONG,
                                      ULONG *, ULONG);
static NTSTATUS (WINAPI *pBCryptDestroyKey)(BCRYPT_KEY_HANDLE);
static NTSTATUS (WINAPI *pBCryptExportKey)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG *, ULONG);
static NTSTATUS (WINAPI *pBCryptFinalizeKeyPair)(BCRYPT_KEY_HANDLE, ULONG);
static NTSTATUS (WINAPI *pBCryptGenerateKeyPair)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE *, ULONG, ULONG);
static NTSTATUS (WINAPI *pBCryptImportKeyPair)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, BCRYPT_KEY_HANDLE *, PUCHAR, ULONG, DWORD);
static NTSTATUS (WINAPI *pBCryptSignHash)(BCRYPT_KEY_HANDLE, VOID *, PBYTE, DWORD, PBYTE, DWORD, DWORD *, ULONG);
static NTSTATUS (WINAPI *pBCryptVerifySignature)(BCRYPT_KEY_HANDLE, PVOID, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);

static void test_BCryptGenRandom(void)
{
    NTSTATUS ret;
    UCHAR buffer[256];

    ret = pBCryptGenRandom(NULL, NULL, 0, 0);
    ok(ret == STATUS_INVALID_HANDLE, "Expected STATUS_INVALID_HANDLE, got 0x%x\n", ret);
    ret = pBCryptGenRandom(NULL, buffer, 0, 0);
    ok(ret == STATUS_INVALID_HANDLE, "Expected STATUS_INVALID_HANDLE, got 0x%x\n", ret);
    ret = pBCryptGenRandom(NULL, buffer, sizeof(buffer), 0);
    ok(ret == STATUS_INVALID_HANDLE, "Expected STATUS_INVALID_HANDLE, got 0x%x\n", ret);
    ret = pBCryptGenRandom(NULL, buffer, sizeof(buffer), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok(ret == STATUS_SUCCESS, "Expected success, got 0x%x\n", ret);
    ret = pBCryptGenRandom(NULL, buffer, sizeof(buffer),
          BCRYPT_USE_SYSTEM_PREFERRED_RNG|BCRYPT_RNG_USE_ENTROPY_IN_BUFFER);
    ok(ret == STATUS_SUCCESS, "Expected success, got 0x%x\n", ret);
    ret = pBCryptGenRandom(NULL, NULL, sizeof(buffer), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok(ret == STATUS_INVALID_PARAMETER, "Expected STATUS_INVALID_PARAMETER, got 0x%x\n", ret);

    /* Zero sized buffer should work too */
    ret = pBCryptGenRandom(NULL, buffer, 0, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok(ret == STATUS_SUCCESS, "Expected success, got 0x%x\n", ret);

    /* Test random number generation - It's impossible for a sane RNG to return 8 zeros */
    memset(buffer, 0, 16);
    ret = pBCryptGenRandom(NULL, buffer, 8, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok(ret == STATUS_SUCCESS, "Expected success, got 0x%x\n", ret);
    ok(memcmp(buffer, buffer + 8, 8), "Expected a random number, got 0\n");
}

static void test_BCryptGetFipsAlgorithmMode(void)
{
    static const WCHAR policyKeyVistaW[] = {
        'S','y','s','t','e','m','\\',
        'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
        'C','o','n','t','r','o','l','\\',
        'L','s','a','\\',
        'F','I','P','S','A','l','g','o','r','i','t','h','m','P','o','l','i','c','y',0};
    static const WCHAR policyValueVistaW[] = {'E','n','a','b','l','e','d',0};
    static const WCHAR policyKeyXPW[] = {
        'S','y','s','t','e','m','\\',
        'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
        'C','o','n','t','r','o','l','\\',
        'L','s','a',0};
    static const WCHAR policyValueXPW[] = {
        'F','I','P','S','A','l','g','o','r','i','t','h','m','P','o','l','i','c','y',0};
    HKEY hkey = NULL;
    BOOLEAN expected;
    BOOLEAN enabled;
    DWORD value, count[2] = {sizeof(value), sizeof(value)};
    NTSTATUS ret;

    if (RegOpenKeyW(HKEY_LOCAL_MACHINE, policyKeyVistaW, &hkey) == ERROR_SUCCESS &&
        RegQueryValueExW(hkey, policyValueVistaW, NULL, NULL, (void *)&value, &count[0]) == ERROR_SUCCESS)
    {
        expected = !!value;
    }
      else if (RegOpenKeyW(HKEY_LOCAL_MACHINE, policyKeyXPW, &hkey) == ERROR_SUCCESS &&
               RegQueryValueExW(hkey, policyValueXPW, NULL, NULL, (void *)&value, &count[0]) == ERROR_SUCCESS)
    {
        expected = !!value;
    }
    else
    {
        expected = FALSE;
todo_wine
        ok(0, "Neither XP or Vista key is present\n");
    }
    RegCloseKey(hkey);

    ret = pBCryptGetFipsAlgorithmMode(&enabled);
    ok(ret == STATUS_SUCCESS, "Expected STATUS_SUCCESS, got 0x%x\n", ret);
    ok(enabled == expected, "expected result %d, got %d\n", expected, enabled);

    ret = pBCryptGetFipsAlgorithmMode(NULL);
    ok(ret == STATUS_INVALID_PARAMETER, "Expected STATUS_INVALID_PARAMETER, got 0x%x\n", ret);
}

static void format_hash(const UCHAR *bytes, ULONG size, char *buf)
{
    ULONG i;
    buf[0] = '\0';
    for (i = 0; i < size; i++)
    {
        sprintf(buf + i * 2, "%02x", bytes[i]);
    }
    return;
}

static int strcmp_wa(const WCHAR *strw, const char *stra)
{
    WCHAR buf[512];
    MultiByteToWideChar(CP_ACP, 0, stra, -1, buf, sizeof(buf)/sizeof(buf[0]));
    return lstrcmpW(strw, buf);
}

#define test_object_length(a) _test_object_length(__LINE__,a)
static void _test_object_length(unsigned line, void *handle)
{
    NTSTATUS status;
    ULONG len, size;

    len = size = 0xdeadbeef;
    status = pBCryptGetProperty(NULL, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok_(__FILE__,line)(status == STATUS_INVALID_HANDLE, "BCryptGetProperty failed: %08x\n", status);

    len = size = 0xdeadbeef;
    status = pBCryptGetProperty(handle, NULL, (UCHAR *)&len, sizeof(len), &size, 0);
    ok_(__FILE__,line)(status == STATUS_INVALID_PARAMETER, "BCryptGetProperty failed: %08x\n", status);

    len = size = 0xdeadbeef;
    status = pBCryptGetProperty(handle, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), NULL, 0);
    ok_(__FILE__,line)(status == STATUS_INVALID_PARAMETER, "BCryptGetProperty failed: %08x\n", status);

    len = size = 0xdeadbeef;
    status = pBCryptGetProperty(handle, BCRYPT_OBJECT_LENGTH, NULL, sizeof(len), &size, 0);
    ok_(__FILE__,line)(status == STATUS_SUCCESS, "BCryptGetProperty failed: %08x\n", status);
    ok_(__FILE__,line)(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    status = pBCryptGetProperty(handle, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, 0, &size, 0);
    ok_(__FILE__,line)(status == STATUS_BUFFER_TOO_SMALL, "BCryptGetProperty failed: %08x\n", status);
    ok_(__FILE__,line)(len == 0xdeadbeef, "got %u\n", len);
    ok_(__FILE__,line)(size == sizeof(len), "got %u\n", size);

    len = size = 0xdeadbeef;
    status = pBCryptGetProperty(handle, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok_(__FILE__,line)(status == STATUS_SUCCESS, "BCryptGetProperty failed: %08x\n", status);
    ok_(__FILE__,line)(len != 0xdeadbeef, "len not set\n");
    ok_(__FILE__,line)(size == sizeof(len), "got %u\n", size);
}

#define test_hash_length(a,b) _test_hash_length(__LINE__,a,b)
static void _test_hash_length(unsigned line, void *handle, ULONG exlen)
{
    ULONG len = 0xdeadbeef, size = 0xdeadbeef;
    NTSTATUS status;

    status = pBCryptGetProperty(handle, BCRYPT_HASH_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok_(__FILE__,line)(status == STATUS_SUCCESS, "BCryptGetProperty failed: %08x\n", status);
    ok_(__FILE__,line)(size == sizeof(len), "got %u\n", size);
    ok_(__FILE__,line)(len == exlen, "len = %u, expected %u\n", len, exlen);
}

#define test_alg_name(a,b) _test_alg_name(__LINE__,a,b)
static void _test_alg_name(unsigned line, void *handle, const char *exname)
{
    ULONG size = 0xdeadbeef;
    UCHAR buf[256];
    const WCHAR *name = (const WCHAR*)buf;
    NTSTATUS status;

    status = pBCryptGetProperty(handle, BCRYPT_ALGORITHM_NAME, buf, sizeof(buf), &size, 0);
    ok_(__FILE__,line)(status == STATUS_SUCCESS, "BCryptGetProperty failed: %08x\n", status);
    ok_(__FILE__,line)(size == (strlen(exname)+1)*sizeof(WCHAR), "got %u\n", size);
    ok_(__FILE__,line)(!strcmp_wa(name, exname), "alg name = %s, expected %s\n", wine_dbgstr_w(name), exname);
}

static void test_sha1(void)
{
    static const char expected[] = "961fa64958818f767707072755d7018dcd278e94";
    static const char expected_hmac[] = "2472cf65d0e090618d769d3e46f0d9446cf212da";
    UCHAR buf[512], buf_hmac[1024], buf_hmac2[1024], sha1[20], sha1_hmac[20];
    BCRYPT_HASH_HANDLE hash, hash2;
    BCRYPT_ALG_HANDLE alg;
    char str[41];
    NTSTATUS ret;
    ULONG len;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    test_object_length(alg);
    test_hash_length(alg, 20);
    test_alg_name(alg, "SHA1");

    hash = NULL;
    len = sizeof(buf);
    ret = pBCryptCreateHash(alg, &hash, buf, len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 20);
    test_alg_name(hash, "SHA1");

    memset(sha1, 0, sizeof(sha1));
    ret = pBCryptFinishHash(hash, sha1, sizeof(sha1), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha1, sizeof(sha1), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    hash = NULL;
    len = sizeof(buf_hmac);
    ret = pBCryptCreateHash(alg, &hash, buf_hmac, len, (UCHAR *)"key", sizeof("key"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 20);
    test_alg_name(hash, "SHA1");

    len = sizeof(buf_hmac2);
    ret = pBCryptDuplicateHash(NULL, &hash2, buf_hmac2, len, 0);
    ok(ret == STATUS_INVALID_HANDLE, "got %08x\n", ret);

    len = sizeof(buf_hmac2);
    ret = pBCryptDuplicateHash(hash, NULL, buf_hmac2, len, 0);
    ok(ret == STATUS_INVALID_PARAMETER, "got %08x\n", ret);

    hash2 = NULL;
    len = sizeof(buf_hmac2);
    ret = pBCryptDuplicateHash(hash, &hash2, buf_hmac2, len, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash2 != NULL, "hash not set\n");

    memset(sha1_hmac, 0, sizeof(sha1_hmac));
    ret = pBCryptFinishHash(hash2, sha1_hmac, sizeof(sha1_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha1_hmac, sizeof(sha1_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash2);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    memset(sha1_hmac, 0, sizeof(sha1_hmac));
    ret = pBCryptFinishHash(hash, sha1_hmac, sizeof(sha1_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha1_hmac, sizeof(sha1_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_sha256(void)
{
    static const char expected[] =
        "ceb73749c899693706ede1e30c9929b3fd5dd926163831c2fb8bd41e6efb1126";
    static const char expected_hmac[] =
        "34c1aa473a4468a91d06e7cdbc75bc4f93b830ccfc2a47ffd74e8e6ed29e4c72";
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    UCHAR buf[512], buf_hmac[1024], sha256[32], sha256_hmac[32];
    char str[65];
    NTSTATUS ret;
    ULONG len;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    test_object_length(alg);
    test_hash_length(alg, 32);
    test_alg_name(alg, "SHA256");

    hash = NULL;
    len = sizeof(buf);
    ret = pBCryptCreateHash(alg, &hash, buf, len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 32);
    test_alg_name(hash, "SHA256");

    memset(sha256, 0, sizeof(sha256));
    ret = pBCryptFinishHash(hash, sha256, sizeof(sha256), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha256, sizeof(sha256), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    hash = NULL;
    len = sizeof(buf_hmac);
    ret = pBCryptCreateHash(alg, &hash, buf_hmac, len, (UCHAR *)"key", sizeof("key"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 32);
    test_alg_name(hash, "SHA256");

    memset(sha256_hmac, 0, sizeof(sha256_hmac));
    ret = pBCryptFinishHash(hash, sha256_hmac, sizeof(sha256_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha256_hmac, sizeof(sha256_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_sha384(void)
{
    static const char expected[] =
        "62b21e90c9022b101671ba1f808f8631a8149f0f12904055839a35c1ca78ae5363eed1e743a692d70e0504b0cfd12ef9";
    static const char expected_hmac[] =
        "4b3e6d6ff2da121790ab7e7b9247583e3a7eed2db5bd4dabc680303b1608f37dfdc836d96a704c03283bc05b4f6c5eb8";
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    UCHAR buf[512], buf_hmac[1024], sha384[48], sha384_hmac[48];
    char str[97];
    NTSTATUS ret;
    ULONG len;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA384_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    test_object_length(alg);
    test_hash_length(alg, 48);
    test_alg_name(alg, "SHA384");

    hash = NULL;
    len = sizeof(buf);
    ret = pBCryptCreateHash(alg, &hash, buf, len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 48);
    test_alg_name(hash, "SHA384");

    memset(sha384, 0, sizeof(sha384));
    ret = pBCryptFinishHash(hash, sha384, sizeof(sha384), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha384, sizeof(sha384), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA384_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    hash = NULL;
    len = sizeof(buf_hmac);
    ret = pBCryptCreateHash(alg, &hash, buf_hmac, len, (UCHAR *)"key", sizeof("key"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 48);
    test_alg_name(hash, "SHA384");

    memset(sha384_hmac, 0, sizeof(sha384_hmac));
    ret = pBCryptFinishHash(hash, sha384_hmac, sizeof(sha384_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha384_hmac, sizeof(sha384_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_sha512(void)
{
    static const char expected[] =
        "d55ced17163bf5386f2cd9ff21d6fd7fe576a915065c24744d09cfae4ec84ee1e"
        "f6ef11bfbc5acce3639bab725b50a1fe2c204f8c820d6d7db0df0ecbc49c5ca";
    static const char expected_hmac[] =
        "415fb6b10018ca03b38a1b1399c42ac0be5e8aceddb9a73103f5e543bf2d888f2"
        "eecf91373941f9315dd730a77937fa92444450fbece86f409d9cb5ec48c6513";
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    UCHAR buf[512], buf_hmac[1024], sha512[64], sha512_hmac[64];
    char str[129];
    NTSTATUS ret;
    ULONG len;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    test_object_length(alg);
    test_hash_length(alg, 64);
    test_alg_name(alg, "SHA512");

    hash = NULL;
    len = sizeof(buf);
    ret = pBCryptCreateHash(alg, &hash, buf, len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 64);
    test_alg_name(hash, "SHA512");

    memset(sha512, 0, sizeof(sha512));
    ret = pBCryptFinishHash(hash, sha512, sizeof(sha512), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha512, sizeof(sha512), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    hash = NULL;
    len = sizeof(buf_hmac);
    ret = pBCryptCreateHash(alg, &hash, buf_hmac, len, (UCHAR *)"key", sizeof("key"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 64);
    test_alg_name(hash, "SHA512");

    memset(sha512_hmac, 0, sizeof(sha512_hmac));
    ret = pBCryptFinishHash(hash, sha512_hmac, sizeof(sha512_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( sha512_hmac, sizeof(sha512_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

}

static void test_md5(void)
{
    static const char expected[] =
        "e2a3e68d23ce348b8f68b3079de3d4c9";
    static const char expected_hmac[] =
        "7bda029b93fa8d817fcc9e13d6bdf092";
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    UCHAR buf[512], buf_hmac[1024], md5[16], md5_hmac[16];
    char str[65];
    NTSTATUS ret;
    ULONG len;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    test_object_length(alg);
    test_hash_length(alg, 16);
    test_alg_name(alg, "MD5");

    hash = NULL;
    len = sizeof(buf);
    ret = pBCryptCreateHash(alg, &hash, buf, len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 16);
    test_alg_name(hash, "MD5");

    memset(md5, 0, sizeof(md5));
    ret = pBCryptFinishHash(hash, md5, sizeof(md5), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( md5, sizeof(md5), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    hash = NULL;
    len = sizeof(buf_hmac);
    ret = pBCryptCreateHash(alg, &hash, buf_hmac, len, (UCHAR *)"key", sizeof("key"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(hash != NULL, "hash not set\n");

    ret = pBCryptHashData(hash, (UCHAR *)"test", sizeof("test"), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    test_hash_length(hash, 16);
    test_alg_name(hash, "MD5");

    memset(md5_hmac, 0, sizeof(md5_hmac));
    ret = pBCryptFinishHash(hash, md5_hmac, sizeof(md5_hmac), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( md5_hmac, sizeof(md5_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_BcryptHash(void)
{
    static const char expected[] =
        "e2a3e68d23ce348b8f68b3079de3d4c9";
    static const char expected_hmac[] =
        "7bda029b93fa8d817fcc9e13d6bdf092";
    BCRYPT_ALG_HANDLE alg;
    UCHAR md5[16], md5_hmac[16];
    char str[65];
    NTSTATUS ret;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    test_hash_length(alg, 16);
    test_alg_name(alg, "MD5");

    memset(md5, 0, sizeof(md5));
    ret = pBCryptHash(alg, NULL, 0, (UCHAR *)"test", sizeof("test"), md5, sizeof(md5));
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( md5, sizeof(md5), str );
    ok(!strcmp(str, expected), "got %s\n", str);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    alg = NULL;
    memset(md5_hmac, 0, sizeof(md5_hmac));
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    ret = pBCryptHash(alg, (UCHAR *)"key", sizeof("key"), (UCHAR *)"test", sizeof("test"), md5_hmac, sizeof(md5_hmac));
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    format_hash( md5_hmac, sizeof(md5_hmac), str );
    ok(!strcmp(str, expected_hmac), "got %s\n", str);

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_rng(void)
{
    BCRYPT_ALG_HANDLE alg;
    ULONG size, len;
    UCHAR buf[16];
    NTSTATUS ret;

    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_RNG_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_NOT_SUPPORTED, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_NOT_SUPPORTED, "got %08x\n", ret);

    test_alg_name(alg, "RNG");

    memset(buf, 0, 16);
    ret = pBCryptGenRandom(alg, buf, 8, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(memcmp(buf, buf + 8, 8), "got zeroes\n");

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_aes(void)
{
    BCRYPT_ALG_HANDLE alg;
    ULONG size, len;
    UCHAR mode[64];
    NTSTATUS ret;
todo_wine {
    alg = NULL;
    ret = pBCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(alg != NULL, "alg not set\n");

    len = size = 0;
    ret = pBCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(len, "expected non-zero len\n");
    ok(size == sizeof(len), "got %u\n", size);

    len = size = 0;
    ret = pBCryptGetProperty(alg, BCRYPT_BLOCK_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(len == 16, "got %u\n", len);
    ok(size == sizeof(len), "got %u\n", size);

    size = 0;
    ret = pBCryptGetProperty(alg, BCRYPT_CHAINING_MODE, mode, 0, &size, 0);
    ok(ret == STATUS_BUFFER_TOO_SMALL, "got %08x\n", ret);
    ok(size == 64, "got %u\n", size);

    size = 0;
    ret = pBCryptGetProperty(alg, BCRYPT_CHAINING_MODE, mode, sizeof(mode), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(!lstrcmpW((const WCHAR *)mode, BCRYPT_CHAIN_MODE_CBC), "got %s\n", mode);
    ok(size == 64, "got %u\n", size);

    test_alg_name(alg, "AES");

    ret = pBCryptCloseAlgorithmProvider(alg, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}
}

static void test_BCryptGenerateSymmetricKey(void)
{
    static UCHAR secret[] =
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static UCHAR iv[] =
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static UCHAR data[] =
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static UCHAR expected[] =
        {0xc6,0xa1,0x3b,0x37,0x87,0x8f,0x5b,0x82,0x6f,0x4f,0x81,0x62,0xa1,0xc8,0xd8,0x79};
    BCRYPT_ALG_HANDLE aes;
    BCRYPT_KEY_HANDLE key;
    UCHAR *buf, ciphertext[16], plaintext[16], ivbuf[16];
    ULONG size, len, i;
    NTSTATUS ret;

    ret = pBCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (ret != STATUS_SUCCESS) /* remove whole IF when Wine is fixed */
    {
        todo_wine ok(0, "AES provider not available\n");
        return;
    }
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    len = size = 0xdeadbeef;
    ret = pBCryptGetProperty(aes, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    key = NULL;
    buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
    ret = pBCryptGenerateSymmetricKey(aes, &key, buf, len, secret, sizeof(secret), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(key != NULL, "key not set\n");

    ret = pBCryptSetProperty(aes, BCRYPT_CHAINING_MODE, (UCHAR *)BCRYPT_CHAIN_MODE_CBC,
                            sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    todo_wine ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    size = 0xdeadbeef;
    ret = pBCryptEncrypt(key, NULL, 0, NULL, NULL, 0, NULL, 0, &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(!size, "got %u\n", size);

    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    ret = pBCryptEncrypt(key, data, 16, NULL, ivbuf, 16, NULL, 0, &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == 16, "got %u\n", size);

    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    memset(ciphertext, 0, sizeof(ciphertext));
    ret = pBCryptEncrypt(key, data, 16, NULL, ivbuf, 16, ciphertext, 16, &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == 16, "got %u\n", size);
    ok(!memcmp(ciphertext, expected, sizeof(expected)), "wrong data\n");
    for (i = 0; i < 16; i++)
        ok(ciphertext[i] == expected[i], "%u: %02x != %02x\n", i, ciphertext[i], expected[i]);

    size = 0xdeadbeef;
    ret = pBCryptDecrypt(key, NULL, 0, NULL, NULL, 0, NULL, 0, &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(!size, "got %u\n", size);

    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    ret = pBCryptDecrypt(key, ciphertext, 16, NULL, ivbuf, 16, NULL, 0, &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == 16, "got %u\n", size);

    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    memset(plaintext, 0, sizeof(plaintext));
    ret = pBCryptDecrypt(key, ciphertext, 16, NULL, ivbuf, 16, plaintext, 16, &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == 16, "got %u\n", size);
    ok(!memcmp(plaintext, data, sizeof(data)), "wrong data\n");

    ret = pBCryptDestroyKey(key);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    HeapFree(GetProcessHeap(), 0, buf);

    ret = pBCryptCloseAlgorithmProvider(aes, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_BCryptEncrypt(void)
{
    static UCHAR secret[] =
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static UCHAR iv[] =
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static UCHAR data[] =
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10};
    static UCHAR expected[] =
        {0xc6,0xa1,0x3b,0x37,0x87,0x8f,0x5b,0x82,0x6f,0x4f,0x81,0x62,0xa1,0xc8,0xd8,0x79};
    static UCHAR expected2[] =
        {0xc6,0xa1,0x3b,0x37,0x87,0x8f,0x5b,0x82,0x6f,0x4f,0x81,0x62,0xa1,0xc8,0xd8,0x79,
         0x28,0x73,0x3d,0xef,0x84,0x8f,0xb0,0xa6,0x5d,0x1a,0x51,0xb7,0xec,0x8f,0xea,0xe9};
    BCRYPT_ALG_HANDLE aes;
    BCRYPT_KEY_HANDLE key;
    UCHAR *buf, ciphertext[32], ivbuf[16];
    ULONG size, len, i;
    NTSTATUS ret;

    ret = pBCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (ret != STATUS_SUCCESS) /* remove whole IF when Wine is fixed */
    {
        todo_wine ok(0, "AES provider not available\n");
        return;
    }
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    len = 0xdeadbeef;
    size = sizeof(len);
    ret = pBCryptGetProperty(aes, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
    ret = pBCryptGenerateSymmetricKey(aes, &key, buf, len, secret, sizeof(secret), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    /* input size is a multiple of block size */
    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    ret = pBCryptEncrypt(key, data, 16, NULL, ivbuf, 16, NULL, 0, &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == 16, "got %u\n", size);

    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    memset(ciphertext, 0, sizeof(ciphertext));
    ret = pBCryptEncrypt(key, data, 16, NULL, ivbuf, 16, ciphertext, 16, &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == 16, "got %u\n", size);
    ok(!memcmp(ciphertext, expected, sizeof(expected)), "wrong data\n");
    for (i = 0; i < 16; i++)
        ok(ciphertext[i] == expected[i], "%u: %02x != %02x\n", i, ciphertext[i], expected[i]);

    /* input size is not a multiple of block size */
    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    ret = pBCryptEncrypt(key, data, 17, NULL, ivbuf, 16, NULL, 0, &size, 0);
    ok(ret == STATUS_INVALID_BUFFER_SIZE, "got %08x\n", ret);
    ok(size == 17, "got %u\n", size);

    /* input size is not a multiple of block size, block padding set */
    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    ret = pBCryptEncrypt(key, data, 17, NULL, ivbuf, 16, NULL, 0, &size, BCRYPT_BLOCK_PADDING);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == 32, "got %u\n", size);

    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    memset(ciphertext, 0, sizeof(ciphertext));
    ret = pBCryptEncrypt(key, data, 17, NULL, ivbuf, 16, ciphertext, 32, &size, BCRYPT_BLOCK_PADDING);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == 32, "got %u\n", size);
    ok(!memcmp(ciphertext, expected2, sizeof(expected2)), "wrong data\n");
    for (i = 0; i < 32; i++)
        ok(ciphertext[i] == expected2[i], "%u: %02x != %02x\n", i, ciphertext[i], expected2[i]);

    /* output size too small */
    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    memset(ciphertext, 0, sizeof(ciphertext));
    ret = pBCryptEncrypt(key, data, 17, NULL, ivbuf, 16, ciphertext, 31, &size, BCRYPT_BLOCK_PADDING);
    ok(ret == STATUS_BUFFER_TOO_SMALL, "got %08x\n", ret);
    ok(size == 32, "got %u\n", size);

    ret = pBCryptDestroyKey(key);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    HeapFree(GetProcessHeap(), 0, buf);

    ret = pBCryptCloseAlgorithmProvider(aes, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_BCryptDecrypt(void)
{
    static UCHAR secret[] =
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static UCHAR iv[] =
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static UCHAR expected[] =
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static UCHAR ciphertext[32] =
        {0xc6,0xa1,0x3b,0x37,0x87,0x8f,0x5b,0x82,0x6f,0x4f,0x81,0x62,0xa1,0xc8,0xd8,0x79,
         0x28,0x73,0x3d,0xef,0x84,0x8f,0xb0,0xa6,0x5d,0x1a,0x51,0xb7,0xec,0x8f,0xea,0xe9};
    BCRYPT_ALG_HANDLE aes;
    BCRYPT_KEY_HANDLE key;
    UCHAR *buf, plaintext[32], ivbuf[16];
    ULONG size, len;
    NTSTATUS ret;

    ret = pBCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (ret != STATUS_SUCCESS) /* remove whole IF when Wine is fixed */
    {
        todo_wine ok(0, "AES provider not available\n");
        return;
    }
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    len = 0xdeadbeef;
    size = sizeof(len);
    ret = pBCryptGetProperty(aes, BCRYPT_OBJECT_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
    ret = pBCryptGenerateSymmetricKey(aes, &key, buf, len, secret, sizeof(secret), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    /* input size is a multiple of block size */
    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    ret = pBCryptDecrypt(key, ciphertext, 32, NULL, ivbuf, 16, NULL, 0, &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == 32, "got %u\n", size);

    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    memset(plaintext, 0, sizeof(plaintext));
    ret = pBCryptDecrypt(key, ciphertext, 32, NULL, ivbuf, 16, plaintext, 32, &size, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(size == 32, "got %u\n", size);
    ok(!memcmp(plaintext, expected, sizeof(expected)), "wrong data\n");

    /* output size too small */
    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    ret = pBCryptDecrypt(key, ciphertext, 32, NULL, ivbuf, 16, plaintext, 31, &size, 0);
    ok(ret == STATUS_BUFFER_TOO_SMALL, "got %08x\n", ret);
    ok(size == 32, "got %u\n", size);

    /* input size is not a multiple of block size */
    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    ret = pBCryptDecrypt(key, ciphertext, 17, NULL, ivbuf, 16, NULL, 0, &size, 0);
    ok(ret == STATUS_INVALID_BUFFER_SIZE, "got %08x\n", ret);
    ok(size == 17 || broken(size == 0 /* Win < 7 */), "got %u\n", size);

    /* input size is not a multiple of block size, block padding set */
    size = 0;
    memcpy(ivbuf, iv, sizeof(iv));
    ret = pBCryptDecrypt(key, ciphertext, 17, NULL, ivbuf, 16, NULL, 0, &size, BCRYPT_BLOCK_PADDING);
    ok(ret == STATUS_INVALID_BUFFER_SIZE, "got %08x\n", ret);
    ok(size == 17 || broken(size == 0 /* Win < 7 */), "got %u\n", size);

    ret = pBCryptDestroyKey(key);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    HeapFree(GetProcessHeap(), 0, buf);

    ret = pBCryptCloseAlgorithmProvider(aes, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void dump_chars(const char* name, UCHAR* bytes, ULONG len)
{
    int i;
    printf("static UCHAR %s[/*%d*/] = {\n", name, len);
    for (i = 0; i < len; i++) {
        printf("0x%2.2x,", bytes[i]);
        if ( ((i+1) % 16) == 0) printf("\n");
    }
    printf("\n");
    printf("};\n");
}

static void test_BCryptVerifySignature_generate_keys(void)
{
    NTSTATUS ret;
    BCRYPT_ALG_HANDLE algorithm;

    BCRYPT_KEY_HANDLE key;
    UCHAR* key_export;
    ULONG key_kexport_len;

    if (!strcmp(winetest_platform, "wine"))
    {
        todo_wine ok(0, "BCryptGenerateKeyPair, BCryptFinalizeKeyPair, BCryptExportKey not yet implemented.\n");
        return;
    }

    /* RSA, create key */
    ret = pBCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, NULL, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptGenerateKeyPair(algorithm, &key, 2048, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptFinalizeKeyPair(key, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    key_kexport_len = 0;
    ret = pBCryptExportKey(key, 0, BCRYPT_RSAPRIVATE_BLOB, NULL, 0, &key_kexport_len, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    key_export = HeapAlloc(GetProcessHeap(), 0, key_kexport_len);
    ret = pBCryptExportKey(key, 0, BCRYPT_RSAPRIVATE_BLOB, key_export, key_kexport_len, &key_kexport_len, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    dump_chars("rsa_private_key_blob", key_export, key_kexport_len);
    HeapFree(GetProcessHeap(), 0, key_export);

    key_kexport_len = 0;
    ret = pBCryptExportKey(key, 0, BCRYPT_RSAPUBLIC_BLOB, NULL, 0, &key_kexport_len, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    key_export = HeapAlloc(GetProcessHeap(), 0, key_kexport_len);
    ret = pBCryptExportKey(key, 0, BCRYPT_RSAPUBLIC_BLOB, key_export, key_kexport_len, &key_kexport_len, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    dump_chars("rsa_public_key_blob", key_export, key_kexport_len);
    HeapFree(GetProcessHeap(), 0, key_export);
}

static UCHAR rsa_private_key_blob[/*539*/] =
    {0x52,0x53,0x41,0x32,0x00,0x08,0x00,0x00,0x03,0x00,0x00,0x00,0x00,0x01,0x00,0x00,
     0x80,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x01,0x00,0x01,0xad,0x41,0x09,0xa2,0x56,
     0x3a,0x7b,0x75,0x4b,0x72,0x9b,0x28,0x72,0x3b,0xae,0x9f,0xd8,0xa8,0x25,0x4a,0x4c,
     0x19,0xf5,0xa6,0xd0,0x05,0x1c,0x59,0x8f,0xe3,0xf3,0x2d,0x29,0x47,0xf8,0x80,0x25,
     0x25,0x21,0x58,0xc2,0xac,0xa1,0x9e,0x93,0x8e,0x82,0x6d,0xd7,0xf3,0xe7,0x8f,0x0b,
     0xc0,0x41,0x85,0x29,0x3c,0xf1,0x0b,0x2c,0x5d,0x49,0xed,0xb4,0x30,0x6e,0x02,0x15,
     0x4b,0x9a,0x08,0x0d,0xe1,0x6f,0xa8,0xd3,0x12,0xab,0x66,0x48,0x4d,0xd9,0x28,0x03,
     0x6c,0x9d,0x44,0x7a,0xed,0xc9,0x43,0x4f,0x9d,0x4e,0x3c,0x7d,0x0e,0xff,0x07,0x87,
     0xeb,0xca,0xca,0x65,0x6d,0xbe,0xc5,0x31,0x8b,0xcc,0x7e,0x0a,0x71,0x4a,0x4d,0x9d,
     0x3d,0xfd,0x7a,0x56,0x32,0x8a,0x6c,0x6d,0x9d,0x2a,0xd9,0x8e,0x68,0x89,0x63,0xc6,
     0x4f,0x24,0xd1,0x2a,0x72,0x69,0x08,0x77,0xa0,0x7f,0xfe,0xc6,0x33,0x8d,0xb4,0x7d,
     0x73,0x91,0x13,0x9c,0x47,0x53,0x6a,0x13,0xdf,0x19,0xc7,0xed,0x48,0x81,0xed,0xd8,
     0x1f,0x11,0x11,0xbb,0x41,0x15,0x5b,0xa4,0xf5,0xc9,0x2b,0x48,0x5e,0xd8,0x4b,0x52,
     0x1f,0xf7,0x87,0xf2,0x68,0x25,0x28,0x79,0xee,0x39,0x41,0xc9,0x0e,0xc8,0xf9,0xf2,
     0xd8,0x24,0x09,0xb4,0xd4,0xb7,0x90,0xba,0x26,0xe8,0x1d,0xb4,0xd7,0x09,0x00,0xc4,
     0xa0,0xb6,0x14,0xe8,0x4c,0x29,0x60,0x54,0x2e,0x01,0xde,0x54,0x66,0x40,0x22,0x50,
     0x27,0xf1,0xe7,0x62,0xa9,0x00,0x5a,0x61,0x2e,0xfa,0xfe,0x16,0xd8,0xe0,0xe7,0x66,
     0x17,0xda,0xb8,0x0c,0xa6,0x04,0x8d,0xf8,0x21,0x68,0x39,0xcd,0x9b,0x21,0x4c,0xa4,
     0x9d,0x13,0x2c,0x7c,0x3e,0x57,0x90,0xb3,0xf3,0x58,0xad,0x30,0x82,0xef,0x66,0x24,
     0x14,0xe2,0x93,0xaf,0xe4,0xaf,0xa5,0xb2,0xdd,0xf0,0xbb,0x24,0x78,0x1b,0x95,0xc9,
     0xf5,0xf9,0x32,0x80,0x74,0x87,0x52,0x78,0xad,0x3c,0x35,0xe8,0xa1,0x10,0x8f,0x3d,
     0x74,0x43,0x3a,0xc6,0x7f,0x89,0xdb,0x76,0x1a,0x82,0xd9,0xa1,0x75,0x87,0x67,0x9d,
     0x3a,0xc8,0xe1,0x82,0x84,0x4d,0xe1,0xd7,0xce,0x16,0x50,0x93,0xc5,0x8f,0x3b,0xc1,
     0x8d,0x28,0x48,0x4e,0x96,0xf1,0x0f,0xad,0x95,0xb3,0x31,0x74,0xd7,0xe5,0xbb,0x51,
     0xf9,0x34,0x17,0x5a,0xc7,0x44,0x31,0x33,0x75,0xc4,0xd9,0xc2,0x06,0x5a,0x7b,0x59,
     0xe8,0x80,0xc7,0xf3,0x8f,0x04,0x95,0x11,0x46,0xd6,0x85,0xd7,0xb7,0xf7,0x7e,0x5d,
     0x05,0x39,0xc4,0x40,0xc1,0x8e,0xbc,0x37,0x91,0x37,0xbe,0xa3,0xfb,0x0c,0x03,0x1b,
     0x4f,0x8e,0xc4,0x02,0xb1,0xc9,0xee,0x03,0xee,0x77,0xd0,0x61,0x6e,0x86,0x9b,0x84,
     0xab,0x35,0xd7,0xf1,0x38,0xac,0xed,0xc1,0x33,0x0e,0xaa,0xc9,0x91,0xda,0x30,0x80,
     0x5c,0x2b,0x04,0x0d,0xf3,0xfa,0x25,0x3e,0x0a,0x9e,0x67,0xb3,0x08,0x19,0xe3,0x86,
     0x22,0xb3,0xee,0x21,0x1d,0x5a,0xd9,0x2f,0xbb,0x4b,0xda,0x47,0x12,0xf7,0x85,0x0d,
     0xcf,0x97,0x75,0x2e,0x80,0x09,0xea,0xea,0x18,0x05,0x68,0x07,0x7a,0x24,0x52,0x34,
     0x63,0x14,0xcf,0xe5,0x5c,0x15,0x12,0xd9,0xb2,0xc5,0x4e,0x4b,0x65,0x1b,0xec,0x77,
     0xc3,0xc1,0x08,0x47,0xc0,0xc5,0x67,0x7f,0x50,0xfb,0x25};

static UCHAR rsa_public_key_blob[/*283*/] =
    {0x52,0x53,0x41,0x31,0x00,0x08,0x00,0x00,0x03,0x00,0x00,0x00,0x00,0x01,0x00,0x00,
     0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0xad,0x41,0x09,0xa2,0x56,
     0x3a,0x7b,0x75,0x4b,0x72,0x9b,0x28,0x72,0x3b,0xae,0x9f,0xd8,0xa8,0x25,0x4a,0x4c,
     0x19,0xf5,0xa6,0xd0,0x05,0x1c,0x59,0x8f,0xe3,0xf3,0x2d,0x29,0x47,0xf8,0x80,0x25,
     0x25,0x21,0x58,0xc2,0xac,0xa1,0x9e,0x93,0x8e,0x82,0x6d,0xd7,0xf3,0xe7,0x8f,0x0b,
     0xc0,0x41,0x85,0x29,0x3c,0xf1,0x0b,0x2c,0x5d,0x49,0xed,0xb4,0x30,0x6e,0x02,0x15,
     0x4b,0x9a,0x08,0x0d,0xe1,0x6f,0xa8,0xd3,0x12,0xab,0x66,0x48,0x4d,0xd9,0x28,0x03,
     0x6c,0x9d,0x44,0x7a,0xed,0xc9,0x43,0x4f,0x9d,0x4e,0x3c,0x7d,0x0e,0xff,0x07,0x87,
     0xeb,0xca,0xca,0x65,0x6d,0xbe,0xc5,0x31,0x8b,0xcc,0x7e,0x0a,0x71,0x4a,0x4d,0x9d,
     0x3d,0xfd,0x7a,0x56,0x32,0x8a,0x6c,0x6d,0x9d,0x2a,0xd9,0x8e,0x68,0x89,0x63,0xc6,
     0x4f,0x24,0xd1,0x2a,0x72,0x69,0x08,0x77,0xa0,0x7f,0xfe,0xc6,0x33,0x8d,0xb4,0x7d,
     0x73,0x91,0x13,0x9c,0x47,0x53,0x6a,0x13,0xdf,0x19,0xc7,0xed,0x48,0x81,0xed,0xd8,
     0x1f,0x11,0x11,0xbb,0x41,0x15,0x5b,0xa4,0xf5,0xc9,0x2b,0x48,0x5e,0xd8,0x4b,0x52,
     0x1f,0xf7,0x87,0xf2,0x68,0x25,0x28,0x79,0xee,0x39,0x41,0xc9,0x0e,0xc8,0xf9,0xf2,
     0xd8,0x24,0x09,0xb4,0xd4,0xb7,0x90,0xba,0x26,0xe8,0x1d,0xb4,0xd7,0x09,0x00,0xc4,
     0xa0,0xb6,0x14,0xe8,0x4c,0x29,0x60,0x54,0x2e,0x01,0xde,0x54,0x66,0x40,0x22,0x50,
     0x27,0xf1,0xe7,0x62,0xa9,0x00,0x5a,0x61,0x2e,0xfa,0xfe,0x16,0xd8,0xe0,0xe7,0x66,
     0x17,0xda,0xb8,0x0c,0xa6,0x04,0x8d,0xf8,0x21,0x68,0x39
};

static UCHAR sha1_hash[] =
    {0x96,0x1f,0xa6,0x49,0x58,0x81,0x8f,0x76,0x77,0x07,0x07,0x27,0x55,0xd7,0x01,0x8d,
     0xcd,0x27,0x8e,0x94};

static void test_BCryptVerifySignature_generate_hash(void)
{
    NTSTATUS ret;
    BCRYPT_ALG_HANDLE algorithm;

    BCRYPT_HASH_HANDLE hash;

    UCHAR* hash_object;
    ULONG hash_object_len;
    ULONG hash_object_len_len;

    UCHAR* hash_export;
    ULONG hash_export_len;
    ULONG hash_export_len_len;

    UCHAR hash_data[] = "test";

    /* SHA1, create hash */
    ret = pBCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, NULL, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    hash_object_len = 0;
    hash_object_len_len = 0;
    ret = pBCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (UCHAR*)&hash_object_len, sizeof(hash_object_len), &hash_object_len_len, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    hash_object = HeapAlloc(GetProcessHeap(), 0, hash_object_len);
    ret = pBCryptCreateHash(algorithm, &hash, hash_object, hash_object_len, NULL, 0, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    hash_export_len = 0;
    hash_export_len_len = 0;
    ret = pBCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, (UCHAR*)&hash_export_len, sizeof(hash_export_len), &hash_export_len_len, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    hash_export = HeapAlloc(GetProcessHeap(), 0, hash_export_len);
    ret = pBCryptHashData(hash, hash_data, sizeof(hash_data), 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptFinishHash(hash, hash_export, hash_export_len, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(sizeof(sha1_hash) == hash_export_len, "hash sizes are different.\n");
    ok(!memcmp(sha1_hash, hash_export, hash_export_len), "hashes are different.\n");
    /*dump_chars("hash", hash_export, hash_export_len);*/

    ret = pBCryptDestroyHash(hash);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(algorithm, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    HeapFree(GetProcessHeap(), 0, hash_export);
    HeapFree(GetProcessHeap(), 0, hash_object);
}

static UCHAR signature[/*256*/] =
    {0xa8,0x3a,0x9d,0xaf,0x92,0x94,0xa4,0x4d,0x34,0xba,0x41,0x0c,0xc1,0x23,0x91,0xc7,
     0x91,0xa8,0xf8,0xfc,0x94,0x87,0x4d,0x05,0x85,0x63,0xe8,0x7d,0xea,0x7f,0x6b,0x8d,
     0xbb,0x9a,0xd4,0x46,0xa6,0xc0,0xd6,0xdc,0x91,0xba,0xd3,0x1a,0xbf,0xf4,0x52,0xa0,
     0xc7,0x15,0x87,0xe9,0x1e,0x60,0x49,0x9c,0xee,0x5a,0x9c,0x6c,0xbd,0x7a,0x3e,0xc3,
     0x48,0xb3,0xee,0xca,0x68,0x40,0x9b,0xa1,0x4c,0x6e,0x20,0xd6,0xca,0x6c,0x72,0xaf,
     0x2b,0x6b,0x62,0x7c,0x78,0x06,0x94,0x4c,0x02,0xf3,0x8d,0x49,0xe0,0x11,0xc4,0x9b,
     0x62,0x5b,0xc2,0xfd,0x68,0xf4,0x07,0x15,0x71,0x11,0x4c,0x35,0x97,0x5d,0xc0,0xe6,
     0x22,0xc9,0x8a,0x7b,0x96,0xc9,0xc3,0xe4,0x2b,0x1e,0x88,0x17,0x4f,0x98,0x9b,0xf3,
     0x42,0x23,0x0c,0xa0,0xfa,0x19,0x03,0x2a,0xf7,0x13,0x2d,0x27,0xac,0x9f,0xaf,0x2d,
     0xa3,0xce,0xf7,0x63,0xbb,0x39,0x9f,0x72,0x80,0xdd,0x6c,0x73,0x00,0x85,0x70,0xf2,
     0xed,0x50,0xed,0xa0,0x74,0x42,0xd7,0x22,0x46,0x24,0xee,0x67,0xdf,0xb5,0x45,0xe8,
     0x49,0xf4,0x9c,0xe4,0x00,0x83,0xf2,0x27,0x8e,0xa2,0xb1,0xc3,0xc2,0x01,0xd7,0x59,
     0x2e,0x4d,0xac,0x49,0xa2,0xc1,0x8d,0x88,0x4b,0xfe,0x28,0xe5,0xac,0xa6,0x85,0xc4,
     0x1f,0xf8,0xc5,0xc5,0x14,0x4e,0xa3,0xcb,0x17,0xb7,0x64,0xb3,0xc2,0x12,0xf8,0xf8,
     0x36,0x99,0x1c,0x91,0x9b,0xbd,0xed,0x55,0x0f,0xfd,0x49,0x85,0xbb,0x32,0xad,0x78,
     0xc1,0x74,0xe6,0x7c,0x18,0x0f,0x2b,0x3b,0xaa,0xd1,0x9d,0x40,0x71,0x1d,0x19,0x53};

static void test_BCryptVerifySignature_generate_signature(void)
{
    NTSTATUS ret;
    BCRYPT_ALG_HANDLE algorithm;

    BCRYPT_KEY_HANDLE key;

    BCRYPT_PKCS1_PADDING_INFO padding_info;
    UCHAR* signature_export;
    ULONG signature_export_len;

    if (!strcmp(winetest_platform, "wine"))
    {
        todo_wine ok(0, "BCryptSignHash not yet implemented.\n");
        return;
    }

    /* Import RSA private key */
    ret = pBCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, NULL, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptImportKeyPair(algorithm, NULL, BCRYPT_RSAPRIVATE_BLOB, &key, rsa_private_key_blob, sizeof(rsa_private_key_blob), 0);
    ok(ret == STATUS_SUCCESS, "Expected STATUS_SUCCESS, got 0x%x\n", ret);

    /* RSA, sign hash */
    padding_info.pszAlgId = BCRYPT_SHA1_ALGORITHM;
    ret = pBCryptSignHash(key, &padding_info, sha1_hash, sizeof(sha1_hash), NULL, 0, &signature_export_len, BCRYPT_PAD_PKCS1);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    signature_export = HeapAlloc(GetProcessHeap(), 0, signature_export_len);
    ret = pBCryptSignHash(key, &padding_info, sha1_hash, sizeof(sha1_hash), signature_export, signature_export_len, &signature_export_len, BCRYPT_PAD_PKCS1);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
    ok(sizeof(signature) == signature_export_len, "signature sizes are different.\n");
    ok(!memcmp(signature, signature_export, signature_export_len), "signatures are different.\n");
    /*dump_chars("signature", signature_export, signature_export_len);*/
    HeapFree(GetProcessHeap(), 0, signature_export);

    ret = pBCryptDestroyKey(key);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(algorithm, 0);
    ok(ret == STATUS_SUCCESS, "got %08x\n", ret);
}

static void test_BCryptVerifySignature(void)
{
    NTSTATUS ret;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    BCRYPT_PKCS1_PADDING_INFO padding_info;

    ret = pBCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, NULL, 0);
    ok(ret == STATUS_SUCCESS, "Expected STATUS_SUCCESS, got 0x%x\n", ret);

    ret = pBCryptImportKeyPair(algorithm, NULL, BCRYPT_RSAPUBLIC_BLOB, &key, rsa_public_key_blob, sizeof(rsa_public_key_blob), 0);
    ok(ret == STATUS_SUCCESS, "Expected STATUS_SUCCESS, got 0x%x\n", ret);

    padding_info.pszAlgId = BCRYPT_SHA1_ALGORITHM;
    ret = pBCryptVerifySignature(key, &padding_info, sha1_hash, sizeof(sha1_hash), signature, sizeof(signature), BCRYPT_PAD_PKCS1);
    ok(ret == STATUS_SUCCESS, "Expected STATUS_SUCCESS, got 0x%x\n", ret);

    ret = pBCryptDestroyKey(key);
    ok(ret == STATUS_SUCCESS, "Expected STATUS_SUCCESS, got 0x%x\n", ret);

    ret = pBCryptCloseAlgorithmProvider(algorithm, 0);
    ok(ret == STATUS_SUCCESS, "Expected STATUS_SUCCESS, got 0x%x\n", ret);
}

START_TEST(bcrypt)
{
    HMODULE module;

    module = LoadLibraryA("bcrypt.dll");
    if (!module)
    {
        win_skip("bcrypt.dll not found\n");
        return;
    }

    pBCryptOpenAlgorithmProvider = (void *)GetProcAddress(module, "BCryptOpenAlgorithmProvider");
    pBCryptCloseAlgorithmProvider = (void *)GetProcAddress(module, "BCryptCloseAlgorithmProvider");
    pBCryptGetFipsAlgorithmMode = (void *)GetProcAddress(module, "BCryptGetFipsAlgorithmMode");
    pBCryptCreateHash = (void *)GetProcAddress(module, "BCryptCreateHash");
    pBCryptHash = (void *)GetProcAddress(module, "BCryptHash");
    pBCryptHashData = (void *)GetProcAddress(module, "BCryptHashData");
    pBCryptDuplicateHash = (void *)GetProcAddress(module, "BCryptDuplicateHash");
    pBCryptFinishHash = (void *)GetProcAddress(module, "BCryptFinishHash");
    pBCryptDestroyHash = (void *)GetProcAddress(module, "BCryptDestroyHash");
    pBCryptGenRandom = (void *)GetProcAddress(module, "BCryptGenRandom");
    pBCryptGetProperty = (void *)GetProcAddress(module, "BCryptGetProperty");
    pBCryptSetProperty = (void *)GetProcAddress(module, "BCryptSetProperty");
    pBCryptGenerateSymmetricKey = (void *)GetProcAddress(module, "BCryptGenerateSymmetricKey");
    pBCryptEncrypt = (void *)GetProcAddress(module, "BCryptEncrypt");
    pBCryptDecrypt = (void *)GetProcAddress(module, "BCryptDecrypt");
    pBCryptDestroyKey = (void *)GetProcAddress(module, "BCryptDestroyKey");
    pBCryptExportKey = (void *)GetProcAddress(module, "BCryptExportKey");
    pBCryptFinalizeKeyPair = (void *)GetProcAddress(module, "BCryptFinalizeKeyPair");
    pBCryptGenerateKeyPair = (void *)GetProcAddress(module, "BCryptGenerateKeyPair");
    pBCryptImportKeyPair = (void *)GetProcAddress(module, "BCryptImportKeyPair");
    pBCryptSignHash = (void *)GetProcAddress(module, "BCryptSignHash");
    pBCryptVerifySignature = (void *)GetProcAddress(module, "BCryptVerifySignature");

    test_BCryptGenRandom();
    test_BCryptGetFipsAlgorithmMode();
    test_sha1();
    test_sha256();
    test_sha384();
    test_sha512();
    test_md5();
    test_rng();
    test_aes();
    test_BCryptGenerateSymmetricKey();
    test_BCryptEncrypt();
    test_BCryptDecrypt();
    if (0) test_BCryptVerifySignature_generate_keys(); /* generates always different keys, so cannot test result */
    test_BCryptVerifySignature_generate_hash();
    test_BCryptVerifySignature_generate_signature();
    test_BCryptVerifySignature();

    if (pBCryptHash) /* >= Win 10 */
        test_BcryptHash();
    else
        win_skip("BCryptHash is not available\n");

    FreeLibrary(module);
}
