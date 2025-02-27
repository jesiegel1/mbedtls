/*
 *  TLS 1.3 client-side functions
 *
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of mbed TLS ( https://tls.mbed.org )
 */

#include "common.h"

#if defined(MBEDTLS_SSL_CLI_C) && defined(MBEDTLS_SSL_PROTO_TLS1_3)

#include <string.h>

#include "mbedtls/debug.h"
#include "mbedtls/error.h"

#include "ssl_misc.h"
#include "ssl_client.h"
#include "ssl_tls13_keys.h"
#include "ssl_debug_helpers.h"

#if defined(MBEDTLS_SSL_USE_MPS)
#include "mps_all.h"
#endif /* MBEDTLS_SSL_USE_MPS */

#if defined(MBEDTLS_PLATFORM_C)
#include "mbedtls/platform.h"
#else
#include <stdlib.h>
#define mbedtls_calloc    calloc
#define mbedtls_free       free
#endif

/* Write extensions */

/*
 * ssl_tls13_write_supported_versions_ext():
 *
 * struct {
 *      ProtocolVersion versions<2..254>;
 * } SupportedVersions;
 */
static int ssl_tls13_write_supported_versions_ext( mbedtls_ssl_context *ssl,
                                                   unsigned char *buf,
                                                   unsigned char *end,
                                                   size_t *out_len )
{
    unsigned char *p = buf;
    unsigned char versions_len = ( ssl->handshake->min_tls_version <=
                                   MBEDTLS_SSL_VERSION_TLS1_2 ) ? 4 : 2;

    *out_len = 0;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "client hello, adding supported versions extension" ) );

    /* Check if we have space to write the extension:
     * - extension_type         (2 bytes)
     * - extension_data_length  (2 bytes)
     * - versions_length        (1 byte )
     * - versions               (2 or 4 bytes)
     */
    MBEDTLS_SSL_CHK_BUF_PTR( p, end, 5 + versions_len );

    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_SUPPORTED_VERSIONS, p, 0 );
    MBEDTLS_PUT_UINT16_BE( versions_len + 1, p, 2 );
    p += 4;

    /* Length of versions */
    *p++ = versions_len;

    /* Write values of supported versions.
     * They are defined by the configuration.
     * Currently, we advertise only TLS 1.3 or both TLS 1.3 and TLS 1.2.
     */
    mbedtls_ssl_write_version( p, MBEDTLS_SSL_TRANSPORT_STREAM,
                               MBEDTLS_SSL_VERSION_TLS1_3 );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "supported version: [3:4]" ) );


    if( ssl->handshake->min_tls_version <= MBEDTLS_SSL_VERSION_TLS1_2 )
    {
        mbedtls_ssl_write_version( p + 2, MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_VERSION_TLS1_2 );
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "supported version: [3:3]" ) );
    }

    *out_len = 5 + versions_len;

    return( 0 );
}

static int ssl_tls13_parse_supported_versions_ext( mbedtls_ssl_context *ssl,
                                                   const unsigned char *buf,
                                                   const unsigned char *end )
{
    ((void) ssl);

    MBEDTLS_SSL_CHK_BUF_READ_PTR( buf, end, 2 );
    if( mbedtls_ssl_read_version( buf, ssl->conf->transport ) !=
          MBEDTLS_SSL_VERSION_TLS1_3 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "unexpected version" ) );

        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                                      MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER);
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    if( &buf[2] != end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "supported_versions ext data length incorrect" ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR,
                                      MBEDTLS_ERR_SSL_DECODE_ERROR );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)
    /* For ticket handling, we need to populate the version
     * and the endpoint information into the session structure
     * since only session information is available in that API.
     */
    ssl->session_negotiate->tls_version = ssl->tls_version;
    ssl->session_negotiate->endpoint = ssl->conf->endpoint;
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */

    return( 0 );
}

#if defined(MBEDTLS_SSL_ALPN)
static int ssl_tls13_parse_alpn_ext( mbedtls_ssl_context *ssl,
                               const unsigned char *buf, size_t len )
{
    size_t list_len, name_len;
    const unsigned char *p = buf;
    const unsigned char *end = buf + len;

    /* If we didn't send it, the server shouldn't send it */
    if( ssl->conf->alpn_list == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    /*
     * opaque ProtocolName<1..2^8-1>;
     *
     * struct {
     *     ProtocolName protocol_name_list<2..2^16-1>
     * } ProtocolNameList;
     *
     * the "ProtocolNameList" MUST contain exactly one "ProtocolName"
     */

    /* Min length is 2 ( list_len ) + 1 ( name_len ) + 1 ( name ) */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 4 );

    list_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, list_len );

    name_len = *p++;
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, list_len - 1 );

    /* Check that the server chosen protocol was in our list and save it */
    for ( const char **alpn = ssl->conf->alpn_list; *alpn != NULL; alpn++ )
    {
        if( name_len == strlen( *alpn ) &&
            memcmp( buf + 3, *alpn, name_len ) == 0 )
        {
            ssl->alpn_chosen = *alpn;
            return( 0 );
        }
    }

    return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
}
#endif /* MBEDTLS_SSL_ALPN */

static int ssl_tls13_reset_key_share( mbedtls_ssl_context *ssl )
{
    uint16_t group_id = ssl->handshake->offered_group_id;

    if( group_id == 0 )
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

#if defined(MBEDTLS_ECDH_C)
    if( mbedtls_ssl_tls13_named_group_is_ecdhe( group_id ) )
    {
        int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
        psa_status_t status = PSA_ERROR_CORRUPTION_DETECTED;

        /* Destroy generated private key. */
        status = psa_destroy_key( ssl->handshake->ecdh_psa_privkey );
        if( status != PSA_SUCCESS )
        {
            ret = psa_ssl_status_to_mbedtls( status );
            MBEDTLS_SSL_DEBUG_RET( 1, "psa_destroy_key", ret );
            return( ret );
        }

        ssl->handshake->ecdh_psa_privkey = MBEDTLS_SVC_KEY_ID_INIT;
        return( 0 );
    }
    else
#endif /* MBEDTLS_ECDH_C */
    if( 0 /* other KEMs? */ )
    {
        /* Do something */
    }

    return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
}

/*
 * Functions for writing key_share extension.
 */
#if defined(MBEDTLS_ECDH_C)
static int ssl_tls13_generate_and_write_ecdh_key_exchange(
                mbedtls_ssl_context *ssl,
                uint16_t named_group,
                unsigned char *buf,
                unsigned char *end,
                size_t *out_len )
{
    psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    int ret = MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;
    psa_key_attributes_t key_attributes;
    size_t own_pubkey_len;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;
    size_t ecdh_bits = 0;

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Perform PSA-based ECDH computation." ) );

    /* Convert EC group to PSA key type. */
    if( ( handshake->ecdh_psa_type =
        mbedtls_psa_parse_tls_ecc_group( named_group, &ecdh_bits ) ) == 0 )
            return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );

    ssl->handshake->ecdh_bits = ecdh_bits;

    key_attributes = psa_key_attributes_init();
    psa_set_key_usage_flags( &key_attributes, PSA_KEY_USAGE_DERIVE );
    psa_set_key_algorithm( &key_attributes, PSA_ALG_ECDH );
    psa_set_key_type( &key_attributes, handshake->ecdh_psa_type );
    psa_set_key_bits( &key_attributes, handshake->ecdh_bits );

    /* Generate ECDH private key. */
    status = psa_generate_key( &key_attributes,
                                &handshake->ecdh_psa_privkey );
    if( status != PSA_SUCCESS )
    {
        ret = psa_ssl_status_to_mbedtls( status );
        MBEDTLS_SSL_DEBUG_RET( 1, "psa_generate_key", ret );
        return( ret );

    }

    /* Export the public part of the ECDH private key from PSA. */
    status = psa_export_public_key( handshake->ecdh_psa_privkey,
                                    buf, (size_t)( end - buf ),
                                    &own_pubkey_len );
    if( status != PSA_SUCCESS )
    {
        ret = psa_ssl_status_to_mbedtls( status );
        MBEDTLS_SSL_DEBUG_RET( 1, "psa_export_public_key", ret );
        return( ret );

    }

    *out_len = own_pubkey_len;

    return( 0 );
}
#endif /* MBEDTLS_ECDH_C */

static int ssl_tls13_get_default_group_id( mbedtls_ssl_context *ssl,
                                           uint16_t *group_id )
{
    int ret = MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;


#if defined(MBEDTLS_ECDH_C)
    const uint16_t *group_list = mbedtls_ssl_get_groups( ssl );
    /* Pick first available ECDHE group compatible with TLS 1.3 */
    if( group_list == NULL )
        return( MBEDTLS_ERR_SSL_BAD_CONFIG );

    for ( ; *group_list != 0; group_list++ )
    {
        const mbedtls_ecp_curve_info *curve_info;
        curve_info = mbedtls_ecp_curve_info_from_tls_id( *group_list );
        if( curve_info != NULL &&
            mbedtls_ssl_tls13_named_group_is_ecdhe( *group_list ) )
        {
            *group_id = *group_list;
            return( 0 );
        }
    }
#else
    ((void) ssl);
    ((void) group_id);
#endif /* MBEDTLS_ECDH_C */

    /*
     * Add DHE named groups here.
     * Pick first available DHE group compatible with TLS 1.3
     */

    return( ret );
}

/*
 * ssl_tls13_write_key_share_ext
 *
 * Structure of key_share extension in ClientHello:
 *
 *  struct {
 *          NamedGroup group;
 *          opaque key_exchange<1..2^16-1>;
 *      } KeyShareEntry;
 *  struct {
 *          KeyShareEntry client_shares<0..2^16-1>;
 *      } KeyShareClientHello;
 */
static int ssl_tls13_write_key_share_ext( mbedtls_ssl_context *ssl,
                                          unsigned char *buf,
                                          unsigned char *end,
                                          size_t *out_len )
{
    unsigned char *p = buf;
    unsigned char *client_shares; /* Start of client_shares */
    size_t client_shares_len;     /* Length of client_shares */
    uint16_t group_id;
    int ret = MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;

    *out_len = 0;

    /* Check if we have space for header and length fields:
     * - extension_type         (2 bytes)
     * - extension_data_length  (2 bytes)
     * - client_shares_length   (2 bytes)
     */
    MBEDTLS_SSL_CHK_BUF_PTR( p, end, 6 );
    p += 6;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "client hello: adding key share extension" ) );

    /* HRR could already have requested something else. */
    group_id = ssl->handshake->offered_group_id;
    if( !mbedtls_ssl_tls13_named_group_is_ecdhe( group_id ) &&
        !mbedtls_ssl_tls13_named_group_is_dhe( group_id ) )
    {
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_get_default_group_id( ssl,
                                                              &group_id ) );
    }

    /*
     * Dispatch to type-specific key generation function.
     *
     * So far, we're only supporting ECDHE. With the introduction
     * of PQC KEMs, we'll want to have multiple branches, one per
     * type of KEM, and dispatch to the corresponding crypto. And
     * only one key share entry is allowed.
     */
    client_shares = p;
#if defined(MBEDTLS_ECDH_C)
    if( mbedtls_ssl_tls13_named_group_is_ecdhe( group_id ) )
    {
        /* Pointer to group */
        unsigned char *group = p;
        /* Length of key_exchange */
        size_t key_exchange_len = 0;

        /* Check there is space for header of KeyShareEntry
         * - group                  (2 bytes)
         * - key_exchange_length    (2 bytes)
         */
        MBEDTLS_SSL_CHK_BUF_PTR( p, end, 4 );
        p += 4;
        ret = ssl_tls13_generate_and_write_ecdh_key_exchange( ssl, group_id, p, end,
                                                              &key_exchange_len );
        p += key_exchange_len;
        if( ret != 0 )
            return( ret );

        /* Write group */
        MBEDTLS_PUT_UINT16_BE( group_id, group, 0 );
        /* Write key_exchange_length */
        MBEDTLS_PUT_UINT16_BE( key_exchange_len, group, 2 );
    }
    else
#endif /* MBEDTLS_ECDH_C */
    if( 0 /* other KEMs? */ )
    {
        /* Do something */
    }
    else
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

    /* Length of client_shares */
    client_shares_len = p - client_shares;
    if( client_shares_len == 0)
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "No key share defined." ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }
    /* Write extension_type */
    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_KEY_SHARE, buf, 0 );
    /* Write extension_data_length */
    MBEDTLS_PUT_UINT16_BE( client_shares_len + 2, buf, 2 );
    /* Write client_shares_length */
    MBEDTLS_PUT_UINT16_BE( client_shares_len, buf, 4 );

    /* Update offered_group_id field */
    ssl->handshake->offered_group_id = group_id;

    /* Output the total length of key_share extension. */
    *out_len = p - buf;

    MBEDTLS_SSL_DEBUG_BUF( 3, "client hello, key_share extension", buf, *out_len );

    ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_KEY_SHARE;

cleanup:

    return( ret );
}

#if defined(MBEDTLS_ECDH_C)

static int ssl_tls13_read_public_ecdhe_share( mbedtls_ssl_context *ssl,
                                              const unsigned char *buf,
                                              size_t buf_len )
{
    uint8_t *p = (uint8_t*)buf;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;

    /* Get size of the TLS opaque key_exchange field of the KeyShareEntry struct. */
    uint16_t peerkey_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    /* Check if key size is consistent with given buffer length. */
    if ( peerkey_len > ( buf_len - 2 ) )
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );

    /* Store peer's ECDH public key. */
    memcpy( handshake->ecdh_psa_peerkey, p, peerkey_len );
    handshake->ecdh_psa_peerkey_len = peerkey_len;

    return( 0 );
}
#endif /* MBEDTLS_ECDH_C */

