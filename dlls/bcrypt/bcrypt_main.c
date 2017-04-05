/*
 * Copyright 2009 Henri Verbeet for CodeWeavers
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
 *
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#ifdef HAVE_COMMONCRYPTO_COMMONDIGEST_H
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#elif defined(SONAME_LIBGNUTLS)
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <gnutls/abstract.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "ntsecapi.h"
#include "bcrypt.h"

#include "bcrypt_internal.h"

#include "wine/debug.h"
#include "wine/library.h"
#include "wine/unicode.h"

WINE_DEFAULT_DEBUG_CHANNEL(bcrypt);

static HINSTANCE instance;

#if defined(HAVE_GNUTLS_HASH) && !defined(HAVE_COMMONCRYPTO_COMMONDIGEST_H)
WINE_DECLARE_DEBUG_CHANNEL(winediag);

static void *libgnutls_handle;
#define MAKE_FUNCPTR(f) static typeof(f) * p##f
MAKE_FUNCPTR(gnutls_global_deinit);
MAKE_FUNCPTR(gnutls_global_init);
MAKE_FUNCPTR(gnutls_global_set_log_function);
MAKE_FUNCPTR(gnutls_global_set_log_level);
MAKE_FUNCPTR(gnutls_perror);
MAKE_FUNCPTR(gnutls_pk_to_sign);
MAKE_FUNCPTR(gnutls_pubkey_deinit);
MAKE_FUNCPTR(gnutls_pubkey_get_pk_algorithm);
MAKE_FUNCPTR(gnutls_pubkey_import_rsa_raw);
MAKE_FUNCPTR(gnutls_pubkey_init);
MAKE_FUNCPTR(gnutls_pubkey_verify_hash2);
#undef MAKE_FUNCPTR

static void gnutls_log( int level, const char *msg )
{
    TRACE( "<%d> %s", level, msg );
}

static BOOL gnutls_initialize(void)
{
    int ret;

    if (!(libgnutls_handle = wine_dlopen( SONAME_LIBGNUTLS, RTLD_NOW, NULL, 0 )))
    {
        ERR_(winediag)( "failed to load libgnutls, no support for crypto hashes\n" );
        return FALSE;
    }

#define LOAD_FUNCPTR(f) \
    if (!(p##f = wine_dlsym( libgnutls_handle, #f, NULL, 0 ))) \
    { \
        ERR( "failed to load %s\n", #f ); \
        goto fail; \
    }

    LOAD_FUNCPTR(gnutls_global_deinit)
    LOAD_FUNCPTR(gnutls_global_init)
    LOAD_FUNCPTR(gnutls_global_set_log_function)
    LOAD_FUNCPTR(gnutls_global_set_log_level)
    LOAD_FUNCPTR(gnutls_perror)
    LOAD_FUNCPTR(gnutls_pk_to_sign)
    LOAD_FUNCPTR(gnutls_pubkey_deinit)
    LOAD_FUNCPTR(gnutls_pubkey_get_pk_algorithm)
    LOAD_FUNCPTR(gnutls_pubkey_import_rsa_raw)
    LOAD_FUNCPTR(gnutls_pubkey_init)
    LOAD_FUNCPTR(gnutls_pubkey_verify_hash2)
#undef LOAD_FUNCPTR

    if ((ret = pgnutls_global_init()) != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror( ret );
        goto fail;
    }

    if (TRACE_ON( bcrypt ))
    {
        pgnutls_global_set_log_level( 4 );
        pgnutls_global_set_log_function( gnutls_log );
    }

    return TRUE;

fail:
    wine_dlclose( libgnutls_handle, NULL, 0 );
    libgnutls_handle = NULL;
    return FALSE;
}

static void gnutls_uninitialize(void)
{
    pgnutls_global_deinit();
    wine_dlclose( libgnutls_handle, NULL, 0 );
    libgnutls_handle = NULL;
}
#endif /* HAVE_GNUTLS_HASH && !HAVE_COMMONCRYPTO_COMMONDIGEST_H */