/*
 * ssl_tls13_parse_hrr_key_share_ext()
 *      Parse key_share extension in Hello Retry Request
 *
 * struct {
 *        NamedGroup selected_group;
 * } KeyShareHelloRetryRequest;
 */
static int ssl_tls13_parse_hrr_key_share_ext( mbedtls_ssl_context *ssl,
                                              const unsigned char *buf,
                                              const unsigned char *end )
{
    const mbedtls_ecp_curve_info *curve_info = NULL;
    const unsigned char *p = buf;
    int selected_group;
    int found = 0;

    const uint16_t *group_list = mbedtls_ssl_get_groups( ssl );
    if( group_list == NULL )
        return( MBEDTLS_ERR_SSL_BAD_CONFIG );

    MBEDTLS_SSL_DEBUG_BUF( 3, "key_share extension", p, end - buf );

    /* Read selected_group */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    selected_group = MBEDTLS_GET_UINT16_BE( p, 0 );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "selected_group ( %d )", selected_group ) );

    /* Upon receipt of this extension in a HelloRetryRequest, the client
     * MUST first verify that the selected_group field corresponds to a
     * group which was provided in the "supported_groups" extension in the
     * original ClientHello.
     * The supported_group was based on the info in ssl->conf->group_list.
     *
     * If the server provided a key share that was not sent in the ClientHello
     * then the client MUST abort the handshake with an "illegal_parameter" alert.
     */
    for( ; *group_list != 0; group_list++ )
    {
        curve_info = mbedtls_ecp_curve_info_from_tls_id( *group_list );
        if( curve_info == NULL || curve_info->tls_id != selected_group )
            continue;

        /* We found a match */
        found = 1;
        break;
    }

    /* Client MUST verify that the selected_group field does not
     * correspond to a group which was provided in the "key_share"
     * extension in the original ClientHello. If the server sent an
     * HRR message with a key share already provided in the
     * ClientHello then the client MUST abort the handshake with
     * an "illegal_parameter" alert.
     */
    if( found == 0 || selected_group == ssl->handshake->offered_group_id )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Invalid key share in HRR" ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT(
                MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    /* Remember server's preference for next ClientHello */
    ssl->handshake->offered_group_id = selected_group;

    return( 0 );
}

/*
 * ssl_tls13_parse_key_share_ext()
 *      Parse key_share extension in Server Hello
 *
 * struct {
 *        KeyShareEntry server_share;
 * } KeyShareServerHello;
 * struct {
 *        NamedGroup group;
 *        opaque key_exchange<1..2^16-1>;
 * } KeyShareEntry;
 */
static int ssl_tls13_parse_key_share_ext( mbedtls_ssl_context *ssl,
                                          const unsigned char *buf,
                                          const unsigned char *end )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    const unsigned char *p = buf;
    uint16_t group, offered_group;

    /* ...
     * NamedGroup group; (2 bytes)
     * ...
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    group = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    /* Check that the chosen group matches the one we offered. */
    offered_group = ssl->handshake->offered_group_id;
    if( offered_group != group )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1,
            ( "Invalid server key share, our group %u, their group %u",
              (unsigned) offered_group, (unsigned) group ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE,
                                      MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
        return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
    }

#if defined(MBEDTLS_ECDH_C)
    if( mbedtls_ssl_tls13_named_group_is_ecdhe( group ) )
    {
        const mbedtls_ecp_curve_info *curve_info =
            mbedtls_ecp_curve_info_from_tls_id( group );
        if( curve_info == NULL )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Invalid TLS curve group id" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        MBEDTLS_SSL_DEBUG_MSG( 2, ( "ECDH curve: %s", curve_info->name ) );

        ret = ssl_tls13_read_public_ecdhe_share( ssl, p, end - p );
        if( ret != 0 )
            return( ret );
    }
    else
#endif /* MBEDTLS_ECDH_C */
    if( 0 /* other KEMs? */ )
    {
        /* Do something */
    }
    else
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

    ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_KEY_SHARE;
    return( ret );
}

/*
 * ssl_tls13_parse_cookie_ext()
 *      Parse cookie extension in Hello Retry Request
 *
 * struct {
 *        opaque cookie<1..2^16-1>;
 * } Cookie;
 *
 * When sending a HelloRetryRequest, the server MAY provide a "cookie"
 * extension to the client (this is an exception to the usual rule that
 * the only extensions that may be sent are those that appear in the
 * ClientHello).  When sending the new ClientHello, the client MUST copy
 * the contents of the extension received in the HelloRetryRequest into
 * a "cookie" extension in the new ClientHello.  Clients MUST NOT use
 * cookies in their initial ClientHello in subsequent connections.
 */
static int ssl_tls13_parse_cookie_ext( mbedtls_ssl_context *ssl,
                                       const unsigned char *buf,
                                       const unsigned char *end )
{
    uint16_t cookie_len;
    const unsigned char *p = buf;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;

    /* Retrieve length field of cookie */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    cookie_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, cookie_len );
    MBEDTLS_SSL_DEBUG_BUF( 3, "cookie extension", p, cookie_len );

    mbedtls_free( handshake->cookie );
    handshake->hrr_cookie_len = 0;
    handshake->cookie = mbedtls_calloc( 1, cookie_len );
    if( handshake->cookie == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1,
                ( "alloc failed ( %ud bytes )",
                  cookie_len ) );
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    memcpy( handshake->cookie, p, cookie_len );
    handshake->hrr_cookie_len = cookie_len;

    return( 0 );
}

static int ssl_tls13_write_cookie_ext( mbedtls_ssl_context *ssl,
                                       unsigned char *buf,
                                       unsigned char *end,
                                       size_t *out_len )
{
    unsigned char *p = buf;
    *out_len = 0;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;

    if( handshake->cookie == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "no cookie to send; skip extension" ) );
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_BUF( 3, "client hello, cookie",
                           handshake->cookie,
                           handshake->hrr_cookie_len );

    MBEDTLS_SSL_CHK_BUF_PTR( p, end, handshake->hrr_cookie_len + 6 );

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "client hello, adding cookie extension" ) );

    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_COOKIE, p, 0 );
    MBEDTLS_PUT_UINT16_BE( handshake->hrr_cookie_len + 2, p, 2 );
    MBEDTLS_PUT_UINT16_BE( handshake->hrr_cookie_len, p, 4 );
    p += 6;

    /* Cookie */
    memcpy( p, handshake->cookie, handshake->hrr_cookie_len );

    *out_len = handshake->hrr_cookie_len + 6;

    return( 0 );
}

#if defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)
/*
 * ssl_tls13_write_psk_key_exchange_modes_ext() structure:
 *
 * enum { psk_ke( 0 ), psk_dhe_ke( 1 ), ( 255 ) } PskKeyExchangeMode;
 *
 * struct {
 *     PskKeyExchangeMode ke_modes<1..255>;
 * } PskKeyExchangeModes;
 */

static int ssl_tls13_write_psk_key_exchange_modes_ext( mbedtls_ssl_context *ssl,
                                                       unsigned char *buf,
                                                       unsigned char *end,
                                                       size_t *out_len )
{
    const unsigned char *psk;
    size_t psk_len;
    const unsigned char *psk_identity;
    size_t psk_identity_len;
    unsigned char *p;
    int num_modes = 0;

    /* Skip writing extension if no PSK key exchange mode
     * is enabled in the config. */
    if( !mbedtls_ssl_conf_tls13_some_psk_enabled( ssl ) )
    {
        *out_len = 0;
        return( 0 );
    }

    if( mbedtls_ssl_get_psk_to_offer( ssl, &psk, &psk_len,
                                      &psk_identity, &psk_identity_len ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "skip psk_key_exchange_modes extension" ) );
        return( 0 );
    }

    /* Require 7 bytes of data, otherwise fail, even if extension might be shorter. */
    if( (size_t)( end - buf ) < 7 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Not enough buffer" ) );
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "client hello, adding psk_key_exchange_modes extension" ) );

    /* Extension Type */
    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_PSK_KEY_EXCHANGE_MODES, buf, 0 );

    /* Skip extension length (2 byte) and PSK mode list length (1 byte) for now. */
    p = buf + 5;

    if( mbedtls_ssl_conf_tls13_psk_enabled( ssl ) )
    {
        *p++ = MBEDTLS_SSL_TLS1_3_PSK_MODE_PURE;
        num_modes++;

        MBEDTLS_SSL_DEBUG_MSG( 4, ( "Adding pure PSK key exchange mode" ) );
    }

    if( mbedtls_ssl_conf_tls13_psk_ephemeral_enabled( ssl ) )
    {
        *p++ = MBEDTLS_SSL_TLS1_3_PSK_MODE_ECDHE;
        num_modes++;

        MBEDTLS_SSL_DEBUG_MSG( 4, ( "Adding PSK-ECDHE key exchange mode" ) );
    }

    /* Add extension length: PSK mode list length byte + actual PSK mode list length */
    buf[2] = 0;
    buf[3] = num_modes + 1;
    /* Add PSK mode list length */
    buf[4] = num_modes;

    *out_len = p - buf;
    ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_PSK_KEY_EXCHANGE_MODES;
    return ( 0 );
}
#endif /* MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED */

/*
 * mbedtls_ssl_tls13_write_pre_shared_key_ext() structure:
 *
 * struct {
 *   opaque identity<1..2^16-1>;
 *   uint32 obfuscated_ticket_age;
 * } PskIdentity;
 *
 * opaque PskBinderEntry<32..255>;
 *
 * struct {
 *   select ( Handshake.msg_type ) {
 *
 *     case client_hello:
 *       PskIdentity identities<7..2^16-1>;
 *       PskBinderEntry binders<33..2^16-1>;
 *
 *     case server_hello:
 *       uint16 selected_identity;
 *   };
 *
 * } PreSharedKeyExtension;
 *
 *
 * part = 0 ==> everything up to the PSK binder list,
 *              returning the binder list length in `binder_list_length`.
 * part = 1 ==> the PSK binder list
 */

#if defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)

int mbedtls_ssl_tls13_write_pre_shared_key_ext_without_binders(
    mbedtls_ssl_context *ssl,
    unsigned char *buf, unsigned char *end,
    size_t *out_len, size_t *binders_len )
{
    unsigned char *p = buf;
    const unsigned char *psk;
    size_t psk_len;
    const unsigned char *psk_identity;
    size_t psk_identity_len;
    const mbedtls_ssl_ciphersuite_t *suite_info;
    const int *ciphersuites;
    int hash_len;
    size_t identities_len, l_binders_len;
    uint32_t obfuscated_ticket_age = 0;

    *out_len = 0;
    *binders_len = 0;

    /* Check if we have any PSKs to offer. If so, return the first.
     *
     * NOTE: Ultimately, we want to be able to offer multiple PSKs,
     *       in which case we want to iterate over them here.
     *
     * As it stands, however, we only ever offer one, chosen
     * by the following heuristic:
     * - If a ticket has been configured, offer the corresponding PSK.
     * - If no ticket has been configured by an external PSK has been
     *   configured, offer that.
     * - Otherwise, skip the PSK extension.
     */

    if( mbedtls_ssl_get_psk_to_offer( ssl, &psk, &psk_len,
                                      &psk_identity, &psk_identity_len ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "skip pre_shared_key extensions" ) );
        return( 0 );
    }

    /*
     * Ciphersuite list
     */
    ciphersuites = ssl->conf->ciphersuite_list;
    for ( int i = 0; ciphersuites[i] != 0; i++ )
    {
        suite_info = mbedtls_ssl_ciphersuite_from_id( ciphersuites[i] );

        if( suite_info == NULL )
            continue;

        /* In this implementation we only add one pre-shared-key extension. */
        ssl->session_negotiate->ciphersuite = ciphersuites[i];
        ssl->handshake->ciphersuite_info = suite_info;
        break;
    }

    hash_len = mbedtls_hash_size_for_ciphersuite( suite_info );
    if( hash_len == -1 )
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

    /* Check if we have space to write the extension, binder included.
     * - extension_type         (2 bytes)
     * - extension_data_len     (2 bytes)
     * - identities_len         (2 bytes)
     * - identity_len           (2 bytes)
     * - identity               (psk_identity_len bytes)
     * - obfuscated_ticket_age  (4 bytes)
     * - binders_len            (2 bytes)
     * - binder_len             (1 byte)
     * - binder                 (hash_len bytes)
     */

    identities_len = 6 + psk_identity_len;
    l_binders_len = 1 + hash_len;
    MBEDTLS_SSL_CHK_BUF_PTR( p, end, 4 + 2 + identities_len + 2 + l_binders_len );

    MBEDTLS_SSL_DEBUG_MSG( 3,
                 ( "client hello, adding pre_shared_key extension, "
                   "omitting PSK binder list" ) );

    /* Extension header */
    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_PRE_SHARED_KEY, p, 0 );
    MBEDTLS_PUT_UINT16_BE( 2 + identities_len + 2 + l_binders_len , p, 2 );

    MBEDTLS_PUT_UINT16_BE( identities_len, p, 4 );
    MBEDTLS_PUT_UINT16_BE( psk_identity_len, p, 6 );
    p += 8;
    memcpy( p, psk_identity, psk_identity_len );
    p += psk_identity_len;

#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)

    /* Calculate obfuscated_ticket_age (omitted for external PSKs). */
    if( ssl->session_negotiate->ticket_age_add > 0 )
    {
        /* TODO: Should we somehow fail if TIME is disabled here?
         * TODO: Use Mbed TLS' time abstraction? */
#if defined(MBEDTLS_HAVE_TIME)
        time_t now = time( NULL );

        if( !( ssl->session_negotiate->ticket_received <= now &&
               now - ssl->session_negotiate->ticket_received < 7 * 86400 * 1000 ) )
        {
            MBEDTLS_SSL_DEBUG_MSG( 3, ( "ticket expired" ) );
            /* TBD: We would have to fall back to another PSK */
            return( MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED );
        }

        obfuscated_ticket_age =
            (uint32_t)( now - ssl->session_negotiate->ticket_received ) +
            ssl->session_negotiate->ticket_age_add;

        MBEDTLS_SSL_DEBUG_MSG( 4, ( "obfuscated_ticket_age: %u",
                                        obfuscated_ticket_age ) );
#endif /* MBEDTLS_HAVE_TIME */
    }
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */

    /* add obfuscated ticket age */
    MBEDTLS_PUT_UINT32_BE( obfuscated_ticket_age, p, 0 );
    p += 4;

    *out_len = ( p - buf ) + l_binders_len + 2;
    *binders_len = l_binders_len + 2;

    ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_PRE_SHARED_KEY;

    return( 0 );
}

int mbedtls_ssl_tls13_write_pre_shared_key_ext_binders(
    mbedtls_ssl_context *ssl,
    unsigned char *buf, unsigned char *end )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char *p = buf;
    const mbedtls_ssl_ciphersuite_t *suite_info;
    const int *ciphersuites;
    int hash_len;
    const unsigned char *psk;
    size_t psk_len;
    const unsigned char *psk_identity;
    size_t psk_identity_len;
    int psk_type;
    unsigned char transcript[MBEDTLS_MD_MAX_SIZE];
    size_t transcript_len;

    /* Check if we have any PSKs to offer. If so, return the first.
     *
     * NOTE: Ultimately, we want to be able to offer multiple PSKs,
     *       in which case we want to iterate over them here.
     *
     * As it stands, however, we only ever offer one, chosen
     * by the following heuristic:
     * - If a ticket has been configured, offer the corresponding PSK.
     * - If no ticket has been configured by an external PSK has been
     *   configured, offer that.
     * - Otherwise, skip the PSK extension.
     */

    if( mbedtls_ssl_get_psk_to_offer( ssl, &psk, &psk_len,
                                      &psk_identity, &psk_identity_len ) != 0 )
    {
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    /*
     * Ciphersuite list
     */
    ciphersuites = ssl->conf->ciphersuite_list;
    for ( int i = 0; ciphersuites[i] != 0; i++ )
    {
        suite_info = mbedtls_ssl_ciphersuite_from_id( ciphersuites[i] );

        if( suite_info == NULL )
            continue;

        /* In this implementation we only add one pre-shared-key extension. */
        ssl->session_negotiate->ciphersuite = ciphersuites[i];
        ssl->handshake->ciphersuite_info = suite_info;
        break;
    }

    hash_len = mbedtls_hash_size_for_ciphersuite( suite_info );
    if( ( hash_len == -1 ) || ( ( end - buf ) != 3 + hash_len ) )
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "client hello, adding PSK binder list" ) );

    /* 2 bytes length field for array of psk binders */
    MBEDTLS_PUT_UINT16_BE( hash_len + 1, p, 0 );
    p += 2;

    /* 1 bytes length field for next psk binder */
    *p++ = MBEDTLS_BYTE_0( hash_len );

    if( ssl->handshake->resume == 1 )
        psk_type = MBEDTLS_SSL_TLS1_3_PSK_RESUMPTION;
    else
        psk_type = MBEDTLS_SSL_TLS1_3_PSK_EXTERNAL;

    /* Get current state of handshake transcript. */
    ret = mbedtls_ssl_get_handshake_transcript( ssl, suite_info->mac,
                                                transcript, sizeof( transcript ),
                                                &transcript_len );
    if( ret != 0 )
        return( ret );

    ret = mbedtls_ssl_tls13_create_psk_binder( ssl,
              mbedtls_psa_translate_md( suite_info->mac ),
              psk, psk_len, psk_type,
              transcript, p );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_create_psk_binder", ret );
        return( ret );
    }

    return( 0 );
}
#endif	/* MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED  */

int mbedtls_ssl_tls13_write_client_hello_exts( mbedtls_ssl_context *ssl,
                                               unsigned char *buf,
                                               unsigned char *end,
                                               size_t *out_len )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char *p = buf;
    size_t ext_len;

    *out_len = 0;

    /* Write supported_versions extension
     *
     * Supported Versions Extension is mandatory with TLS 1.3.
     */
    ret = ssl_tls13_write_supported_versions_ext( ssl, p, end, &ext_len );
    if( ret != 0 )
        return( ret );
    p += ext_len;

    /* Echo the cookie if the server provided one in its preceding
     * HelloRetryRequest message.
     */
    ret = ssl_tls13_write_cookie_ext( ssl, p, end, &ext_len );
    if( ret != 0 )
        return( ret );
    p += ext_len;

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
    if( mbedtls_ssl_conf_tls13_some_ephemeral_enabled( ssl ) )
    {
        ret = ssl_tls13_write_key_share_ext( ssl, p, end, &ext_len );
        if( ret != 0 )
            return( ret );
        p += ext_len;
    }
#endif /* MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED */

#if defined(MBEDTLS_ZERO_RTT)
    ret = mbedtls_ssl_tls13_write_early_data_ext( ssl, p, end, &ext_len );
    if( ret != 0 )
        return( ret );
    p += ext_len;
#endif /* MBEDTLS_ZERO_RTT */

#if defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)
    /* For PSK-based key exchange we need the pre_shared_key extension
     * and the psk_key_exchange_modes extension.
     *
     * The pre_shared_key extension MUST be the last extension in the
     * ClientHello. Servers MUST check that it is the last extension and
     * otherwise fail the handshake with an "illegal_parameter" alert.
     *
     * Add the psk_key_exchange_modes extension.
     */
    ret = ssl_tls13_write_psk_key_exchange_modes_ext( ssl, p, end, &ext_len );
    if( ret != 0 )
        return( ret );
    p += ext_len;
#endif /* MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED */

    *out_len = p - buf;

    return( 0 );
}

/*
 * Functions for parsing and processing Server Hello
 */

/**
 * \brief Detect if the ServerHello contains a supported_versions extension
 *        or not.
 *
 * \param[in] ssl  SSL context
 * \param[in] buf  Buffer containing the ServerHello message
 * \param[in] end  End of the buffer containing the ServerHello message
 *
 * \return 0 if the ServerHello does not contain a supported_versions extension
 * \return 1 if the ServerHello contains a supported_versions extension
 * \return A negative value if an error occurred while parsing the ServerHello.
 */
static int ssl_tls13_is_supported_versions_ext_present(
    mbedtls_ssl_context *ssl,
    const unsigned char *buf,
    const unsigned char *end )
{
    const unsigned char *p = buf;
    size_t legacy_session_id_echo_len;
    size_t extensions_len;
    const unsigned char *extensions_end;

    /*
     * Check there is enough data to access the legacy_session_id_echo vector
     * length:
     * - legacy_version                 2 bytes
     * - random                         MBEDTLS_SERVER_HELLO_RANDOM_LEN bytes
     * - legacy_session_id_echo length  1 byte
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, MBEDTLS_SERVER_HELLO_RANDOM_LEN + 3 );
    p += MBEDTLS_SERVER_HELLO_RANDOM_LEN + 2;
    legacy_session_id_echo_len = *p;

    /*
     * Jump to the extensions, jumping over:
     * - legacy_session_id_echo     (legacy_session_id_echo_len + 1) bytes
     * - cipher_suite               2 bytes
     * - legacy_compression_method  1 byte
     */
     p += legacy_session_id_echo_len + 4;

    /* Case of no extension */
    if( p == end )
        return( 0 );

    /* ...
     * Extension extensions<6..2^16-1>;
     * ...
     * struct {
     *      ExtensionType extension_type; (2 bytes)
     *      opaque extension_data<0..2^16-1>;
     * } Extension;
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    extensions_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    /* Check extensions do not go beyond the buffer of data. */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, extensions_len );
    extensions_end = p + extensions_len;

    while( p < extensions_end )
    {
        unsigned int extension_type;
        size_t extension_data_len;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, 4 );
        extension_type = MBEDTLS_GET_UINT16_BE( p, 0 );
        extension_data_len = MBEDTLS_GET_UINT16_BE( p, 2 );
        p += 4;

        if( extension_type == MBEDTLS_TLS_EXT_SUPPORTED_VERSIONS )
            return( 1 );

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, extension_data_len );
        p += extension_data_len;
    }

    return( 0 );
}

/* Returns a negative value on failure, and otherwise
 * - 1 if the last eight bytes of the ServerHello random bytes indicate that
 *     the server is TLS 1.3 capable but negotiating TLS 1.2 or below.
 * - 0 otherwise
 */
static int ssl_tls13_is_downgrade_negotiation( mbedtls_ssl_context *ssl,
                                               const unsigned char *buf,
                                               const unsigned char *end )
{
    /* First seven bytes of the magic downgrade strings, see RFC 8446 4.1.3 */
    static const unsigned char magic_downgrade_string[] =
        { 0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44 };
    const unsigned char *last_eight_bytes_of_random;
    unsigned char last_byte_of_random;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( buf, end, MBEDTLS_SERVER_HELLO_RANDOM_LEN + 2 );
    last_eight_bytes_of_random = buf + 2 + MBEDTLS_SERVER_HELLO_RANDOM_LEN - 8;

    if( memcmp( last_eight_bytes_of_random,
                magic_downgrade_string,
                sizeof( magic_downgrade_string ) ) == 0 )
    {
        last_byte_of_random = last_eight_bytes_of_random[7];
        return( last_byte_of_random == 0 ||
                last_byte_of_random == 1    );
    }

    return( 0 );
}

/* Returns a negative value on failure, and otherwise
 * - SSL_SERVER_HELLO_COORDINATE_HELLO or
 * - SSL_SERVER_HELLO_COORDINATE_HRR
 * to indicate which message is expected and to be parsed next.
 */
#define SSL_SERVER_HELLO_COORDINATE_HELLO 0
#define SSL_SERVER_HELLO_COORDINATE_HRR 1
static int ssl_server_hello_is_hrr( mbedtls_ssl_context *ssl,
                                    const unsigned char *buf,
                                    const unsigned char *end )
{
    static const unsigned char magic_hrr_string[MBEDTLS_SERVER_HELLO_RANDOM_LEN] =
        { 0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11,
          0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
          0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
          0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33 ,0x9C };

    /* Check whether this message is a HelloRetryRequest ( HRR ) message.
     *
     * Server Hello and HRR are only distinguished by Random set to the
     * special value of the SHA-256 of "HelloRetryRequest".
     *
     * struct {
     *    ProtocolVersion legacy_version = 0x0303;
     *    Random random;
     *    opaque legacy_session_id_echo<0..32>;
     *    CipherSuite cipher_suite;
     *    uint8 legacy_compression_method = 0;
     *    Extension extensions<6..2^16-1>;
     * } ServerHello;
     *
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( buf, end, 2 + sizeof( magic_hrr_string ) );

    if( memcmp( buf + 2, magic_hrr_string, sizeof( magic_hrr_string ) ) == 0 )
    {
        return( SSL_SERVER_HELLO_COORDINATE_HRR );
    }

    return( SSL_SERVER_HELLO_COORDINATE_HELLO );
}

/* Fetch and preprocess
 * Returns a negative value on failure, and otherwise
 * - SSL_SERVER_HELLO_COORDINATE_HELLO or
 * - SSL_SERVER_HELLO_COORDINATE_HRR or
 * - SSL_SERVER_HELLO_COORDINATE_TLS1_2
 */
#define SSL_SERVER_HELLO_COORDINATE_TLS1_2 2
#if defined(MBEDTLS_SSL_USE_MPS)
static int ssl_tls13_server_hello_coordinate( mbedtls_ssl_context *ssl,
                                              mbedtls_mps_handshake_in *msg,
                                              unsigned char **buf,
                                              size_t *buf_len )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    const unsigned char *end;

    MBEDTLS_SSL_PROC_CHK_NEG( mbedtls_mps_read( &ssl->mps->l4 ) );

#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
    if( ret == MBEDTLS_MPS_MSG_CCS )
    {
        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_read_consume( &ssl->mps->l4 ) );
        return( MBEDTLS_ERR_SSL_WANT_READ );
    }
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

    if( ret != MBEDTLS_MPS_MSG_HS )
        return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );

    MBEDTLS_SSL_PROC_CHK( mbedtls_mps_read_handshake( &ssl->mps->l4,
                                                      msg ) );

    if( msg->type != MBEDTLS_SSL_HS_SERVER_HELLO )
        return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );

    ret = mbedtls_mps_reader_get( msg->handle,
                                  msg->length,
                                  buf, NULL );

    if( ret == MBEDTLS_ERR_MPS_READER_OUT_OF_DATA )
    {
        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_read_pause( &ssl->mps->l4 ) );
        return( MBEDTLS_ERR_SSL_WANT_READ );
    }

    *buf_len = msg->length;
    end = *buf + *buf_len;

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_is_supported_versions_ext_present(
                                  ssl, *buf, end ) );
    if( ret == 0 )
    {
        MBEDTLS_SSL_PROC_CHK_NEG(
            ssl_tls13_is_downgrade_negotiation( ssl, *buf, end ) );

        /* If the server is negotiating TLS 1.2 or below and:
         * . we did not propose TLS 1.2 or
         * . the server responded it is TLS 1.3 capable but negotiating a lower
         *   version of the protocol and thus we are under downgrade attack
         * abort the handshake with an "illegal parameter" alert.
         */
        if( ssl->handshake->min_tls_version > MBEDTLS_SSL_VERSION_TLS1_2 || ret )
        {
            MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                                          MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
            return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
        }

        ssl->keep_current_message = 1;
        ssl->tls_version = MBEDTLS_SSL_VERSION_TLS1_2;
        mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_SERVER_HELLO,
                                            *buf, *buf_len );

        if( mbedtls_ssl_conf_tls13_some_ephemeral_enabled( ssl ) )
        {
            ret = ssl_tls13_reset_key_share( ssl );
            if( ret != 0 )
                return( ret );
        }

        return( SSL_SERVER_HELLO_COORDINATE_TLS1_2 );
    }

    ret = ssl_server_hello_is_hrr( ssl, *buf, end );
    switch( ret )
    {
        case SSL_SERVER_HELLO_COORDINATE_HELLO:
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "received ServerHello message" ) );
            break;
        case SSL_SERVER_HELLO_COORDINATE_HRR:
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "received HelloRetryRequest message" ) );
             /* If a client receives a second
              * HelloRetryRequest in the same connection (i.e., where the ClientHello
              * was itself in response to a HelloRetryRequest), it MUST abort the
              * handshake with an "unexpected_message" alert.
              */
            if( ssl->handshake->hello_retry_request_count > 0 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "Multiple HRRs received" ) );
                MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_UNEXPECTED_MESSAGE,
                                    MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
                return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
            }
            /*
             * Clients must abort the handshake with an "illegal_parameter"
             * alert if the HelloRetryRequest would not result in any change
             * in the ClientHello.
             * In a PSK only key exchange that what we expect.
             */
            if( ! mbedtls_ssl_conf_tls13_some_ephemeral_enabled( ssl ) )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1,
                            ( "Unexpected HRR in pure PSK key exchange." ) );
                MBEDTLS_SSL_PEND_FATAL_ALERT(
                            MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                            MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER);
                return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
            }

            ssl->handshake->hello_retry_request_count++;

            break;
    }