NTSTATUS WINAPI BCryptEnumAlgorithms(ULONG dwAlgOperations, ULONG *pAlgCount,
                                     BCRYPT_ALGORITHM_IDENTIFIER **ppAlgList, ULONG dwFlags)
{
    FIXME("%08x, %p, %p, %08x - stub\n", dwAlgOperations, pAlgCount, ppAlgList, dwFlags);

    *ppAlgList=NULL;
    *pAlgCount=0;

    return STATUS_NOT_IMPLEMENTED;
}

#define MAGIC_ALG  (('A' << 24) | ('L' << 16) | ('G' << 8) | '0')
#define MAGIC_HASH (('H' << 24) | ('A' << 16) | ('S' << 8) | 'H')
struct object
{
    ULONG magic;
};

enum alg_id
{
    ALG_ID_MD5,
    ALG_ID_RNG,
    ALG_ID_SHA1,
    ALG_ID_SHA256,
    ALG_ID_SHA384,
    ALG_ID_SHA512,
    ALG_ID_RSA
};

#define MAX_HASH_OUTPUT_BYTES 64
#define MAX_HASH_BLOCK_BITS 1024

static const struct {
    ULONG object_length;
    ULONG hash_length;
    ULONG block_bits;
    const WCHAR *alg_name;
} alg_props[] = {
    /* ALG_ID_MD5    */ {  274,   16,  512, BCRYPT_MD5_ALGORITHM },
    /* ALG_ID_RNG    */ {    0,    0,    0, BCRYPT_RNG_ALGORITHM },
    /* ALG_ID_SHA1   */ {  278,   20,  512, BCRYPT_SHA1_ALGORITHM },
    /* ALG_ID_SHA256 */ {  286,   32,  512, BCRYPT_SHA256_ALGORITHM },
    /* ALG_ID_SHA384 */ {  382,   48, 1024, BCRYPT_SHA384_ALGORITHM },
    /* ALG_ID_SHA512 */ {  382,   64, 1024, BCRYPT_SHA512_ALGORITHM }
};

struct algorithm
{
    struct object hdr;
    enum alg_id   id;
    BOOL hmac;
};

NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE handle, UCHAR *buffer, ULONG count, ULONG flags)
{
    const DWORD supported_flags = BCRYPT_USE_SYSTEM_PREFERRED_RNG;
    struct algorithm *algorithm = handle;

    TRACE("%p, %p, %u, %08x - semi-stub\n", handle, buffer, count, flags);

    if (!algorithm)
    {
        /* It's valid to call without an algorithm if BCRYPT_USE_SYSTEM_PREFERRED_RNG
         * is set. In this case the preferred system RNG is used.
         */
        if (!(flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG))
            return STATUS_INVALID_HANDLE;
    }
    else if (algorithm->hdr.magic != MAGIC_ALG || algorithm->id != ALG_ID_RNG)
        return STATUS_INVALID_HANDLE;

    if (!buffer)
        return STATUS_INVALID_PARAMETER;

    if (flags & ~supported_flags)
        FIXME("unsupported flags %08x\n", flags & ~supported_flags);

    if (algorithm)
        FIXME("ignoring selected algorithm\n");

    /* When zero bytes are requested the function returns success too. */
    if (!count)
        return STATUS_SUCCESS;

    if (algorithm || (flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG))
    {
        if (RtlGenRandom(buffer, count))
            return STATUS_SUCCESS;
    }

    FIXME("called with unsupported parameters, returning error\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS WINAPI BCryptOpenAlgorithmProvider( BCRYPT_ALG_HANDLE *handle, LPCWSTR id, LPCWSTR implementation, DWORD flags )
{
    struct algorithm *alg;
    enum alg_id alg_id;

    const DWORD supported_flags = BCRYPT_ALG_HANDLE_HMAC_FLAG;

    TRACE( "%p, %s, %s, %08x\n", handle, wine_dbgstr_w(id), wine_dbgstr_w(implementation), flags );

    if (!handle || !id) return STATUS_INVALID_PARAMETER;
    if (flags & ~supported_flags)
    {
        FIXME( "unsupported flags %08x\n", flags & ~supported_flags);
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!strcmpW( id, BCRYPT_SHA1_ALGORITHM )) alg_id = ALG_ID_SHA1;
    else if (!strcmpW( id, BCRYPT_MD5_ALGORITHM )) alg_id = ALG_ID_MD5;
    else if (!strcmpW( id, BCRYPT_RNG_ALGORITHM )) alg_id = ALG_ID_RNG;
    else if (!strcmpW( id, BCRYPT_SHA256_ALGORITHM )) alg_id = ALG_ID_SHA256;
    else if (!strcmpW( id, BCRYPT_SHA384_ALGORITHM )) alg_id = ALG_ID_SHA384;
    else if (!strcmpW( id, BCRYPT_SHA512_ALGORITHM )) alg_id = ALG_ID_SHA512;
    else if (!strcmpW( id, BCRYPT_RSA_ALGORITHM )) alg_id = ALG_ID_RSA;
    else
    {
        FIXME( "algorithm %s not supported\n", debugstr_w(id) );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (implementation && strcmpW( implementation, MS_PRIMITIVE_PROVIDER ))
    {
        FIXME( "implementation %s not supported\n", debugstr_w(implementation) );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!(alg = HeapAlloc( GetProcessHeap(), 0, sizeof(*alg) ))) return STATUS_NO_MEMORY;
    alg->hdr.magic = MAGIC_ALG;
    alg->id        = alg_id;
    alg->hmac      = flags & BCRYPT_ALG_HANDLE_HMAC_FLAG;

    *handle = alg;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptCloseAlgorithmProvider( BCRYPT_ALG_HANDLE handle, DWORD flags )
{
    struct algorithm *alg = handle;

    TRACE( "%p, %08x\n", handle, flags );

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;
    HeapFree( GetProcessHeap(), 0, alg );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptGetFipsAlgorithmMode(BOOLEAN *enabled)
{
    FIXME("%p - semi-stub\n", enabled);

    if (!enabled)
        return STATUS_INVALID_PARAMETER;

    *enabled = FALSE;
    return STATUS_SUCCESS;
}

struct hash_impl
{
    union
    {
        MD5_CTX md5;
        SHA_CTX sha1;
        SHA256_CTX sha256;
        SHA512_CTX sha512;
    } u;
};

static NTSTATUS hash_init( struct hash_impl *hash, enum alg_id alg_id )
{
    switch (alg_id)
    {
    case ALG_ID_MD5:
        MD5Init( &hash->u.md5 );
        break;

    case ALG_ID_SHA1:
        A_SHAInit( &hash->u.sha1 );
        break;

    case ALG_ID_SHA256:
        sha256_init( &hash->u.sha256 );
        break;

    case ALG_ID_SHA384:
        sha384_init( &hash->u.sha512 );
        break;

    case ALG_ID_SHA512:
        sha512_init( &hash->u.sha512 );
        break;

    default:
        ERR( "unhandled id %u\n", alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS hash_update( struct hash_impl *hash, enum alg_id alg_id,
                             UCHAR *input, ULONG size )
{
    switch (alg_id)
    {
    case ALG_ID_MD5:
        MD5Update( &hash->u.md5, input, size );
        break;

    case ALG_ID_SHA1:
        A_SHAUpdate( &hash->u.sha1, input, size );
        break;

    case ALG_ID_SHA256:
        sha256_update( &hash->u.sha256, input, size );
        break;

    case ALG_ID_SHA384:
        sha384_update( &hash->u.sha512, input, size );
        break;

    case ALG_ID_SHA512:
        sha512_update( &hash->u.sha512, input, size );
        break;

    default:
        ERR( "unhandled id %u\n", alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS hash_finish( struct hash_impl *hash, enum alg_id alg_id,
                             UCHAR *output, ULONG size )
{
    switch (alg_id)
    {
    case ALG_ID_MD5:
        MD5Final( &hash->u.md5 );
        memcpy( output, hash->u.md5.digest, 16 );
        break;

    case ALG_ID_SHA1:
        A_SHAFinal( &hash->u.sha1, (ULONG *)output );
        break;

    case ALG_ID_SHA256:
        sha256_finalize( &hash->u.sha256, output );
        break;

    case ALG_ID_SHA384:
        sha384_finalize( &hash->u.sha512, output );
        break;

    case ALG_ID_SHA512:
        sha512_finalize( &hash->u.sha512, output );
        break;

    default:
        ERR( "unhandled id %u\n", alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
    return STATUS_SUCCESS;
}

struct hash
{
    struct object    hdr;
    enum alg_id      alg_id;
    BOOL             hmac;
    struct hash_impl outer;
    struct hash_impl inner;
};

static NTSTATUS generic_alg_property( enum alg_id id, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    if (!strcmpW( prop, BCRYPT_OBJECT_LENGTH ))
    {
        if (!alg_props[id].object_length)
            return STATUS_NOT_SUPPORTED;
        *ret_size = sizeof(ULONG);
        if (size < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;
        if (buf)
            *(ULONG *)buf = alg_props[id].object_length;
        return STATUS_SUCCESS;
    }

    if (!strcmpW( prop, BCRYPT_HASH_LENGTH ))
    {
        if (!alg_props[id].hash_length)
            return STATUS_NOT_SUPPORTED;
        *ret_size = sizeof(ULONG);
        if (size < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;
        if(buf)
            *(ULONG*)buf = alg_props[id].hash_length;
        return STATUS_SUCCESS;
    }

    if (!strcmpW( prop, BCRYPT_ALGORITHM_NAME ))
    {
        *ret_size = (strlenW(alg_props[id].alg_name)+1)*sizeof(WCHAR);
        if (size < *ret_size)
            return STATUS_BUFFER_TOO_SMALL;
        if(buf)
            memcpy(buf, alg_props[id].alg_name, *ret_size);
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_alg_property( enum alg_id id, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    NTSTATUS status;

    status = generic_alg_property( id, prop, buf, size, ret_size );
    if (status == STATUS_NOT_IMPLEMENTED)
        FIXME( "unsupported property %s\n", debugstr_w(prop) );
    return status;
}

static NTSTATUS get_hash_property( enum alg_id id, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    NTSTATUS status;

    status = generic_alg_property( id, prop, buf, size, ret_size );
    if (status == STATUS_NOT_IMPLEMENTED)
        FIXME( "unsupported property %s\n", debugstr_w(prop) );
    return status;
}

NTSTATUS WINAPI BCryptGetProperty( BCRYPT_HANDLE handle, LPCWSTR prop, UCHAR *buffer, ULONG count, ULONG *res, ULONG flags )
{
    struct object *object = handle;

    TRACE( "%p, %s, %p, %u, %p, %08x\n", handle, wine_dbgstr_w(prop), buffer, count, res, flags );

    if (!object) return STATUS_INVALID_HANDLE;
    if (!prop || !res) return STATUS_INVALID_PARAMETER;

    switch (object->magic)
    {
    case MAGIC_ALG:
    {
        const struct algorithm *alg = (const struct algorithm *)object;
        return get_alg_property( alg->id, prop, buffer, count, res );
    }
    case MAGIC_HASH:
    {
        const struct hash *hash = (const struct hash *)object;
        return get_hash_property( hash->alg_id, prop, buffer, count, res );
    }
    default:
        WARN( "unknown magic %08x\n", object->magic );
        return STATUS_INVALID_HANDLE;
    }
}

NTSTATUS WINAPI BCryptCreateHash( BCRYPT_ALG_HANDLE algorithm, BCRYPT_HASH_HANDLE *handle, UCHAR *object, ULONG objectlen,
                                  UCHAR *secret, ULONG secretlen, ULONG flags )
{
    struct algorithm *alg = algorithm;
    UCHAR buffer[MAX_HASH_BLOCK_BITS / 8] = {0};
    struct hash *hash;
    int block_bytes;
    NTSTATUS status;
    int i;

    TRACE( "%p, %p, %p, %u, %p, %u, %08x - stub\n", algorithm, handle, object, objectlen,
           secret, secretlen, flags );
    if (flags)
    {
        FIXME( "unimplemented flags %08x\n", flags );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;
    if (object) FIXME( "ignoring object buffer\n" );

    if (!(hash = HeapAlloc( GetProcessHeap(), 0, sizeof(*hash) ))) return STATUS_NO_MEMORY;
    hash->hdr.magic = MAGIC_HASH;
    hash->alg_id    = alg->id;
    hash->hmac      = alg->hmac;

    /* initialize hash */
    if ((status = hash_init( &hash->inner, hash->alg_id ))) goto end;
    if (!hash->hmac) goto end;

    /* initialize hmac */
    if ((status = hash_init( &hash->outer, hash->alg_id ))) goto end;
    block_bytes = alg_props[hash->alg_id].block_bits / 8;
    if (secretlen > block_bytes)
    {
        struct hash_impl temp;
        if ((status = hash_init( &temp, hash->alg_id ))) goto end;
        if ((status = hash_update( &temp, hash->alg_id, secret, secretlen ))) goto end;
        if ((status = hash_finish( &temp, hash->alg_id, buffer,
                                   alg_props[hash->alg_id].hash_length ))) goto end;
    }
    else
    {
        memcpy( buffer, secret, secretlen );
    }
    for (i = 0; i < block_bytes; i++) buffer[i] ^= 0x5c;
    if ((status = hash_update( &hash->outer, hash->alg_id, buffer, block_bytes ))) goto end;
    for (i = 0; i < block_bytes; i++) buffer[i] ^= (0x5c ^ 0x36);
    status = hash_update( &hash->inner, hash->alg_id, buffer, block_bytes );

end:
    if (status != STATUS_SUCCESS)
    {
        HeapFree( GetProcessHeap(), 0, hash );
        return status;
    }

    *handle = hash;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptDuplicateHash( BCRYPT_HASH_HANDLE handle, BCRYPT_HASH_HANDLE *handle_copy,
                                     UCHAR *object, ULONG objectlen, ULONG flags )
{
    struct hash *hash_orig = handle;
    struct hash *hash_copy;

    TRACE( "%p, %p, %p, %u, %u\n", handle, handle_copy, object, objectlen, flags );

    if (!hash_orig || hash_orig->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    if (!handle_copy) return STATUS_INVALID_PARAMETER;
    if (object) FIXME( "ignoring object buffer\n" );

    if (!(hash_copy = HeapAlloc( GetProcessHeap(), 0, sizeof(*hash_copy) )))
        return STATUS_NO_MEMORY;

    memcpy( hash_copy, hash_orig, sizeof(*hash_orig) );

    *handle_copy = hash_copy;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptDestroyHash( BCRYPT_HASH_HANDLE handle )
{
    struct hash *hash = handle;

    TRACE( "%p\n", handle );

    if (!hash || hash->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    HeapFree( GetProcessHeap(), 0, hash );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptHashData( BCRYPT_HASH_HANDLE handle, UCHAR *input, ULONG size, ULONG flags )
{
    struct hash *hash = handle;

    TRACE( "%p, %p, %u, %08x\n", handle, input, size, flags );

    if (!hash || hash->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    if (!input) return STATUS_SUCCESS;

    return hash_update( &hash->inner, hash->alg_id, input, size );
}

NTSTATUS WINAPI BCryptFinishHash( BCRYPT_HASH_HANDLE handle, UCHAR *output, ULONG size, ULONG flags )
{
    UCHAR buffer[MAX_HASH_OUTPUT_BYTES];
    struct hash *hash = handle;
    NTSTATUS status;
    int hash_length;

    TRACE( "%p, %p, %u, %08x\n", handle, output, size, flags );

    if (!hash || hash->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    if (!output) return STATUS_INVALID_PARAMETER;

    if (!hash->hmac)
        return hash_finish( &hash->inner, hash->alg_id, output, size );

    hash_length = alg_props[hash->alg_id].hash_length;
    if ((status = hash_finish( &hash->inner, hash->alg_id, buffer, hash_length ))) return status;
    if ((status = hash_update( &hash->outer, hash->alg_id, buffer, hash_length ))) return status;
    return hash_finish( &hash->outer, hash->alg_id, output, size );
}

NTSTATUS WINAPI BCryptHash( BCRYPT_ALG_HANDLE algorithm, UCHAR *secret, ULONG secretlen,
                            UCHAR *input, ULONG inputlen, UCHAR *output, ULONG outputlen )
{
    NTSTATUS status;
    BCRYPT_HASH_HANDLE handle;

    TRACE( "%p, %p, %u, %p, %u, %p, %u\n", algorithm, secret, secretlen,
           input, inputlen, output, outputlen );

    status = BCryptCreateHash( algorithm, &handle, NULL, 0, secret, secretlen, 0);
    if (status != STATUS_SUCCESS)
    {
        return status;
    }

    status = BCryptHashData( handle, input, inputlen, 0 );
    if (status != STATUS_SUCCESS)
    {
        BCryptDestroyHash( handle );
        return status;
    }

    status = BCryptFinishHash( handle, output, outputlen, 0 );
    if (status != STATUS_SUCCESS)
    {
        BCryptDestroyHash( handle );
        return status;
    }

    return BCryptDestroyHash( handle );
}

NTSTATUS WINAPI BCryptImportKeyPair(BCRYPT_ALG_HANDLE algorithm, BCRYPT_KEY_HANDLE import_key,
                                    LPCWSTR blob_type, BCRYPT_KEY_HANDLE *key,
                                    UCHAR *input, ULONG input_len, DWORD flags)
{
    struct algorithm *alg = (struct algorithm*)algorithm;

    FIXME("%p, %p, %s, %p, %p, %u, %08x - semi-stub\n", algorithm, import_key, wine_dbgstr_w(blob_type), key, input, input_len, flags);

    if (!pgnutls_pubkey_import_rsa_raw)
        return STATUS_NOT_IMPLEMENTED;

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;

    if (!key)
        return STATUS_INVALID_PARAMETER;
    if (!input)
        return STATUS_INVALID_PARAMETER;

    *key = NULL;

    if (alg->id == ALG_ID_RSA)
    {
        if (lstrcmpW(BCRYPT_RSAPUBLIC_BLOB, blob_type) == 0)
        {
            int ret;

            gnutls_pubkey_t pubkey;
            gnutls_datum_t exp;
            gnutls_datum_t modulus;
            BCRYPT_RSAKEY_BLOB *rsakey_blob = (void*)input;

            if (rsakey_blob->Magic != BCRYPT_RSAPUBLIC_MAGIC)
            {
                ERR("wrong magic=%u, line=%d\n", rsakey_blob->Magic, __LINE__);
                return STATUS_INVALID_PARAMETER;
            }

            exp.data = input + sizeof(*rsakey_blob),
            exp.size = rsakey_blob->cbPublicExp;

            modulus.data = input + sizeof(*rsakey_blob) + rsakey_blob->cbPublicExp;
            modulus.size = rsakey_blob->cbModulus;

            ret = pgnutls_pubkey_init(&pubkey);
            if (ret < 0)
            {
                ERR("gnutls_pubkey_init failed, ret=%d\n", ret);
                return STATUS_UNSUCCESSFUL;
            }

            ret = pgnutls_pubkey_import_rsa_raw(pubkey, &modulus, &exp);
            if (ret < 0)
            {
                pgnutls_pubkey_deinit(pubkey);
                ERR("gnutls_pubkey_import_rsa_raw failed, ret=%d\n", ret);
                return STATUS_UNSUCCESSFUL;
            }

            *key = pubkey;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS WINAPI BCryptVerifySignature(BCRYPT_KEY_HANDLE key, void* padding_info, UCHAR *hash, ULONG hash_len,
                                      UCHAR *signature, ULONG signature_len, ULONG flags)
{
    gnutls_pubkey_t pubkey = (gnutls_pubkey_t)key;
    gnutls_digest_algorithm_t hash_algo = GNUTLS_DIG_UNKNOWN;
    gnutls_sign_algorithm_t sign_algo;
    gnutls_datum_t hash_data;
    gnutls_datum_t signature_data;
    int ret;

    FIXME("%p, %p, %p, %u, %p, %u, %08x - semi-stub\n", key, padding_info, hash, hash_len, signature, signature_len, flags);

    if (!pgnutls_pubkey_verify_hash2)
        return STATUS_NOT_IMPLEMENTED;

    if (!key)
        return STATUS_INVALID_PARAMETER;
    if (!padding_info)
        return STATUS_INVALID_PARAMETER;
    if (!hash)
        return STATUS_INVALID_PARAMETER;
    if (!hash_len)
        return STATUS_INVALID_PARAMETER;
    if (!signature)
        return STATUS_INVALID_PARAMETER;
    if (!signature_len)
        return STATUS_INVALID_PARAMETER;

    if (flags & BCRYPT_PAD_PKCS1)
    {
        BCRYPT_PKCS1_PADDING_INFO *p = (BCRYPT_PKCS1_PADDING_INFO*)padding_info;

        if (lstrcmpW(BCRYPT_SHA1_ALGORITHM, p->pszAlgId) == 0)
            hash_algo = GNUTLS_DIG_SHA1;
    }

    if (hash_algo == GNUTLS_DIG_UNKNOWN)
        return STATUS_INVALID_PARAMETER;

    hash_data.data = (void*)hash;
    hash_data.size = hash_len;

    signature_data.data = (void*)signature;
    signature_data.size = signature_len;

    sign_algo = pgnutls_pk_to_sign(pgnutls_pubkey_get_pk_algorithm (pubkey, NULL), hash_algo);

    ret = pgnutls_pubkey_verify_hash2(pubkey, sign_algo, 0, &hash_data, &signature_data);
    if (ret < 0)
    {
        ERR("gnutls_pubkey_verify_hash2 failed, ret=%d\n", ret);
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptDestroyKey(BCRYPT_KEY_HANDLE key)
{
    gnutls_pubkey_t pubkey = (gnutls_pubkey_t)key;

    FIXME("%p - semi-stub\n", key);

    if (!pgnutls_pubkey_deinit)
        return STATUS_NOT_IMPLEMENTED;

    if (!key)
        return STATUS_INVALID_PARAMETER;

    pgnutls_pubkey_deinit(pubkey);

    return STATUS_SUCCESS;
}

BOOL WINAPI DllMain( HINSTANCE hinst, DWORD reason, LPVOID reserved )
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        instance = hinst;
        DisableThreadLibraryCalls( hinst );
#if defined(HAVE_GNUTLS_HASH) && !defined(HAVE_COMMONCRYPTO_COMMONDIGEST_H)
        gnutls_initialize();
#endif
        break;

    case DLL_PROCESS_DETACH:
        if (reserved) break;
#if defined(HAVE_GNUTLS_HASH) && !defined(HAVE_COMMONCRYPTO_COMMONDIGEST_H)
        gnutls_uninitialize();
#endif
        break;
    }
    return TRUE;
}