cleanup:

    return( ret );
}
#else /* MBEDTLS_SSL_USE_MPS */
static int ssl_tls13_server_hello_coordinate( mbedtls_ssl_context *ssl,
                                              unsigned char **buf,
                                              size_t *buf_len )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    const unsigned char *end;

    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_tls13_fetch_handshake_msg( ssl,
                                             MBEDTLS_SSL_HS_SERVER_HELLO,
                                             buf, buf_len ) );
    end = *buf + *buf_len;

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_is_supported_versions_ext_present(
                                  ssl, *buf, end ) );
    if( ret == 0 )
    {
        MBEDTLS_SSL_PROC_CHK_NEG(
            ssl_tls13_is_downgrade_negotiation( ssl, *buf, end ) );

        /* If the server is negotiating TLS 1.2 or below and:
         * . we did not propose TLS 1.2 or
         * . the server responded it is TLS 1.3 capable but negotiating a lower
         *   version of the protocol and thus we are under downgrade attack
         * abort the handshake with an "illegal parameter" alert.
         */
        if( ssl->handshake->min_tls_version > MBEDTLS_SSL_VERSION_TLS1_2 || ret )
        {
            MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                                          MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
            return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
        }

        ssl->keep_current_message = 1;
        ssl->tls_version = MBEDTLS_SSL_VERSION_TLS1_2;
        mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_SERVER_HELLO,
                                            *buf, *buf_len );

        if( mbedtls_ssl_conf_tls13_some_ephemeral_enabled( ssl ) )
        {
            ret = ssl_tls13_reset_key_share( ssl );
            if( ret != 0 )
                return( ret );
        }

        return( SSL_SERVER_HELLO_COORDINATE_TLS1_2 );
    }

    ret = ssl_server_hello_is_hrr( ssl, *buf, end );
    switch( ret )
    {
        case SSL_SERVER_HELLO_COORDINATE_HELLO:
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "received ServerHello message" ) );
            break;
        case SSL_SERVER_HELLO_COORDINATE_HRR:
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "received HelloRetryRequest message" ) );
             /* If a client receives a second
              * HelloRetryRequest in the same connection (i.e., where the ClientHello
              * was itself in response to a HelloRetryRequest), it MUST abort the
              * handshake with an "unexpected_message" alert.
              */
            if( ssl->handshake->hello_retry_request_count > 0 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "Multiple HRRs received" ) );
                MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_UNEXPECTED_MESSAGE,
                                    MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
                return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
            }
            /*
             * Clients must abort the handshake with an "illegal_parameter"
             * alert if the HelloRetryRequest would not result in any change
             * in the ClientHello.
             * In a PSK only key exchange that what we expect.
             */
            if( ! mbedtls_ssl_conf_tls13_some_ephemeral_enabled( ssl ) )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1,
                            ( "Unexpected HRR in pure PSK key exchange." ) );
                MBEDTLS_SSL_PEND_FATAL_ALERT(
                            MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                            MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER);
                return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
            }

            ssl->handshake->hello_retry_request_count++;

            break;
    }

cleanup:

    return( ret );
}
#endif /* MBEDTLS_SSL_USE_MPS */

static int ssl_tls13_check_server_hello_session_id_echo( mbedtls_ssl_context *ssl,
                                                         const unsigned char **buf,
                                                         const unsigned char *end )
{
    const unsigned char *p = *buf;
    size_t legacy_session_id_echo_len;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 1 );
    legacy_session_id_echo_len = *p++ ;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, legacy_session_id_echo_len );

    /* legacy_session_id_echo */
    if( ssl->session_negotiate->id_len != legacy_session_id_echo_len ||
        memcmp( ssl->session_negotiate->id, p , legacy_session_id_echo_len ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_BUF( 3, "Expected Session ID",
                               ssl->session_negotiate->id,
                               ssl->session_negotiate->id_len );
        MBEDTLS_SSL_DEBUG_BUF( 3, "Received Session ID", p,
                               legacy_session_id_echo_len );

        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                                      MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER);

        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    p += legacy_session_id_echo_len;
    *buf = p;

    MBEDTLS_SSL_DEBUG_BUF( 3, "Session ID", ssl->session_negotiate->id,
                            ssl->session_negotiate->id_len );
    return( 0 );
}

static int ssl_tls13_cipher_suite_is_offered( mbedtls_ssl_context *ssl,
                                              int cipher_suite )
{
    const int *ciphersuite_list = ssl->conf->ciphersuite_list;

    /* Check whether we have offered this ciphersuite */
    for ( size_t i = 0; ciphersuite_list[i] != 0; i++ )
    {
        if( ciphersuite_list[i] == cipher_suite )
        {
            return( 1 );
        }
    }
    return( 0 );
}

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
static int ssl_tls13_parse_max_fragment_length_ext( mbedtls_ssl_context *ssl,
                                                    const unsigned char *buf,
                                                    size_t len )
{
    /*
     * server should use the extension only if we did,
     * and if so the server's value should match ours ( and len is always 1 )
     */
    if( ssl->conf->mfl_code == MBEDTLS_SSL_MAX_FRAG_LEN_NONE ||
        len != 1 ||
        buf[0] != ssl->conf->mfl_code )
    {
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    return( 0 );
}
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */

#if defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)
/*
 * struct {
 *   opaque identity<1..2^16-1>;
 *   uint32 obfuscated_ticket_age;
 * } PskIdentity;
 *
 * opaque PskBinderEntry<32..255>;
 *
 * struct {
 *   select ( Handshake.msg_type ) {
 *     case client_hello:
 *          PskIdentity identities<7..2^16-1>;
 *          PskBinderEntry binders<33..2^16-1>;
 *     case server_hello:
 *          uint16 selected_identity;
 *   };
 *
 * } PreSharedKeyExtension;
 *
 */

static int ssl_tls13_parse_server_psk_identity_ext( mbedtls_ssl_context *ssl,
                                                    const unsigned char *buf,
                                                    size_t len )
{
    int ret = 0;
    size_t selected_identity;

    const unsigned char *psk;
    size_t psk_len;
    const unsigned char *psk_identity;
    size_t psk_identity_len;


    /* Check which PSK we've offered.
     *
     * NOTE: Ultimately, we want to offer multiple PSKs, and in this
     *       case, we need to iterate over them here.
     */
    if( mbedtls_ssl_get_psk_to_offer( ssl, &psk, &psk_len,
                                      &psk_identity, &psk_identity_len ) != 0 )
    {
        /* If we haven't offered a PSK, the server must not send
         * a PSK identity extension. */
        return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
    }

    if( len != (size_t) 2 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad psk_identity extension in server hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    selected_identity = MBEDTLS_GET_UINT16_BE( buf, 0 );

    /* We have offered only one PSK, so the only valid choice
     * for the server is PSK index 0.
     *
     * This will change once we support multiple PSKs. */
    if( selected_identity > 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Server's chosen PSK identity out of range" ) );

        if( ( ret = mbedtls_ssl_send_alert_message( ssl,
                        MBEDTLS_SSL_ALERT_LEVEL_FATAL,
                        MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER ) ) != 0 )
        {
            return( ret );
        }

        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    /* Set the chosen PSK
     *
     * TODO: We don't have to do this in case we offered 0-RTT and the
     *       server accepted it, because in this case we've already
     *       set the handshake PSK. */
    ret = mbedtls_ssl_set_hs_psk( ssl, psk, psk_len );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_set_hs_psk", ret );
        return( ret );
    }

    ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_PRE_SHARED_KEY;
    return( 0 );
}

#endif

#if defined(MBEDTLS_ZERO_RTT)
/* Early Data Extension
*
* struct {} Empty;
*
* struct {
*   select (Handshake.msg_type) {
*     case new_session_ticket:   uint32 max_early_data_size;
*     case client_hello:         Empty;
*     case encrypted_extensions: Empty;
*   };
* } EarlyDataIndication;
*
* This function only handles the case of the EncryptedExtensions message.
*/
static int ssl_tls13_parse_encrypted_extensions_early_data_ext(
    mbedtls_ssl_context *ssl,
    const unsigned char *buf, size_t len )
{
    if( ssl->handshake->early_data != MBEDTLS_SSL_EARLY_DATA_ON )
    {
        /* The server must not send the EarlyDataIndication if the
         * client hasn't indicated the use of 0-RTT. */
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    if( len != 0 )
    {
        /* The message must be empty. */
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    /* Nothing to parse */
    ((void) buf);

    ssl->early_data_status = MBEDTLS_SSL_EARLY_DATA_ACCEPTED;
    return( 0 );
}

int mbedtls_ssl_get_early_data_status( mbedtls_ssl_context *ssl )
{
    if( ssl->state != MBEDTLS_SSL_HANDSHAKE_OVER )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    if( ssl->conf->endpoint == MBEDTLS_SSL_IS_SERVER )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    return( ssl->early_data_status );
}

int mbedtls_ssl_set_early_data( mbedtls_ssl_context *ssl,
                                const unsigned char *buffer, size_t len )
{
    if( buffer == NULL || len == 0 )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    ssl->early_data_buf = buffer;
    ssl->early_data_len = len;
    return( 0 );
}
#endif /* MBEDTLS_ZERO_RTT */

/* Parse ServerHello message and configure context
 *
 * struct {
 *    ProtocolVersion legacy_version = 0x0303; // TLS 1.2
 *    Random random;
 *    opaque legacy_session_id_echo<0..32>;
 *    CipherSuite cipher_suite;
 *    uint8 legacy_compression_method = 0;
 *    Extension extensions<6..2^16-1>;
 * } ServerHello;
 */
static int ssl_tls13_parse_server_hello( mbedtls_ssl_context *ssl,
                                         const unsigned char *buf,
                                         const unsigned char *end,
                                         int is_hrr )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    const unsigned char *p = buf;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;
    size_t extensions_len;
    const unsigned char *extensions_end;
    uint16_t cipher_suite;
    const mbedtls_ssl_ciphersuite_t *ciphersuite_info;
    int fatal_alert = 0;

    /*
     * Check there is space for minimal fields
     *
     * - legacy_version             ( 2 bytes)
     * - random                     (MBEDTLS_SERVER_HELLO_RANDOM_LEN bytes)
     * - legacy_session_id_echo     ( 1 byte ), minimum size
     * - cipher_suite               ( 2 bytes)
     * - legacy_compression_method  ( 1 byte )
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, MBEDTLS_SERVER_HELLO_RANDOM_LEN + 6 );

    MBEDTLS_SSL_DEBUG_BUF( 4, "server hello", p, end - p );
    MBEDTLS_SSL_DEBUG_BUF( 3, "server hello, version", p, 2 );

    /* ...
     * ProtocolVersion legacy_version = 0x0303; // TLS 1.2
     * ...
     * with ProtocolVersion defined as:
     * uint16 ProtocolVersion;
     */
    if( mbedtls_ssl_read_version( p, ssl->conf->transport ) !=
          MBEDTLS_SSL_VERSION_TLS1_2 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Unsupported version of TLS." ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_PROTOCOL_VERSION,
                                      MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION );
        ret = MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION;
        goto cleanup;
    }
    p += 2;

    /* ...
     * Random random;
     * ...
     * with Random defined as:
     * opaque Random[MBEDTLS_SERVER_HELLO_RANDOM_LEN];
     */
    if( !is_hrr )
    {
        memcpy( &handshake->randbytes[MBEDTLS_CLIENT_HELLO_RANDOM_LEN], p,
                MBEDTLS_SERVER_HELLO_RANDOM_LEN );
        MBEDTLS_SSL_DEBUG_BUF( 3, "server hello, random bytes",
                               p, MBEDTLS_SERVER_HELLO_RANDOM_LEN );
    }
    p += MBEDTLS_SERVER_HELLO_RANDOM_LEN;

    /* ...
     * opaque legacy_session_id_echo<0..32>;
     * ...
     */
    if( ssl_tls13_check_server_hello_session_id_echo( ssl, &p, end ) != 0 )
    {
        fatal_alert = MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER;
        goto cleanup;
    }

    /* ...
     * CipherSuite cipher_suite;
     * ...
     * with CipherSuite defined as:
     * uint8 CipherSuite[2];
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    cipher_suite = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;


    ciphersuite_info = mbedtls_ssl_ciphersuite_from_id( cipher_suite );
    /*
     * Check whether this ciphersuite is valid and offered.
     */
    if( ( mbedtls_ssl_validate_ciphersuite( ssl, ciphersuite_info,
                                            ssl->tls_version,
                                            ssl->tls_version ) != 0 ) ||
        !ssl_tls13_cipher_suite_is_offered( ssl, cipher_suite ) )
    {
        fatal_alert = MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER;
    }
    /*
     * If we received an HRR before and that the proposed selected
     * ciphersuite in this server hello is not the same as the one
     * proposed in the HRR, we abort the handshake and send an
     * "illegal_parameter" alert.
     */
    else if( ( !is_hrr ) && ( handshake->hello_retry_request_count > 0 ) &&
             ( cipher_suite != ssl->session_negotiate->ciphersuite ) )
    {
        fatal_alert = MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER;
    }

    if( fatal_alert == MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "invalid ciphersuite(%04x) parameter",
                                    cipher_suite ) );
        goto cleanup;
    }

    /* Configure ciphersuites */
    mbedtls_ssl_optimize_checksum( ssl, ciphersuite_info );

    handshake->ciphersuite_info = ciphersuite_info;
    ssl->session_negotiate->ciphersuite = cipher_suite;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "server hello, chosen ciphersuite: ( %04x ) - %s",
                                 cipher_suite, ciphersuite_info->name ) );

#if defined(MBEDTLS_HAVE_TIME)
    ssl->session_negotiate->start = time( NULL );
#endif /* MBEDTLS_HAVE_TIME */

    /* ...
     * uint8 legacy_compression_method = 0;
     * ...
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 1 );
    if( p[0] != 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad legacy compression method" ) );
        fatal_alert = MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER;
        goto cleanup;
    }
    p++;

    /* ...
     * Extension extensions<6..2^16-1>;
     * ...
     * struct {
     *      ExtensionType extension_type; (2 bytes)
     *      opaque extension_data<0..2^16-1>;
     * } Extension;
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    extensions_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    /* Check extensions do not go beyond the buffer of data. */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, extensions_len );
    extensions_end = p + extensions_len;

    MBEDTLS_SSL_DEBUG_BUF( 3, "server hello extensions", p, extensions_len );

    while( p < extensions_end )
    {
        unsigned int extension_type;
        size_t extension_data_len;
        const unsigned char *extension_data_end;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, 4 );
        extension_type = MBEDTLS_GET_UINT16_BE( p, 0 );
        extension_data_len = MBEDTLS_GET_UINT16_BE( p, 2 );
        p += 4;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, extension_data_len );
        extension_data_end = p + extension_data_len;

        switch( extension_type )
        {
            case MBEDTLS_TLS_EXT_COOKIE:

                if( !is_hrr )
                {
                    fatal_alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT;
                    goto cleanup;
                }

                ret = ssl_tls13_parse_cookie_ext( ssl,
                                                  p, extension_data_end );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1,
                                           "ssl_tls13_parse_cookie_ext",
                                           ret );
                    goto cleanup;
                }
                break;

            case MBEDTLS_TLS_EXT_SUPPORTED_VERSIONS:
                ret = ssl_tls13_parse_supported_versions_ext( ssl,
                                                              p,
                                                              extension_data_end );
                if( ret != 0 )
                    goto cleanup;
                break;

#if defined(MBEDTLS_KEY_EXCHANGE_PSK_ENABLED)
            case MBEDTLS_TLS_EXT_PRE_SHARED_KEY:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found pre_shared_key extension" ) );
                if( is_hrr )
                {
                    fatal_alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT;
                    goto cleanup;
                }

                if( ( ret = ssl_tls13_parse_server_psk_identity_ext(
                                ssl, p, extension_data_len ) ) != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET(
                        1, ( "ssl_tls13_parse_server_psk_identity_ext" ), ret );
                    return( ret );
                }
                break;
#endif /* MBEDTLS_KEY_EXCHANGE_PSK_ENABLED */

            case MBEDTLS_TLS_EXT_KEY_SHARE:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found key_shares extension" ) );
                if( ! mbedtls_ssl_conf_tls13_some_ephemeral_enabled( ssl ) )
                {
                    fatal_alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT;
                    goto cleanup;
                }

                if( is_hrr )
                    ret = ssl_tls13_parse_hrr_key_share_ext( ssl,
                                            p, extension_data_end );
                else
                    ret = ssl_tls13_parse_key_share_ext( ssl,
                                            p, extension_data_end );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1,
                                           "ssl_tls13_parse_key_share_ext",
                                           ret );
                    goto cleanup;
                }
                break;

            default:
                MBEDTLS_SSL_DEBUG_MSG(
                    3,
                    ( "unknown extension found: %u ( ignoring )",
                      extension_type ) );

                fatal_alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT;
                goto cleanup;
        }

        p += extension_data_len;
    }

cleanup:

    if( fatal_alert == MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT )
    {
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT,
                                      MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION );
        ret = MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION;
    }
    else if ( fatal_alert == MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER )
    {
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                                      MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
        ret = MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER;
    }
    return( ret );
}

static int ssl_tls13_postprocess_server_hello( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    mbedtls_ssl_key_set traffic_keys;
    mbedtls_ssl_transform *transform_handshake = NULL;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;

    /* Determine the key exchange mode:
     * 1) If both the pre_shared_key and key_share extensions were received
     *    then the key exchange mode is PSK with EPHEMERAL.
     * 2) If only the pre_shared_key extension was received then the key
     *    exchange mode is PSK-only.
     * 3) If only the key_share extension was received then the key
     *    exchange mode is EPHEMERAL-only.
     */
    switch( handshake->extensions_present &
            ( MBEDTLS_SSL_EXT_PRE_SHARED_KEY | MBEDTLS_SSL_EXT_KEY_SHARE ) )
    {
        /* Only the pre_shared_key extension was received */
        case MBEDTLS_SSL_EXT_PRE_SHARED_KEY:
            handshake->key_exchange = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK;
            break;

        /* Only the key_share extension was received */
        case MBEDTLS_SSL_EXT_KEY_SHARE:
            handshake->key_exchange = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL;
            break;

        /* Both the pre_shared_key and key_share extensions were received */
        case ( MBEDTLS_SSL_EXT_PRE_SHARED_KEY | MBEDTLS_SSL_EXT_KEY_SHARE ):
            handshake->key_exchange = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL;
            break;

        /* Neither pre_shared_key nor key_share extension was received */
        default:
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Unknown key exchange." ) );
            ret = MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE;
            goto cleanup;
    }

    /* Start the TLS 1.3 key schedule: Set the PSK and derive early secret.
     *
     * TODO: We don't have to do this in case we offered 0-RTT and the
     *       server accepted it. In this case, we could skip generating
     *       the early secret. */
    ret = mbedtls_ssl_tls13_key_schedule_stage_early( ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_key_schedule_stage_early_data",
                               ret );
        goto cleanup;
    }

    /* Compute handshake secret */
    ret = mbedtls_ssl_tls13_key_schedule_stage_handshake( ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_derive_master_secret", ret );
        goto cleanup;
    }

    /* Next evolution in key schedule: Establish handshake secret and
     * key material. */
    ret = mbedtls_ssl_tls13_generate_handshake_keys( ssl, &traffic_keys );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_generate_handshake_keys",
                               ret );
        goto cleanup;
    }

    transform_handshake = mbedtls_calloc( 1, sizeof( mbedtls_ssl_transform ) );
    if( transform_handshake == NULL )
    {
        ret = MBEDTLS_ERR_SSL_ALLOC_FAILED;
        goto cleanup;
    }

    ret = mbedtls_ssl_tls13_populate_transform( transform_handshake,
                              ssl->conf->endpoint,
                              ssl->session_negotiate->ciphersuite,
                              &traffic_keys,
                              ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_populate_transform", ret );
        goto cleanup;
    }

#if !defined(MBEDTLS_SSL_USE_MPS)
    handshake->transform_handshake = transform_handshake;
    mbedtls_ssl_set_inbound_transform( ssl, transform_handshake );
#else /* MBEDTLS_SSL_USE_MPS */
    ret = mbedtls_mps_add_key_material( &ssl->mps->l4,
                                        transform_handshake,
                                        &handshake->epoch_handshake );
    if( ret != 0 )
        return( ret );

    ret = mbedtls_mps_set_incoming_keys( &ssl->mps->l4,
                                         handshake->epoch_handshake );
    if( ret != 0 )
        return( ret );
#endif /* MBEDTLS_SSL_USE_MPS */

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to handshake keys for inbound traffic" ) );
    ssl->session_in = ssl->session_negotiate;

    /*
     * State machine update
     */
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_ENCRYPTED_EXTENSIONS );

cleanup:

    mbedtls_platform_zeroize( &traffic_keys, sizeof( traffic_keys ) );
    if( ret != 0 )
    {
        mbedtls_free( transform_handshake );

        MBEDTLS_SSL_PEND_FATAL_ALERT(
            MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE,
            MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
    }
    return( ret );
}

static int ssl_tls13_postprocess_hrr( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
    /* If not offering early data, the client sends a dummy CCS record
     * immediately before its second flight. This may either be before
     * its second ClientHello or before its encrypted handshake flight.
     */
    mbedtls_ssl_handshake_set_state( ssl,
            MBEDTLS_SSL_CLIENT_CCS_BEFORE_2ND_CLIENT_HELLO );
#else
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_HELLO );
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

    mbedtls_ssl_session_reset_msg_layer( ssl, 0 );

    /*
     * We are going to re-generate a shared secret corresponding to the group
     * selected by the server, which is different from the group for which we
     * generated a shared secret in the first client hello.
     * Thus, reset the shared secret.
     */
    ret = ssl_tls13_reset_key_share( ssl );
    if( ret != 0 )
        return( ret );

    return( 0 );
}

/*
 * Wait and parse ServerHello handshake message.
 * Handler for MBEDTLS_SSL_SERVER_HELLO
 */
static int ssl_tls13_process_server_hello( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
#if defined(MBEDTLS_SSL_USE_MPS)
    mbedtls_mps_handshake_in msg;
#endif
    unsigned char *buf = NULL;
    size_t buf_len = 0;
    int is_hrr = 0;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> %s", __func__ ) );

    /* Coordination step
     * - Fetch record
     * - Make sure it's either a ServerHello or a HRR.
     * - Switch processing routine in case of HRR
     */
    ssl->handshake->extensions_present = MBEDTLS_SSL_EXT_NONE;

#if defined(MBEDTLS_SSL_USE_MPS)
    ret = ssl_tls13_server_hello_coordinate( ssl, &msg, &buf, &buf_len );
#else /* MBEDTLS_SSL_USE_MPS */
    ret = ssl_tls13_server_hello_coordinate( ssl, &buf, &buf_len );
#endif /* MBEDTLS_SSL_USE_MPS */

    if( ret < 0 )
        goto cleanup;
    else
        is_hrr = ( ret == SSL_SERVER_HELLO_COORDINATE_HRR );

    if( ret == SSL_SERVER_HELLO_COORDINATE_TLS1_2 )
    {
        ret = 0;
        goto cleanup;
    }

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_parse_server_hello( ssl, buf,
                                                        buf + buf_len,
                                                        is_hrr ) );
    if( is_hrr )
        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_reset_transcript_for_hrr( ssl ) );

    mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_SERVER_HELLO,
                                        buf, buf_len );
#if defined(MBEDTLS_SSL_USE_MPS)
        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_reader_commit( msg.handle ) );
        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_read_consume( &ssl->mps->l4  ) );
#endif /* MBEDTLS_SSL_USE_MPS */

    if( is_hrr )
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_postprocess_hrr( ssl ) );
    else
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_postprocess_server_hello( ssl ) );

cleanup:
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= %s ( %s )", __func__,
                                is_hrr?"HelloRetryRequest":"ServerHello" ) );
    return( ret );
}

/*
 *
 * EncryptedExtensions message
 *
 * The EncryptedExtensions message contains any extensions which
 * should be protected, i.e., any which are not needed to establish
 * the cryptographic context.
 */

/*
 * Overview
 */

/* Main entry point; orchestrates the other functions */
static int ssl_tls13_process_encrypted_extensions( mbedtls_ssl_context *ssl );

static int ssl_tls13_parse_encrypted_extensions( mbedtls_ssl_context *ssl,
                                                 const unsigned char *buf,
                                                 const unsigned char *end );
static int ssl_tls13_postprocess_encrypted_extensions( mbedtls_ssl_context *ssl );

/*
 * Handler for  MBEDTLS_SSL_ENCRYPTED_EXTENSIONS
 */
static int ssl_tls13_process_encrypted_extensions( mbedtls_ssl_context *ssl )
{
    int ret;
    unsigned char *buf;
    size_t buf_len;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse encrypted extensions" ) );

    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_tls13_fetch_handshake_msg( ssl,
                                             MBEDTLS_SSL_HS_ENCRYPTED_EXTENSIONS,
                                             &buf, &buf_len ) );

    /* Process the message contents */
    MBEDTLS_SSL_PROC_CHK(
        ssl_tls13_parse_encrypted_extensions( ssl, buf, buf + buf_len ) );

    mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_ENCRYPTED_EXTENSIONS,
                                        buf, buf_len );

#if defined(MBEDTLS_SSL_USE_MPS)
    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_mps_hs_consume_full_hs_msg( ssl ) );
#endif /* MBEDTLS_SSL_USE_MPS */

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_postprocess_encrypted_extensions( ssl ) );

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse encrypted extensions" ) );
    return( ret );

}

/* Parse EncryptedExtensions message
 * struct {
 *     Extension extensions<0..2^16-1>;
 * } EncryptedExtensions;
 */
static int ssl_tls13_parse_encrypted_extensions( mbedtls_ssl_context *ssl,
                                                 const unsigned char *buf,
                                                 const unsigned char *end )
{
    int ret = 0;
    size_t extensions_len;
    const unsigned char *p = buf;
    const unsigned char *extensions_end;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    extensions_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    MBEDTLS_SSL_DEBUG_BUF( 3, "encrypted extensions", p, extensions_len );
    extensions_end = p + extensions_len;
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, extensions_len );

    while( p < extensions_end )
    {
        unsigned int extension_type;
        size_t extension_data_len;

        /*
         * struct {
         *     ExtensionType extension_type; (2 bytes)
         *     opaque extension_data<0..2^16-1>;
         * } Extension;
         */
        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, 4 );
        extension_type = MBEDTLS_GET_UINT16_BE( p, 0 );
        extension_data_len = MBEDTLS_GET_UINT16_BE( p, 2 );
        p += 4;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, extension_data_len );

        /* The client MUST check EncryptedExtensions for the
         * presence of any forbidden extensions and if any are found MUST abort
         * the handshake with an "unsupported_extension" alert.
         */
        switch( extension_type )
        {

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
            case MBEDTLS_TLS_EXT_MAX_FRAGMENT_LENGTH:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found max_fragment_length extension" ) );

                ret = ssl_tls13_parse_max_fragment_length_ext( ssl, p,
                                                               extension_data_len );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_parse_max_fragment_length_ext", ret );
                    return( ret );
                }

                break;
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */

            case MBEDTLS_TLS_EXT_SUPPORTED_GROUPS:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found extensions supported groups" ) );
                break;

#if defined(MBEDTLS_SSL_ALPN)
            case MBEDTLS_TLS_EXT_ALPN:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found alpn extension" ) );

                if( ( ret = ssl_tls13_parse_alpn_ext( ssl, p, (size_t)extension_data_len ) ) != 0 )
                {
                    return( ret );
                }

                break;
#endif /* MBEDTLS_SSL_ALPN */

#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
            case MBEDTLS_TLS_EXT_SERVERNAME:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found server_name extension" ) );

                /* The server_name extension should be an empty extension */

                break;
#endif /* MBEDTLS_SSL_SERVER_NAME_INDICATION */

#if defined(MBEDTLS_ZERO_RTT)
            case MBEDTLS_TLS_EXT_EARLY_DATA:
                MBEDTLS_SSL_DEBUG_MSG(3, ( "found early_data extension" ));

                ret = ssl_tls13_parse_encrypted_extensions_early_data_ext(
                    ssl, p, extension_data_len );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_parse_encrypted_extensions_early_data_ext", ret );
                    return( ret );
                }
                break;
#endif /* MBEDTLS_ZERO_RTT */

            default:
                MBEDTLS_SSL_DEBUG_MSG(
                    3, ( "unsupported extension found: %u ", extension_type) );
                MBEDTLS_SSL_PEND_FATAL_ALERT(
                    MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT,   \
                    MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION );
                return ( MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION );
                break;
        }

        p += extension_data_len;
    }

    /* Check that we consumed all the message. */
    if( p != end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "EncryptedExtension lengths misaligned" ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR,   \
                                      MBEDTLS_ERR_SSL_DECODE_ERROR );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    return( ret );
}

static int ssl_tls13_postprocess_encrypted_extensions( mbedtls_ssl_context *ssl )
{
#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
    if( mbedtls_ssl_tls13_some_psk_enabled( ssl ) )
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_FINISHED );
    else
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CERTIFICATE_REQUEST );
#else
    ((void) ssl);
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_FINISHED );
#endif
    return( 0 );
}

/*
 *
 * STATE HANDLING: Write Early-Data
 *
 */

 /*
  * Overview
  */

  /* Main state-handling entry point; orchestrates the other functions. */
int ssl_tls13_write_early_data_process( mbedtls_ssl_context *ssl );

#define SSL_EARLY_DATA_WRITE 0
#define SSL_EARLY_DATA_SKIP  1
static int ssl_tls13_write_early_data_coordinate( mbedtls_ssl_context *ssl );

#if defined(MBEDTLS_ZERO_RTT)
static int ssl_tls13_write_early_data_prepare( mbedtls_ssl_context *ssl );

/* Write early-data message */
static int ssl_tls13_write_early_data_write( mbedtls_ssl_context *ssl,
    unsigned char *buf,
    size_t buf_len,
    size_t *out_len );
#endif /* MBEDTLS_ZERO_RTT */

/* Update the state after handling the outgoing early-data message. */
static int ssl_tls13_write_early_data_postprocess( mbedtls_ssl_context *ssl );

/*
 * Implementation
 */

int ssl_tls13_write_early_data_process( mbedtls_ssl_context *ssl )
{
    int ret;
#if defined(MBEDTLS_SSL_USE_MPS)
    mbedtls_writer *msg;
    unsigned char *buf;
    mbedtls_mps_size_t buf_len, msg_len;
#endif /* MBEDTLS_SSL_USE_MPS */
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write early data" ) );

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_write_early_data_coordinate( ssl ) );
    if( ret == SSL_EARLY_DATA_WRITE )
    {
#if defined(MBEDTLS_ZERO_RTT)

        MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_early_data_prepare( ssl ) );

#if defined(MBEDTLS_SSL_USE_MPS)
        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_write_application( &ssl->mps->l4,
                                                             &msg ) );

        /* Request write-buffer */
        MBEDTLS_SSL_PROC_CHK( mbedtls_writer_get( msg, MBEDTLS_MPS_SIZE_MAX,
                                                  &buf, &buf_len ) );

        MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_early_data_write(
                                  ssl, buf, buf_len, &msg_len ) );

        /* Commit message */
        MBEDTLS_SSL_PROC_CHK( mbedtls_writer_commit_partial( msg,
                                                             buf_len - msg_len ) );

        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_dispatch( &ssl->mps->l4 ) );

        /* Update state */
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_early_data_postprocess( ssl ) );

#else  /* MBEDTLS_SSL_USE_MPS */

        /* Write early-data to message buffer. */
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_early_data_write( ssl, ssl->out_msg,
                                                                MBEDTLS_SSL_OUT_CONTENT_LEN,
                                                                &ssl->out_msglen ) );

        ssl->out_msgtype = MBEDTLS_SSL_MSG_APPLICATION_DATA;

        /* Update state */
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_early_data_postprocess( ssl ) );

        /* Dispatch message */
        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_write_record( ssl, 1 ) );

#endif /* MBEDTLS_SSL_USE_MPS */

#else /* MBEDTLS_ZERO_RTT */
        /* Should never happen */
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

#endif /* MBEDTLS_ZERO_RTT */
    }
    else
    {
        /* Update state */
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_early_data_postprocess( ssl ) );
    }

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write early data" ) );
    return( ret );
}

#if defined(MBEDTLS_ZERO_RTT)

static int ssl_tls13_write_early_data_coordinate( mbedtls_ssl_context *ssl )
{
    if( ssl->handshake->early_data != MBEDTLS_SSL_EARLY_DATA_ON )
        return( SSL_EARLY_DATA_SKIP );

    return( SSL_EARLY_DATA_WRITE );
}

static int ssl_tls13_write_early_data_prepare( mbedtls_ssl_context *ssl )
{
    int ret;
    mbedtls_ssl_key_set traffic_keys;

    const unsigned char *psk;
    size_t psk_len;
    const unsigned char *psk_identity;
    size_t psk_identity_len;

    mbedtls_ssl_transform *transform_earlydata;

    /* From RFC 8446:
     * "The PSK used to encrypt the
     *  early data MUST be the first PSK listed in the client's
     *  'pre_shared_key' extension."
     */

    if( mbedtls_ssl_get_psk_to_offer( ssl, &psk, &psk_len,
                                      &psk_identity, &psk_identity_len ) != 0 )
    {
        /* This should never happen: We can only have gone past
         * ssl_tls13_write_early_data_coordinate() if we have offered a PSK. */
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    if( ( ret = mbedtls_ssl_set_hs_psk( ssl, psk, psk_len ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_set_hs_psk", ret );
        return( ret );
    }

    /* Start the TLS 1.3 key schedule: Set the PSK and derive early secret. */
    ret = mbedtls_ssl_tls13_key_schedule_stage_early( ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1,
             "mbedtls_ssl_tls13_key_schedule_stage_early", ret );
        return( ret );
    }

    /* Derive 0-RTT key material */
    ret = mbedtls_ssl_tls13_generate_early_data_keys(
        ssl, &traffic_keys );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1,
            "mbedtls_ssl_tls13_generate_early_data_keys", ret );
        return( ret );
    }

    transform_earlydata =
        mbedtls_calloc( 1, sizeof( mbedtls_ssl_transform ) );
    if( transform_earlydata == NULL )
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

    ret = mbedtls_ssl_tls13_populate_transform(
                          transform_earlydata,
                          ssl->conf->endpoint,
                          ssl->session_negotiate->ciphersuite,
                          &traffic_keys,
                          ssl );
    if( ret != 0 )
        return( ret );

#if defined(MBEDTLS_SSL_USE_MPS)
    /* Register transform with MPS. */
    ret = mbedtls_mps_add_key_material( &ssl->mps->l4,
                                        transform_earlydata,
                                        &ssl->handshake->epoch_earlydata );
    if( ret != 0 )
        return( ret );

    /* Use new transform for outgoing data. */
    ret = mbedtls_mps_set_outgoing_keys( &ssl->mps->l4,
                                         ssl->handshake->epoch_earlydata );
    if( ret != 0 )
        return( ret );
#else /* MBEDTLS_SSL_USE_MPS */

    /* Activate transform */
    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to 0-RTT keys for outbound traffic" ) );
    ssl->handshake->transform_earlydata = transform_earlydata;
    mbedtls_ssl_set_outbound_transform( ssl, ssl->handshake->transform_earlydata );

#endif /* MBEDTLS_SSL_USE_MPS */

    return( 0 );
}

static int ssl_tls13_write_early_data_write( mbedtls_ssl_context *ssl,
    unsigned char *buf,
    size_t buf_len,
    size_t *out_len )
{
    if( ssl->early_data_len > buf_len )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "buffer too small" ) );
        return ( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }
    else
    {
        memcpy( buf, ssl->early_data_buf, ssl->early_data_len );

#if defined(MBEDTLS_SSL_USE_MPS)
        *out_len = ssl->early_data_len;
        MBEDTLS_SSL_DEBUG_BUF( 3, "Early Data", buf, ssl->early_data_len );
#else
        buf[ssl->early_data_len] = MBEDTLS_SSL_MSG_APPLICATION_DATA;
        *out_len = ssl->early_data_len + 1;

        MBEDTLS_SSL_DEBUG_BUF( 3, "Early Data", ssl->out_msg, *out_len );
#endif /* MBEDTLS_SSL_USE_MPS */
    }

    return( 0 );
}

#else /* MBEDTLS_ZERO_RTT */

static int ssl_tls13_write_early_data_coordinate( mbedtls_ssl_context *ssl )
{
    ((void) ssl);
    return( SSL_EARLY_DATA_SKIP );
}

#endif /* MBEDTLS_ZERO_RTT */

static int ssl_tls13_write_early_data_postprocess( mbedtls_ssl_context *ssl )
{
    /* Clear PSK we've used for the 0-RTT. */
    mbedtls_ssl_remove_hs_psk( ssl );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_HELLO );
    return ( 0 );
}

/*
 *
 * STATE HANDLING: Write End-of-Early-Data
 *
 */

 /*
  * Overview
  */

  /* Main state-handling entry point; orchestrates the other functions. */
int ssl_tls13_write_end_of_early_data_process( mbedtls_ssl_context *ssl );

#define SSL_END_OF_EARLY_DATA_WRITE 0
#define SSL_END_OF_EARLY_DATA_SKIP  1
static int ssl_tls13_write_end_of_early_data_coordinate( mbedtls_ssl_context *ssl );

/* Update the state after handling the outgoing end-of-early-data message. */
static int ssl_tls13_write_end_of_early_data_postprocess( mbedtls_ssl_context *ssl );

/*
 * Implementation
 */

int ssl_tls13_write_end_of_early_data_process( mbedtls_ssl_context *ssl )
{
    int ret;
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write EndOfEarlyData" ) );

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_write_end_of_early_data_coordinate( ssl ) );
    if( ret == SSL_END_OF_EARLY_DATA_WRITE )
    {
        unsigned char *buf;
        size_t buf_len;

        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_start_handshake_msg( ssl,
                          MBEDTLS_SSL_HS_END_OF_EARLY_DATA, &buf, &buf_len ) );

        mbedtls_ssl_add_hs_hdr_to_checksum(
            ssl, MBEDTLS_SSL_HS_END_OF_EARLY_DATA, 0 );

        MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_end_of_early_data_postprocess( ssl ) );
        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_finish_handshake_msg( ssl, buf_len, 0 ) );
    }
    else
    {
        /* Update state */
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_end_of_early_data_postprocess( ssl ) );
    }

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write EndOfEarlyData" ) );
    return( ret );
}

static int ssl_tls13_write_end_of_early_data_coordinate( mbedtls_ssl_context *ssl )
{
    ((void) ssl);

#if defined(MBEDTLS_ZERO_RTT)
    if( ssl->handshake->early_data == MBEDTLS_SSL_EARLY_DATA_ON )
    {
        if( ssl->early_data_status == MBEDTLS_SSL_EARLY_DATA_ACCEPTED )
            return( SSL_END_OF_EARLY_DATA_WRITE );

        /*
         * RFC 8446:
         * "If the server does not send an "early_data"
         *  extension in EncryptedExtensions, then the client MUST NOT send an
         *  EndOfEarlyData message."
         */

        MBEDTLS_SSL_DEBUG_MSG( 4, ( "skip EndOfEarlyData, server rejected" ) );
    }
#endif /* MBEDTLS_ZERO_RTT */

    return( SSL_END_OF_EARLY_DATA_SKIP );
}

static int ssl_tls13_write_end_of_early_data_postprocess( mbedtls_ssl_context *ssl )
{
#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
    mbedtls_ssl_handshake_set_state(
        ssl,
        MBEDTLS_SSL_CLIENT_CCS_AFTER_SERVER_FINISHED );
#else
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_CERTIFICATE );
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

    return( 0 );
}

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
/*
 *
 * STATE HANDLING: CertificateRequest
 *
 */
#define SSL_CERTIFICATE_REQUEST_EXPECT_REQUEST 0
#define SSL_CERTIFICATE_REQUEST_SKIP           1
/* Coordination:
 * Deals with the ambiguity of not knowing if a CertificateRequest
 * will be sent. Returns a negative code on failure, or
 * - SSL_CERTIFICATE_REQUEST_EXPECT_REQUEST
 * - SSL_CERTIFICATE_REQUEST_SKIP
 * indicating if a Certificate Request is expected or not.
 */
#if defined(MBEDTLS_SSL_USE_MPS)
static int ssl_tls13_certificate_request_coordinate( mbedtls_ssl_context *ssl )
{
    int ret;
    mbedtls_mps_handshake_in msg;

    if( mbedtls_ssl_tls13_kex_with_psk( ssl ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "<= skip parse certificate request" ) );
        return( SSL_CERTIFICATE_REQUEST_SKIP );
    }

    MBEDTLS_SSL_PROC_CHK_NEG( mbedtls_mps_read( &ssl->mps->l4 ) );
    if( ret == MBEDTLS_MPS_MSG_HS )
    {
        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_read_handshake( &ssl->mps->l4, &msg ) );

        if( msg.type == MBEDTLS_SSL_HS_CERTIFICATE_REQUEST )
            return( SSL_CERTIFICATE_REQUEST_EXPECT_REQUEST );
    }

    return( SSL_CERTIFICATE_REQUEST_SKIP );

cleanup:
    return( ret);
}
#else /* MBEDTLS_SSL_USE_MPS */
static int ssl_tls13_certificate_request_coordinate( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    if( mbedtls_ssl_tls13_kex_with_psk( ssl ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "<= skip parse certificate request" ) );
        return( SSL_CERTIFICATE_REQUEST_SKIP );
    }

    if( ( ret = mbedtls_ssl_read_record( ssl, 0 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_read_record", ret );
        return( ret );
    }
    ssl->keep_current_message = 1;

    if( ( ssl->in_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE ) &&
        ( ssl->in_msg[0] == MBEDTLS_SSL_HS_CERTIFICATE_REQUEST ) )
    {
        return( SSL_CERTIFICATE_REQUEST_EXPECT_REQUEST );
    }

    return( SSL_CERTIFICATE_REQUEST_SKIP );
}
#endif /* MBEDTLS_SSL_USE_MPS */

/*
 * ssl_tls13_parse_certificate_request()
 *     Parse certificate request
 * struct {
 *   opaque certificate_request_context<0..2^8-1>;
 *   Extension extensions<2..2^16-1>;
 * } CertificateRequest;
 */
static int ssl_tls13_parse_certificate_request( mbedtls_ssl_context *ssl,
                                                const unsigned char *buf,
                                                const unsigned char *end )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    const unsigned char *p = buf;
    size_t certificate_request_context_len = 0;
    size_t extensions_len = 0;
    const unsigned char *extensions_end;
    unsigned char sig_alg_ext_found = 0;

    /* ...
     * opaque certificate_request_context<0..2^8-1>
     * ...
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 1 );
    certificate_request_context_len = (size_t) p[0];
    p += 1;

    if( certificate_request_context_len > 0 )
    {
        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, certificate_request_context_len );
        MBEDTLS_SSL_DEBUG_BUF( 3, "Certificate Request Context",
                               p, certificate_request_context_len );

        mbedtls_ssl_handshake_params *handshake = ssl->handshake;
        handshake->certificate_request_context =
                mbedtls_calloc( 1, certificate_request_context_len );
        if( handshake->certificate_request_context == NULL )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "buffer too small" ) );
            return ( MBEDTLS_ERR_SSL_ALLOC_FAILED );
        }
        memcpy( handshake->certificate_request_context, p,
                certificate_request_context_len );
        p += certificate_request_context_len;
    }

    /* ...
     * Extension extensions<2..2^16-1>;
     * ...
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    extensions_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, extensions_len );
    extensions_end = p + extensions_len;

    while( p < extensions_end )
    {
        unsigned int extension_type;
        size_t extension_data_len;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, 4 );
        extension_type = MBEDTLS_GET_UINT16_BE( p, 0 );
        extension_data_len = MBEDTLS_GET_UINT16_BE( p, 2 );
        p += 4;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, extension_data_len );

        switch( extension_type )
        {
            case MBEDTLS_TLS_EXT_SIG_ALG:
                MBEDTLS_SSL_DEBUG_MSG( 3,
                        ( "found signature algorithms extension" ) );
                ret = mbedtls_ssl_tls13_parse_sig_alg_ext( ssl, p,
                              p + extension_data_len );
                if( ret != 0 )
                    return( ret );
                if( ! sig_alg_ext_found )
                    sig_alg_ext_found = 1;
                else
                {
                    MBEDTLS_SSL_DEBUG_MSG( 3,
                        ( "Duplicate signature algorithms extensions found" ) );
                    goto decode_error;
                }
                break;

            default:
                MBEDTLS_SSL_DEBUG_MSG(
                    3,
                    ( "unknown extension found: %u ( ignoring )",
                    extension_type ) );
                break;
        }
        p += extension_data_len;
    }
    /* Check that we consumed all the message. */
    if( p != end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1,
            ( "CertificateRequest misaligned" ) );
        goto decode_error;
    }
    /* Check that we found signature algorithms extension */
    if( ! sig_alg_ext_found )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3,
            ( "no signature algorithms extension found" ) );
        goto decode_error;
    }

    ssl->handshake->client_auth = 1;
    return( 0 );

decode_error:
    MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR,
                                  MBEDTLS_ERR_SSL_DECODE_ERROR );
    return( MBEDTLS_ERR_SSL_DECODE_ERROR );
}

/*
 * Handler for  MBEDTLS_SSL_CERTIFICATE_REQUEST
 */
static int ssl_tls13_process_certificate_request( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse certificate request" ) );

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_certificate_request_coordinate( ssl ) );

    if( ret == SSL_CERTIFICATE_REQUEST_EXPECT_REQUEST )
    {
        unsigned char *buf;
        size_t buf_len;

        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_tls13_fetch_handshake_msg( ssl,
                                            MBEDTLS_SSL_HS_CERTIFICATE_REQUEST,
                                            &buf, &buf_len ) );

        MBEDTLS_SSL_PROC_CHK( ssl_tls13_parse_certificate_request( ssl,
                                              buf, buf + buf_len ) );

        mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_CERTIFICATE_REQUEST,
                                            buf, buf_len );

#if defined(MBEDTLS_SSL_USE_MPS)
        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_mps_hs_consume_full_hs_msg( ssl ) );
#endif
    }
    else if( ret == SSL_CERTIFICATE_REQUEST_SKIP )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= skip parse certificate request" ) );
        ret = 0;
    }
    else
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        ret = MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        goto cleanup;
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "got %s certificate request",
                                ssl->handshake->client_auth ? "a" : "no" ) );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_CERTIFICATE );

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse certificate request" ) );
    return( ret );
}

/*
 * Handler for MBEDTLS_SSL_SERVER_CERTIFICATE
 */
static int ssl_tls13_process_server_certificate( mbedtls_ssl_context *ssl )
{
    int ret;

    ret = mbedtls_ssl_tls13_process_certificate( ssl );
    if( ret != 0 )
        return( ret );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CERTIFICATE_VERIFY );

    return( 0 );
}

/*
 * Handler for MBEDTLS_SSL_CERTIFICATE_VERIFY
 */
static int ssl_tls13_process_certificate_verify( mbedtls_ssl_context *ssl )
{
    int ret;

    ret = mbedtls_ssl_tls13_process_certificate_verify( ssl );
    if( ret != 0 )
        return( ret );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_FINISHED );
    return( 0 );
}
#endif /* MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED */

/*
 * Handler for MBEDTLS_SSL_SERVER_FINISHED
 */
static int ssl_tls13_process_server_finished( mbedtls_ssl_context *ssl )
{
    int ret;

    ret = mbedtls_ssl_tls13_process_finished_message( ssl );
    if( ret != 0 )
        return( ret );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_END_OF_EARLY_DATA );

    return( 0 );
}

/*
 * Handler for MBEDTLS_SSL_CLIENT_CERTIFICATE
 */
static int ssl_tls13_write_client_certificate( mbedtls_ssl_context *ssl )
{
    int non_empty_certificate_msg = 0;

    MBEDTLS_SSL_DEBUG_MSG( 1,
                  ( "Switch to handshake traffic keys for outbound traffic" ) );

#if defined(MBEDTLS_SSL_USE_MPS)
    {
        int ret;

        /* Use new transform for outgoing data. */
        ret = mbedtls_mps_set_outgoing_keys( &ssl->mps->l4,
                                             ssl->handshake->epoch_handshake );
        if( ret != 0 )
            return( ret );
    }
#else
    mbedtls_ssl_set_outbound_transform( ssl, ssl->handshake->transform_handshake );
#endif /* MBEDTLS_SSL_USE_MPS */

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
    if( ssl->handshake->client_auth )
    {
        int ret = mbedtls_ssl_tls13_write_certificate( ssl );
        if( ret != 0 )
            return( ret );

        if( mbedtls_ssl_own_cert( ssl ) != NULL )
            non_empty_certificate_msg = 1;
    }
    else
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "No certificate message to send." ) );
    }
#endif

   if( non_empty_certificate_msg )
   {
        mbedtls_ssl_handshake_set_state( ssl,
                                         MBEDTLS_SSL_CLIENT_CERTIFICATE_VERIFY );
   }
   else
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_FINISHED );

    return( 0 );
}

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
/*
 * Handler for MBEDTLS_SSL_CLIENT_CERTIFICATE_VERIFY
 */
static int ssl_tls13_write_client_certificate_verify( mbedtls_ssl_context *ssl )
{
    int ret = mbedtls_ssl_tls13_write_certificate_verify( ssl );

    if( ret == 0 )
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_FINISHED );

    return( ret );
}
#endif /* MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED */

/*
 * Handler for MBEDTLS_SSL_CLIENT_FINISHED
 */
static int ssl_tls13_write_client_finished( mbedtls_ssl_context *ssl )
{
    return( mbedtls_ssl_tls13_write_finished_message( ssl ) );
}

/*
 * Handler for MBEDTLS_SSL_FLUSH_BUFFERS
 */
static int ssl_tls13_flush_buffers( mbedtls_ssl_context *ssl )
{
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "handshake: done" ) );
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_HANDSHAKE_WRAPUP );

    return( 0 );
}

/*
 * Handler for MBEDTLS_SSL_HANDSHAKE_WRAPUP
 */
static int ssl_tls13_handshake_wrapup( mbedtls_ssl_context *ssl )
{
    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to application keys for inbound traffic" ) );
    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to application keys for outbound traffic" ) );

#if defined(MBEDTLS_SSL_USE_MPS)
    int ret = 0;

    ret = mbedtls_mps_set_incoming_keys( &ssl->mps->l4,
                                                 ssl->epoch_application );
    if( ret != 0 )
        return( ret );

    ret = mbedtls_mps_set_outgoing_keys( &ssl->mps->l4,
                                         ssl->epoch_application );
    if( ret != 0 )
        return( ret );
#else
    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to application keys for inbound traffic" ) );
    mbedtls_ssl_set_inbound_transform ( ssl, ssl->transform_application );

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to application keys for outbound traffic" ) );
    mbedtls_ssl_set_outbound_transform( ssl, ssl->transform_application );
#endif /* MBEDTLS_SSL_USE_MPS */

    mbedtls_ssl_tls13_handshake_wrapup( ssl );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_HANDSHAKE_OVER );

    return( 0 );
}

/*
 *
 * Handler for MBEDTLS_SSL_CLIENT_NEW_SESSION_TICKET
 *
 */

#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)

static int ssl_tls13_new_session_ticket_early_data_ext_parse(
    mbedtls_ssl_context *ssl,
    const unsigned char *buf, size_t ext_size )
{
    /* From RFC 8446:
     *
     * struct {
     *         select (Handshake.msg_type) {
     *            case new_session_ticket:   uint32 max_early_data_size;
     *            case client_hello:         Empty;
     *            case encrypted_extensions: Empty;
     *        };
     *    } EarlyDataIndication;
     */

    if( ext_size == 4 && ssl->session != NULL )
    {
        ssl->session->max_early_data_size = MBEDTLS_GET_UINT32_BE( buf, 0 );
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "ticket->max_early_data_size: %u",
                                    ssl->session->max_early_data_size ) );
        ssl->session->ticket_flags |= allow_early_data;
        return( 0 );
    }

    return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
}

static int ssl_tls13_new_session_ticket_extensions_parse(
    mbedtls_ssl_context *ssl,
    const unsigned char *buf, size_t buf_remain )
{
    int ret;
    unsigned int ext_id;
    size_t ext_size;

    while( buf_remain != 0 )
    {
        if( buf_remain < 4 )
        {
            /* TODO: Replace with DECODE_ERROR once we have merged 3.0 */
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
        }

        ext_id   = MBEDTLS_GET_UINT16_BE( buf, 0 );
        ext_size = MBEDTLS_GET_UINT16_BE( buf, 2 );

        buf        += 4;
        buf_remain -= 4;

        if( ext_size > buf_remain )
        {
            /* TODO: Replace with DECODE_ERROR once we have merged 3.0 */
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
        }

        if( ext_id == MBEDTLS_TLS_EXT_EARLY_DATA )
        {
            ret = ssl_tls13_new_session_ticket_early_data_ext_parse( ssl, buf,
                                                                     ext_size );
            if( ret != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_new_session_ticket_early_data_ext_parse", ret );
                return( ret );
            }
        }
        /* Ignore other extensions */

        buf        += ext_size;
        buf_remain -= ext_size;
    }

    return( 0 );
}

static int ssl_tls13_new_session_ticket_parse( mbedtls_ssl_context *ssl,
                                               unsigned char *buf,
                                               size_t buf_len )
{
    int ret;
    size_t ticket_len, ext_len;
    unsigned char *ticket;
    const mbedtls_ssl_ciphersuite_t *suite_info;
    size_t used = 0, i = 0;
    int hash_length;
    size_t ticket_nonce_len;
    unsigned char ticket_nonce[256];

    /*
     * struct {
     *    uint32 ticket_lifetime;
     *    uint32 ticket_age_add;
     *    opaque ticket_nonce<0..255>;
     *    opaque ticket<1..2^16-1>;
     *    Extension extensions<0..2^16-2>;
     * } NewSessionTicket;
     *
     */
    used += 4   /* ticket_lifetime */
          + 4   /* ticket_age_add */
          + 1   /* ticket_nonce length */
          + 2   /* ticket length */
          + 2;  /* extension length */

    if( used > buf_len )
    {
         MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad new session ticket message" ) );
         return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    /* Ticket lifetime */
    ssl->session->ticket_lifetime = MBEDTLS_GET_UINT32_BE( buf, i );
    i += 4;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "ticket->lifetime: %u",
                                ssl->session->ticket_lifetime ) );

    /* Ticket Age Add */
    ssl->session->ticket_age_add = MBEDTLS_GET_UINT32_BE( buf, i );
    i += 4;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "ticket->ticket_age_add: %u",
                                ssl->session->ticket_age_add ) );

    ticket_nonce_len = buf[i];
    i++;

    used += ticket_nonce_len;

    if( used > buf_len )
    {
         MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad new session ticket message" ) );
         return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    if( ticket_nonce_len > 0 )
    {
        if( ticket_nonce_len > sizeof( ticket_nonce )  )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "ticket_nonce is too small" ) );
            return( MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE );
        }

        memcpy( ticket_nonce, &buf[i], ticket_nonce_len );

        MBEDTLS_SSL_DEBUG_BUF( 3, "nonce:", &buf[i],
                               ticket_nonce_len );

    }
    i += ticket_nonce_len;

    /* Ticket */
    ticket_len = MBEDTLS_GET_UINT16_BE( buf, i );
    i += 2;

    used += ticket_len;

    if( used > buf_len )
    {
         MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad new session ticket message" ) );
         return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "ticket->length: %" MBEDTLS_PRINTF_SIZET , ticket_len ) );

    /* Check if we previously received a ticket already. */
    if( ssl->session->ticket != NULL || ssl->session->ticket_len > 0 )
    {
        mbedtls_free( ssl->session->ticket );
        ssl->session->ticket = NULL;
        ssl->session->ticket_len = 0;
    }

    if( ( ticket = mbedtls_calloc( 1, ticket_len ) ) == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "ticket alloc failed" ) );
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    memcpy( ticket, buf + i, ticket_len );
    i += ticket_len;
    ssl->session->ticket = ticket;
    ssl->session->ticket_len = ticket_len;

    MBEDTLS_SSL_DEBUG_BUF( 4, "ticket", ticket, ticket_len );


    /* Ticket Extension */
    ext_len = MBEDTLS_GET_UINT16_BE( buf, i );
    i += 2;

    used += ext_len;
    if( used != buf_len )
    {
         MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad new session ticket message" ) );
         return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    MBEDTLS_SSL_DEBUG_BUF( 3, "ticket->extension", &buf[i], ext_len );

    ret = ssl_tls13_new_session_ticket_extensions_parse( ssl, &buf[i], ext_len );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_new_session_ticket_extensions_parse", ret );
        return( ret );
    }
    i += ext_len;

    /* Compute PSK based on received nonce and resumption_master_secret
     * in the following style:
     *
     *  HKDF-Expand-Label( resumption_master_secret,
     *                    "resumption", ticket_nonce, Hash.length )
     */

    suite_info = mbedtls_ssl_ciphersuite_from_id( ssl->session->ciphersuite );

    if( suite_info == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    hash_length = mbedtls_hash_size_for_ciphersuite( suite_info );

    if( hash_length == -1 )
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

    MBEDTLS_SSL_DEBUG_BUF( 3, "resumption_master_secret",
                           ssl->session->app_secrets.resumption_master_secret,
                           hash_length );

    /* Computer resumption key
     *
     *  HKDF-Expand-Label( resumption_master_secret,
     *                    "resumption", ticket_nonce, Hash.length )
     */
    ret = mbedtls_ssl_tls13_hkdf_expand_label(
                    mbedtls_psa_translate_md( suite_info->mac ),
                    ssl->session->app_secrets.resumption_master_secret,
                    hash_length,
                    MBEDTLS_SSL_TLS1_3_LBL_WITH_LEN( resumption ),
                    ticket_nonce,
                    ticket_nonce_len,
                    ssl->session->key,
                    hash_length );

    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 2, "Creating the ticket-resumed PSK failed", ret );
        return( ret );
    }

    ssl->session->key_len = hash_length;

    MBEDTLS_SSL_DEBUG_BUF( 3, "Ticket-resumed PSK", ssl->session->key,
                           ssl->session->key_len );

#if defined(MBEDTLS_HAVE_TIME)
    /* Store ticket creation time */
    ssl->session->ticket_received = time( NULL );
#endif

    return( 0 );
}

static int ssl_tls13_new_session_ticket_postprocess( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_HANDSHAKE_OVER );
    return( 0 );
}

/* The ssl_tls13_new_session_ticket_process( ) function is used by the
 * client to process the NewSessionTicket message, which contains
 * the ticket and meta-data provided by the server in a post-
 * handshake message.
 */

static int ssl_tls13_new_session_ticket_process( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char *buf;
    size_t buf_len;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse new session ticket" ) );

    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_tls13_fetch_handshake_msg(
                              ssl, MBEDTLS_SSL_HS_NEW_SESSION_TICKET,
                              &buf, &buf_len ) );

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_new_session_ticket_parse( ssl, buf, buf_len ) );

#if defined(MBEDTLS_SSL_USE_MPS)
    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_mps_hs_consume_full_hs_msg( ssl ) );
#endif /* MBEDTLS_SSL_USE_MPS */

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_new_session_ticket_postprocess( ssl ) );

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse new session ticket" ) );
    return( ret );
}
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */

/*
 * TLS and DTLS 1.3 State Maschine -- client side
 */
int mbedtls_ssl_tls13_handshake_client_step( mbedtls_ssl_context *ssl )
{
    int ret = 0;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "tls13 client state: %s(%d)",
                                mbedtls_ssl_states_str( ssl->state ),
                                ssl->state ) );

    switch( ssl->state )
    {
        case MBEDTLS_SSL_HELLO_REQUEST:
            mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_HELLO );
            break;

        /*
         *  ==>   ClientHello
         *        (EarlyData)
         */
        case MBEDTLS_SSL_CLIENT_HELLO:
            ret = mbedtls_ssl_write_client_hello( ssl );
            break;

        case MBEDTLS_SSL_EARLY_APP_DATA:
            ret = ssl_tls13_write_early_data_process( ssl );
            break;

        /*
         *  <==   ServerHello / HelloRetryRequest
         *        EncryptedExtensions
         *        (CertificateRequest)
         *        (Certificate)
         *        (CertificateVerify)
         *        Finished
         */
        case MBEDTLS_SSL_SERVER_HELLO:
            ret = ssl_tls13_process_server_hello( ssl );
            break;

        case MBEDTLS_SSL_ENCRYPTED_EXTENSIONS:
            ret = ssl_tls13_process_encrypted_extensions( ssl );
            break;

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
        case MBEDTLS_SSL_CERTIFICATE_REQUEST:
            ret = ssl_tls13_process_certificate_request( ssl );
            break;

        case MBEDTLS_SSL_SERVER_CERTIFICATE:
            ret = ssl_tls13_process_server_certificate( ssl );
            break;

        case MBEDTLS_SSL_CERTIFICATE_VERIFY:
            ret = ssl_tls13_process_certificate_verify( ssl );
            break;
#endif /* MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED */

        case MBEDTLS_SSL_SERVER_FINISHED:
            ret = ssl_tls13_process_server_finished( ssl );
            break;

        /*
         *  ==>   (EndOfEarlyData)
         *        (Certificate)
         *        (CertificateVerify)
         *        (Finished)
         */
        case MBEDTLS_SSL_END_OF_EARLY_DATA:
            ret = ssl_tls13_write_end_of_early_data_process( ssl );
            break;

        case MBEDTLS_SSL_CLIENT_CERTIFICATE:
            ret = ssl_tls13_write_client_certificate( ssl );
            break;

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
        case MBEDTLS_SSL_CLIENT_CERTIFICATE_VERIFY:
            ret = ssl_tls13_write_client_certificate_verify( ssl );
            break;
#endif /* MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED */

        case MBEDTLS_SSL_CLIENT_FINISHED:
            ret = ssl_tls13_write_client_finished( ssl );
            break;

        /*
         *  <==   NewSessionTicket
         */
#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)
        case MBEDTLS_SSL_CLIENT_NEW_SESSION_TICKET:

            ret = ssl_tls13_new_session_ticket_process( ssl );
            if( ret != 0 )
                break;

            ret = MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET;
            break;
#endif

        /*
         * Injection of dummy-CCS's for middlebox compatibility
         */
#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
        case MBEDTLS_SSL_CLIENT_CCS_AFTER_CLIENT_HELLO:
            ret = mbedtls_ssl_tls13_write_change_cipher_spec( ssl );
            if( ret == 0 )
                mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_EARLY_APP_DATA );
            break;

        case MBEDTLS_SSL_CLIENT_CCS_BEFORE_2ND_CLIENT_HELLO:
            ret = mbedtls_ssl_tls13_write_change_cipher_spec( ssl );
            if( ret == 0 )
                mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_HELLO );
            break;

        case MBEDTLS_SSL_CLIENT_CCS_AFTER_SERVER_FINISHED:
            ret = mbedtls_ssl_tls13_write_change_cipher_spec( ssl );
            if( ret == 0 )
                mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_CERTIFICATE );
            break;
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

        /*
         * Internal intermediate states
         */

        case MBEDTLS_SSL_FLUSH_BUFFERS:
            ret = ssl_tls13_flush_buffers( ssl );
            break;

        case MBEDTLS_SSL_HANDSHAKE_WRAPUP:
            ret = ssl_tls13_handshake_wrapup( ssl );
            break;

        default:
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "invalid state %d", ssl->state ) );
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    return( ret );
}

#endif /* MBEDTLS_SSL_CLI_C && MBEDTLS_SSL_PROTO_TLS1_3 */
