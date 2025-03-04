/*
 *  TLS 1.3 server-side functions
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
 */

#include "common.h"

#if defined(MBEDTLS_SSL_SRV_C) && defined(MBEDTLS_SSL_PROTO_TLS1_3)

#define SSL_DONT_FORCE_FLUSH 0
#define SSL_FORCE_FLUSH      1

#include "mbedtls/debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "mbedtls/constant_time.h"

#include "ssl_misc.h"
#include "ssl_tls13_keys.h"
#include "ssl_debug_helpers.h"

#if defined(MBEDTLS_SSL_USE_MPS)
#include "mps_all.h"
#endif /* MBEDTLS_SSL_USE_MPS */

#include <string.h>

#if defined(MBEDTLS_ECP_C)
#include "mbedtls/ecp.h"
#include "ecp_internal.h"
#endif /* MBEDTLS_ECP_C */

#include "mbedtls/hkdf.h"

#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)
#include "mbedtls/ssl_ticket.h"
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */

#if defined(MBEDTLS_PLATFORM_C)
#include "mbedtls/platform.h"
#else
#include <stdlib.h>
#define mbedtls_calloc    calloc
#define mbedtls_free       free
#endif /* MBEDTLS_PLATFORM_C */

#if defined(MBEDTLS_HAVE_TIME)
#include <time.h>
#endif /* MBEDTLS_HAVE_TIME */


#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
static int ssl_tls13_write_sni_server_ext(
    mbedtls_ssl_context *ssl,
    unsigned char *buf,
    size_t buflen,
    size_t *olen )
{
    unsigned char *p = buf;
    *olen = 0;

    if( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_SERVERNAME ) == 0 )
    {
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "adding server_name extension" ) );

    if( buflen < 4 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "buffer too small" ) );
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    }

    /* Write extension header */
    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_SERVERNAME, p, 0 );

    /* Write total extension length */
    MBEDTLS_PUT_UINT16_BE( 0, p, 2 );

    *olen = 4;

    return( 0 );
}
#endif /* MBEDTLS_SSL_SERVER_NAME_INDICATION */


/* Key Shares Extension
 *
 * enum {
 *     ... (0xFFFF)
 * } NamedGroup;
 *
 * struct {
 *     NamedGroup group;
 *     opaque key_exchange<1..2^16-1>;
 * } KeyShareEntry;
 *
 * struct {
 *     select(role) {
 *     ...
 *     case server:
 *         KeyShareEntry server_share;
 *     }
 * } KeyShare;
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

    if( ecdh_bits > 0xffff )
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    ssl->handshake->ecdh_bits = (uint16_t) ecdh_bits;

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

    if( own_pubkey_len > (size_t)( end - buf ) )
    {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "No space in the buffer for ECDH public key." ) );
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    }

    *out_len = own_pubkey_len;

    return( 0 );
}
#endif /* MBEDTLS_ECDH_C */

static int ssl_tls13_write_key_shares_ext(
    mbedtls_ssl_context *ssl,
    unsigned char *buf,
    unsigned char *end,
    size_t *olen )
{
    size_t ext_len = 0;

    *olen = 0;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "server hello, adding key share extension" ) );

    if( end - buf < 8 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "buffer too small" ) );
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    }

#if defined(MBEDTLS_ECDH_C)
    /* When we introduce PQC-ECDHE hybrids, we'll want to call this
     * function multiple times. */
    int ret;
    unsigned char *key_share_entry = buf + 4;
    unsigned char *key_share       = buf + 8;
    size_t share_len;

    ret = ssl_tls13_generate_and_write_ecdh_key_exchange(
              ssl, ssl->handshake->offered_group_id,
              key_share, end, &share_len );
    if( ret != 0 )
        return( ret );

    /* Write group ID */
    MBEDTLS_PUT_UINT16_BE( ssl->handshake->offered_group_id, key_share_entry, 0 );
    /* Write key share length */
    MBEDTLS_PUT_UINT16_BE( share_len, key_share_entry, 2 );
    key_share_entry += 4;

    ext_len = share_len + 4;
#endif /* MBEDTLS_ECDH_C */

    /* Write extension header */
    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_KEY_SHARE, buf, 0 );

    /* Write total extension length */
    MBEDTLS_PUT_UINT16_BE( ext_len, buf, 2 );

    *olen = ext_len + 4; /* 4 bytes for fixed header + length of key share */
    return( 0 );
}

#if ( defined(MBEDTLS_ECDH_C) || defined(MBEDTLS_ECDSA_C) )
/* This function parses the TLS 1.3 supported_groups extension and
 * stores the received groups in ssl->handshake->curves.
 *
 * From RFC 8446:
 *   enum {
 *       ... (0xFFFF)
 *   } NamedGroup;
 *   struct {
 *       NamedGroup named_group_list<2..2^16-1>;
 *   } NamedGroupList;
 */
int mbedtls_ssl_tls13_parse_supported_groups_ext(
    mbedtls_ssl_context *ssl,
    const unsigned char *buf, size_t len ) {

    size_t list_size, our_size;
    const unsigned char *p;
    const mbedtls_ecp_curve_info *curve_info, **curves;

    MBEDTLS_SSL_DEBUG_BUF( 3, "supported_groups extension", buf, len );
    list_size = MBEDTLS_GET_UINT16_BE( buf, 0 );
    if( list_size + 2 != len || list_size % 2 != 0 )
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );

    /* TODO: At the moment, this can happen when receiving a second
     *       ClientHello after an HRR. We should properly reset the
     *       state upon receiving an HRR, in which case we should
     *       not observe handshake->curves already being allocated. */
    if( ssl->handshake->curves != NULL )
    {
        mbedtls_free( ssl->handshake->curves );
        ssl->handshake->curves = NULL;
    }

    /* Don't allow our peer to make us allocate too much memory,
     * and leave room for a final 0 */
    our_size = list_size / 2 + 1;
    if( our_size > MBEDTLS_ECP_DP_MAX )
        our_size = MBEDTLS_ECP_DP_MAX;

    if( ( curves = mbedtls_calloc( our_size, sizeof( *curves ) ) ) == NULL )
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

    ssl->handshake->curves = curves;

    p = buf + 2;
    while ( list_size > 0 && our_size > 1 )
    {
        uint16_t tls_grp_id = MBEDTLS_GET_UINT16_BE( p, 0 );
        curve_info = mbedtls_ecp_curve_info_from_tls_id( tls_grp_id );

        /* mbedtls_ecp_curve_info_from_tls_id() uses the mbedtls_ecp_curve_info
         * data structure (defined in ecp.c), which only includes the list of
         * curves implemented. Hence, we only add curves that are also supported
         * and implemented by the server. */
        if( curve_info != NULL )
        {
            *curves++ = curve_info;
            MBEDTLS_SSL_DEBUG_MSG( 4, ( "supported curve: %s", curve_info->name ) );
            our_size--;
        }

        list_size -= 2;
        p += 2;
    }

    return( 0 );

}
#endif /* MBEDTLS_ECDH_C || ( MBEDTLS_ECDSA_C */

#if defined(MBEDTLS_ZERO_RTT)
static int ssl_tls13_parse_early_data_ext( mbedtls_ssl_context *ssl,
                                           const unsigned char *buf,
                                           size_t len )
{
    ((void) ssl);
    ((void) buf);
    /* From RFC 8446:
     *  struct {} Empty;
     *  struct {
     *     select (Handshake.msg_type) {
     *         case new_session_ticket:   uint32 max_early_data_size;
     *         case client_hello:         Empty;
     *         case encrypted_extensions: Empty;
     *     };
     * } EarlyDataIndication;
     */
    if( len != 0 )
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    return( 0 );
}
#endif /* MBEDTLS_ZERO_RTT */

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

#if ( defined(MBEDTLS_ECDH_C) || defined(MBEDTLS_ECDSA_C) )

/* TODO: Code for MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED missing */
/*
 *  ssl_tls13_parse_key_shares_ext() verifies whether the information in the
 *  extension is correct and stores the provided key shares. Whether this is an
 *  acceptable key share depends on the selected ciphersuite.
 *
 *  Possible return values are:
 *  - 0: Successful processing of the client provided key share extension.
 *  - MBEDTLS_ERR_SSL_BAD_HS_WRONG_KEY_SHARE: The key share provided by the client
 *    does not match a group supported by the server. A HelloRetryRequest will
 *    be needed.
 *  - Another negative return value for fatal errors.
*/

static int ssl_tls13_parse_key_shares_ext( mbedtls_ssl_context *ssl,
                                           const unsigned char *buf,
                                           size_t len )
{
    int ret = 0;
    unsigned char const *p = buf;

    size_t total_ext_len, cur_share_len;
    int match_found = 0;

    /* From RFC 8446:
     *
     * struct {
     *     KeyShareEntry client_shares<0..2^16-1>;
     * } KeyShareClientHello;
     *
     */

    /* Read total legnth of KeyShareClientHello */
    if( len < 2 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Invalid key share extension" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    total_ext_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p   += 2;
    len -= 2;

    if( total_ext_len != len )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Invalid key share extension" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    ssl->handshake->offered_group_id = 0;

    /* We try to find a suitable key share entry and copy it to the
     * handshake context. Later, we have to find out whether we can do
     * something with the provided key share or whether we have to
     * dismiss it and send a HelloRetryRequest message. */

    for( ; len > 0; p += cur_share_len, len -= cur_share_len )
    {
        uint16_t their_group;
        mbedtls_ecp_group_id their_curve;
        mbedtls_ecp_curve_info const *their_curve_info;
        unsigned char const *end_of_share;

        /*
         * struct {
         *    NamedGroup group;
         *    opaque key_exchange<1..2^16-1>;
         * } KeyShareEntry;
         */

        if( len < 4 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Invalid key share extension" ) );
            return( MBEDTLS_ERR_SSL_DECODE_ERROR );
        }

        their_group = MBEDTLS_GET_UINT16_BE( p, 0 );
        p   += 2;
        len -= 2;

        cur_share_len = MBEDTLS_GET_UINT16_BE( p, 0 );
        p   += 2;
        len -= 2;

        if( len < cur_share_len )
            return( MBEDTLS_ERR_SSL_DECODE_ERROR );

        end_of_share = p + cur_share_len;

        /* Continue parsing even if we have already found a match,
         * for input validation purposes. */
        if( match_found == 1 )
            continue;

        /*
         * NamedGroup matching
         *
         * For now, we only support ECDHE groups, but e.g.
         * PQC KEMs will need to be added at a later stage.
         */

        /* Type 1: ECDHE shares
         *
         * - Check if we recognize the group
         * - Check if it's supported
         */

        their_curve = mbedtls_ecp_named_group_to_id( their_group );
        if( mbedtls_ssl_check_curve( ssl, their_curve ) != 0 )
            continue;

        /* Type 2..X: Other kinds of shares */
        /* TO BE ADDED */

        /* Skip if we no match succeeded. */
        if( their_curve == MBEDTLS_ECP_DP_NONE )
        {
            MBEDTLS_SSL_DEBUG_MSG( 4, ( "Unrecognized NamedGroup %u",
                                        (unsigned) their_group ) );
            continue;
        }

        match_found = 1;

        /* KeyShare parsing
         *
         * Once we add more key share types, this needs to be a switch
         * over the (type of) the named curve */

        /* Type 1: ECDHE shares
         *
         * - Setup ECDHE context
         * - Import client's public key
         * - Apply further curve checks
         */

        their_curve_info = mbedtls_ecp_curve_info_from_grp_id( their_curve );
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "ECDH curve: %s", their_curve_info->name ) );

        ret = ssl_tls13_read_public_ecdhe_share( ssl, p - 2, end_of_share - p + 2 );
        if( ret != 0 )
            return( ret );

        ssl->handshake->offered_group_id = their_group;
    }

    if( match_found == 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "no matching key share" ) );
        return( MBEDTLS_ERR_SSL_HRR_REQUIRED );
    }

    return( 0 );
}
#endif /* MBEDTLS_ECDH_C || MBEDTLS_ECDSA_C */

#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)
int mbedtls_ssl_tls13_parse_new_session_ticket_server(
    mbedtls_ssl_context *ssl,
    unsigned char *buf,
    size_t len )
{
    int ret;
    unsigned char *ticket_buffer;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse new session ticket" ) );

    if( ssl->conf->f_ticket_parse == NULL ||
        ssl->conf->f_ticket_write == NULL )
    {
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "ticket length: %" MBEDTLS_PRINTF_SIZET , len ) );

    if( len == 0 ) return( 0 );

    /* We create a copy of the encrypted ticket since decrypting
     * it into the same buffer will wipe-out the original content.
     * We do, however, need the original buffer for computing the
     * psk binder value.
     */
    ticket_buffer = mbedtls_calloc( len,1 );

    if( ticket_buffer == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "buffer too small" ) );
        return ( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    memcpy( ticket_buffer, buf, len );

    if( ( ret = ssl->conf->f_ticket_parse( ssl->conf->p_ticket, ssl->session_negotiate,
                                         ticket_buffer, len ) ) != 0 )
    {
        mbedtls_free( ticket_buffer );
        if( ret == MBEDTLS_ERR_SSL_INVALID_MAC )
            MBEDTLS_SSL_DEBUG_MSG( 3, ( "ticket is not authentic" ) );
        else if( ret == MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED )
            MBEDTLS_SSL_DEBUG_MSG( 3, ( "ticket is expired" ) );
        else
            MBEDTLS_SSL_DEBUG_RET( 1, "ticket_parse", ret );

        return( ret );
    }

    /* We delete the temporary buffer */
    mbedtls_free( ticket_buffer );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse new session ticket" ) );

    return( 0 );
}
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */

#if defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)
int mbedtls_ssl_tls13_parse_client_psk_identity_ext(
    mbedtls_ssl_context *ssl,
    const unsigned char *buf,
    size_t len )
{
    int ret = 0;
    unsigned int item_array_length, item_length, sum, length_so_far;
    unsigned char server_computed_binder[MBEDTLS_MD_MAX_SIZE];
    const unsigned char *psk = NULL;
    unsigned char const * const start = buf;
    size_t psk_len = 0;
    unsigned char const *end_of_psk_identities;

    unsigned char transcript[MBEDTLS_MD_MAX_SIZE];
    size_t transcript_len;

#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)
    uint32_t obfuscated_ticket_age;
#if defined(MBEDTLS_HAVE_TIME)
    time_t now;
    int64_t diff;
#endif /* MBEDTLS_HAVE_TIME */
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */

    /* Read length of array of identities */
    item_array_length = MBEDTLS_GET_UINT16_BE( buf, 0 );
    length_so_far = item_array_length + 2;
    if( length_so_far > len )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad psk_identity extension in client hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }
    end_of_psk_identities = buf + length_so_far;
    buf += 2;
    sum = 2;
    while( sum < item_array_length + 2 )
    {
        /* Read to psk identity length */
        item_length = MBEDTLS_GET_UINT16_BE( buf, 0 );
        sum = sum + 2 + item_length;

        if( sum > len )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "psk_identity length mismatch" ) );

            if( ( ret = mbedtls_ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
                return( ret );

            return( MBEDTLS_ERR_SSL_DECODE_ERROR );
        }

        /*
         * Extract pre-shared key identity provided by the client
         */
        /* jump to identity value itself */
        buf += 2;

        MBEDTLS_SSL_DEBUG_BUF( 3, "received psk identity", buf, item_length );

        if( ssl->conf->f_psk != NULL )
        {
            if( ssl->conf->f_psk( ssl->conf->p_psk, ssl, buf, item_length ) != 0 )
                ret = MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY;
        }
        else
        {
            /* Identity is not a big secret since clients send it in the clear,
             * but treat it carefully anyway, just in case */
            if( item_length != ssl->conf->psk_identity_len ||
                mbedtls_ct_memcmp( ssl->conf->psk_identity, buf, item_length ) != 0 )
            {
                ret = MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY;
            }
            else
            {
                /* skip obfuscated ticket age */
                /* TBD: Process obfuscated ticket age ( zero for externally configured PSKs?! ) */
                buf = buf + item_length + 4; /* 4 for obfuscated ticket age */;

                mbedtls_ssl_set_hs_psk( ssl, ssl->conf->psk, ssl->conf->psk_len );
                goto psk_parsing_successful;

            }
#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)
            /* Check the ticket cache if previous lookup was unsuccessful */
            if( ret == MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY )
            {
                /* copy ticket since it acts as the psk_identity */
                if( ssl->session_negotiate->ticket != NULL )
                {
                    mbedtls_free( ssl->session_negotiate->ticket );
                }
                ssl->session_negotiate->ticket = mbedtls_calloc( 1, item_length );
                if( ssl->session_negotiate->ticket == NULL )
                {
                    MBEDTLS_SSL_DEBUG_MSG( 1, ( "alloc failed ( %d bytes )", item_length ) );
                    return( MBEDTLS_ERR_SSL_ALLOC_FAILED );
                }
                memcpy( ssl->session_negotiate->ticket, buf, item_length );
                ssl->session_negotiate->ticket_len = item_length;

                ret = mbedtls_ssl_tls13_parse_new_session_ticket_server( ssl,
                                             ssl->session_negotiate->ticket,
                                             item_length );
                if( ret == 0 )
                {
                    /* found a match in the ticket cache; everything is OK */
                    ssl->handshake->resume = 1;

                    /* We put the resumption key into the handshake->psk.
                     *
                     * Note: The key in the ticket is already the final PSK,
                     *       i.e., the HKDF-Expand-Label( resumption_master_secret,
                     *                                    "resumption",
                     *                                    ticket_nonce,
                     *                                    Hash.length )
                     *       function has already been applied.
                     */
                    mbedtls_ssl_set_hs_psk( ssl, ssl->session_negotiate->key,
                                            ssl->session_negotiate->key_len );
                    MBEDTLS_SSL_DEBUG_BUF( 4, "Ticket-resumed PSK:", ssl->session_negotiate->key,
                                           ssl->session_negotiate->key_len );

                    /* obfuscated ticket age follows the identity field, which is
                     * item_length long, containing the ticket */
                    memcpy( &obfuscated_ticket_age, buf+item_length, 4 );

                    MBEDTLS_SSL_DEBUG_MSG( 4, ( "ticket: obfuscated_ticket_age: %u",
                                                obfuscated_ticket_age ) );
                    /*
                     * A server MUST validate that the ticket age for the selected PSK identity
                     * is within a small tolerance of the time since the ticket was issued.
                     */

#if defined(MBEDTLS_HAVE_TIME)
                    now = time( NULL );

                    /* Check #1:
                     *   Is the time when the ticket was issued later than now?
                     */

                    if( now < ssl->session_negotiate->start )
                    {
                        MBEDTLS_SSL_DEBUG_MSG( 3,
                               ( "Ticket expired: now=%ld, ticket.start=%ld",
                                 now, ssl->session_negotiate->start ) );
                        ret = MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED;
                    }

                    /* Check #2:
                     *   Is the ticket expired already?
                     */

                    if( now - ssl->session_negotiate->start > ssl->session_negotiate->ticket_lifetime )
                    {
                        MBEDTLS_SSL_DEBUG_MSG( 3,
                               ( "Ticket expired ( now - ticket.start=%ld, "\
                                 "ticket.ticket_lifetime=%d",
                                 now - ssl->session_negotiate->start,
                                 ssl->session_negotiate->ticket_lifetime ) );

                        ret = MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED;
                    }

                    /* Check #3:
                     *   Is the ticket age for the selected PSK identity
                     *   (computed by subtracting ticket_age_add from
                     *   PskIdentity.obfuscated_ticket_age modulo 2^32 )
                     *   within a small tolerance of the time since the
                     *   ticket was issued?
                     */

                    diff = ( now - ssl->session_negotiate->start ) -
                        ( obfuscated_ticket_age - ssl->session_negotiate->ticket_age_add );

                    if( diff > MBEDTLS_SSL_TICKET_AGE_TOLERANCE )
                    {
                        MBEDTLS_SSL_DEBUG_MSG( 3,
                            ( "Ticket age outside tolerance window ( diff=%ld )",
                              diff ) );
                        ret = MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED;
                    }

#if defined(MBEDTLS_ZERO_RTT)
                    if( ssl->conf->early_data_enabled == MBEDTLS_SSL_EARLY_DATA_ENABLED )
                    {
                        if( diff <= MBEDTLS_SSL_EARLY_DATA_MAX_DELAY )
                        {
                            ssl->session_negotiate->process_early_data =
                                MBEDTLS_SSL_EARLY_DATA_ENABLED;
                        }
                        else
                        {
                            MBEDTLS_SSL_DEBUG_MSG( 3,
                            ( "0-RTT is disabled ( diff=%ld exceeds "\
                              "MBEDTLS_SSL_EARLY_DATA_MAX_DELAY )", diff ) );
                            ssl->session_negotiate->process_early_data =
                                MBEDTLS_SSL_EARLY_DATA_DISABLED;
                            ret = MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED;
                        }
                    }
#endif /* MBEDTLS_ZERO_RTT */
#endif /* MBEDTLS_HAVE_TIME */

                    /* TBD: check ALPN, ciphersuite and SNI as well */

                    /*
                     * If the check failed, the server SHOULD proceed with
                     * the handshake but reject 0-RTT, and SHOULD NOT take any
                     * other action that assumes that this ClientHello is fresh.
                     */

                    /* Disable 0-RTT */
                    if( ret == MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED )
                    {
#if defined(MBEDTLS_ZERO_RTT)
                        if( ssl->conf->early_data_enabled ==
                            MBEDTLS_SSL_EARLY_DATA_ENABLED )
                        {
                            ssl->session_negotiate->process_early_data =
                                MBEDTLS_SSL_EARLY_DATA_DISABLED;
                        }
#else
                        ( ( void )buf );
#endif /* MBEDTLS_ZERO_RTT */
                    }
                }
            }
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */
        }
        /* skip the processed identity field and obfuscated ticket age field */
        buf += item_length;
        buf += 4;
        sum = sum + 4;
    }

    if( ret == MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Session ticket expired." ) );
        return( ret );
    }

    if( ret == MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY )
    {
        MBEDTLS_SSL_DEBUG_BUF( 3, "Unknown PSK identity", buf, item_length );
        if( ( ret = mbedtls_ssl_send_alert_message( ssl,
                   MBEDTLS_SSL_ALERT_LEVEL_FATAL,
                   MBEDTLS_SSL_ALERT_MSG_UNKNOWN_PSK_IDENTITY ) ) != 0 )
        {
            return( ret );
        }

        return( MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY );
    }

    if( length_so_far != sum )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad psk_identity extension in client hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

psk_parsing_successful:

    /* Update the handshake transcript with the CH content up to
     * but excluding the PSK binder list. */
    ssl->handshake->update_checksum( ssl, start,
                                     (size_t)( end_of_psk_identities - start ) );

    buf = end_of_psk_identities;

    /* Get current state of handshake transcript. */
    ret = mbedtls_ssl_get_handshake_transcript( ssl,
                                                ssl->handshake->ciphersuite_info->mac,
                                                transcript, sizeof( transcript ),
                                                &transcript_len );
    if( ret != 0 )
        return( ret );

    /* read length of psk binder array */
    item_array_length = MBEDTLS_GET_UINT16_BE( buf, 0 );
    length_so_far += item_array_length;
    buf += 2;

    sum = 0;
    while( sum < item_array_length )
    {
        int psk_type;
        /* Read to psk binder length */
        item_length = buf[0];
        sum = sum + 1 + item_length;
        buf += 1;

        if( sum > item_array_length )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "psk binder length mismatch" ) );

            if( ( ret = mbedtls_ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
                return( ret );

            return( MBEDTLS_ERR_SSL_DECODE_ERROR );
        }

        psk = ssl->handshake->psk;
        psk_len = ssl->handshake->psk_len;

        if( ssl->handshake->resume == 1 )
            psk_type = MBEDTLS_SSL_TLS1_3_PSK_RESUMPTION;
        else
            psk_type = MBEDTLS_SSL_TLS1_3_PSK_EXTERNAL;

        ret = mbedtls_ssl_tls13_create_psk_binder( ssl,
                 mbedtls_psa_translate_md( ssl->handshake->ciphersuite_info->mac ),
                 psk, psk_len, psk_type,
                 transcript, server_computed_binder );

        /* We do not check for multiple binders */
        if( ret != 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "PSK binder calculation failed." ) );
            return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
        }

        MBEDTLS_SSL_DEBUG_BUF( 3, "psk binder ( computed ): ",
                               server_computed_binder, item_length );
        MBEDTLS_SSL_DEBUG_BUF( 3, "psk binder ( received ): ",
                               buf, item_length );

        if( mbedtls_ct_memcmp( server_computed_binder, buf, item_length ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1,
                ( "Received psk binder does not match computed psk binder." ) );

            if( ( ret = mbedtls_ssl_send_fatal_handshake_failure( ssl ) ) != 0 )
                return( ret );

            return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
        }

        buf += item_length;

        ret = 0;
        goto done;
    }

    /* No valid PSK binder value found */
    /* TODO: Shouldn't we just fall back to a full handshake in this case? */
    ret = MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE;

done:

    /* Update the handshake transcript with the binder list. */
    ssl->handshake->update_checksum( ssl,
                                     end_of_psk_identities,
                                     (size_t)( buf - end_of_psk_identities ) );

    return( ret );
}
#endif /* MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED*/

#if defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)
/*
 * struct {
 *   select ( Handshake.msg_type ) {
 *      case client_hello:
 *          PskIdentity identities<6..2^16-1>;
 *
 *      case server_hello:
 *          uint16 selected_identity;
 *   }
 * } PreSharedKeyExtension;
 */

static int ssl_tls13_write_server_pre_shared_key_ext( mbedtls_ssl_context *ssl,
                                                      unsigned char *buf,
                                                      unsigned char *end,
                                                      size_t *olen )
{
    unsigned char *p = (unsigned char*)buf;
    size_t selected_identity;

    *olen = 0;

    if( ssl->handshake->psk == NULL )
    {
        /* We shouldn't have called this extension writer unless we've
         * chosen to use a PSK. */
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "server hello, adding pre_shared_key extension" ) );

    if( end < p || ( end - p ) < 6 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "buffer too small" ) );
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    }

    /* Extension Type */
    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_PRE_SHARED_KEY, p, 0 );

    /* Extension Length */
    MBEDTLS_PUT_UINT16_BE( 2, p, 2 );

    /* NOTE: This will need to be adjusted once we support multiple PSKs
     *       being offered by the client. */
    selected_identity = 0;

    /* Write selected_identity */
    MBEDTLS_PUT_UINT16_BE( selected_identity, p, 4 );

    *olen = 6;

    MBEDTLS_SSL_DEBUG_MSG( 4, ( "sent selected_identity: %" MBEDTLS_PRINTF_SIZET , selected_identity ) );

    return( 0 );
}
#endif	/* MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED  */

#if defined(MBEDTLS_SSL_COOKIE_C)
void mbedtls_ssl_conf_cookies( mbedtls_ssl_config *conf,
                               mbedtls_ssl_cookie_write_t *f_cookie_write,
                               mbedtls_ssl_cookie_check_t *f_cookie_check,
                               void *p_cookie,
                               unsigned int rr_config )
{
    conf->f_cookie_write = f_cookie_write;
    conf->f_cookie_check = f_cookie_check;
    conf->p_cookie = p_cookie;
    conf->rr_config = rr_config;
}
#endif /* MBEDTLS_SSL_COOKIE_C */

#if defined(MBEDTLS_SSL_COOKIE_C)
static int ssl_tls13_parse_cookie_ext( mbedtls_ssl_context *ssl,
                                       const unsigned char *buf,
                                       size_t len )
{
    int ret = 0;
    size_t cookie_len;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "parse cookie extension" ) );

    if( ssl->conf->f_cookie_check != NULL )
    {
        if( len >= 2 )
        {
            cookie_len = MBEDTLS_GET_UINT16_BE( buf, 0 );
            buf += 2;
        }
        else
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message - cookie length mismatch" ) );
            return( MBEDTLS_ERR_SSL_DECODE_ERROR );
        }

        if( cookie_len + 2 != len )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message - cookie length mismatch" ) );
            return( MBEDTLS_ERR_SSL_DECODE_ERROR );
        }

        MBEDTLS_SSL_DEBUG_BUF( 3, "Received cookie", buf, cookie_len );

        if( ssl->conf->f_cookie_check( ssl->conf->p_cookie,
                      buf, cookie_len, ssl->cli_id, ssl->cli_id_len ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "cookie verification failed" ) );
            ret = MBEDTLS_ERR_SSL_HRR_REQUIRED;
        }
        else
        {
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "cookie verification passed" ) );
        }
    }
    else {
        /* TBD: Check under what cases this is appropriate */
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "cookie verification skipped" ) );
    }

    return( ret );
}
#endif /* MBEDTLS_SSL_COOKIE_C */



#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
static int ssl_tls13_parse_servername_ext( mbedtls_ssl_context *ssl,
                                           const unsigned char *buf,
                                           size_t len )
{
    int ret;
    size_t servername_list_size, hostname_len;
    const unsigned char *p;

    if( ssl->conf->p_sni == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "No SNI callback configured. Skip SNI parsing." ) );
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "Parse ServerName extension" ) );

    servername_list_size = MBEDTLS_GET_UINT16_BE( buf, 0 );
    if( servername_list_size + 2 != len )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    p = buf + 2;
    while ( servername_list_size > 0 )
    {
        hostname_len = MBEDTLS_GET_UINT16_BE( p, 1 );
        if( hostname_len + 3 > servername_list_size )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
            return( MBEDTLS_ERR_SSL_DECODE_ERROR );
        }

        if( p[0] == MBEDTLS_TLS_EXT_SERVERNAME_HOSTNAME )
        {
            ret = ssl->conf->f_sni( ssl->conf->p_sni,
                                   ssl, p + 3, hostname_len );
            if( ret != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "sni_wrapper", ret );
                mbedtls_ssl_send_alert_message( ssl, MBEDTLS_SSL_ALERT_LEVEL_FATAL,
                                               MBEDTLS_SSL_ALERT_MSG_UNRECOGNIZED_NAME );
                return( MBEDTLS_ERR_SSL_UNRECOGNIZED_NAME );
            }
            return( 0 );
        }

        servername_list_size -= hostname_len + 3;
        p += hostname_len + 3;
    }

    if( servername_list_size != 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    return( 0 );
}
#endif /* MBEDTLS_SSL_SERVER_NAME_INDICATION */


#if defined(MBEDTLS_ZERO_RTT)
/*
  static int ssl_tls13_parse_early_data_ext( mbedtls_ssl_context *ssl,
  const unsigned char *buf,
  size_t len )
  {
  ( ( void* )ssl );
  ( ( void* )buf );
  return( 0 );
  }
*/
#endif /* MBEDTLS_ZERO_RTT */

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
static int ssl_tls13_parse_max_fragment_length_ext( mbedtls_ssl_context *ssl,
                                                    const unsigned char *buf,
                                                    size_t len )
{
    if( len != 1 )
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    if( buf[0] >= MBEDTLS_SSL_MAX_FRAG_LEN_INVALID )
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );

    ssl->session_negotiate->mfl_code = buf[0];
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "Maximum fragment length = %d", buf[0] ) );

    return( 0 );
}
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */

#if defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)
/* From RFC 8446:
 *
 *   enum { psk_ke(0), psk_dhe_ke(1), (255) } PskKeyExchangeMode;
 *   struct {
 *       PskKeyExchangeMode ke_modes<1..255>;
 *   } PskKeyExchangeModes;
 */
static int ssl_tls13_parse_key_exchange_modes_ext( mbedtls_ssl_context *ssl,
                                                   const unsigned char *buf,
                                                   size_t len )
{
    size_t ke_modes_len;
    int ke_modes = 0;

    /* Read PSK mode list length (1 Byte) */
    ke_modes_len = *buf++;
    len--;

    /* There's no content after the PSK mode list, to its length
     * must match the total length of the extension. */
    if( ke_modes_len != len )
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );

    /* Currently, there are only two PSK modes, so even without looking
     * at the content, something's wrong if the list has more than 2 items. */
    if( ke_modes_len > 2 )
        return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );

    while( ke_modes_len-- != 0 )
    {
        switch( *buf )
        {
        case MBEDTLS_SSL_TLS1_3_PSK_MODE_PURE:
            ke_modes |= MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK;
            break;
        case MBEDTLS_SSL_TLS1_3_PSK_MODE_ECDHE:
            ke_modes |= MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL;
            break;
        default:
            return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
        }
    }

    ssl->handshake->tls13_kex_modes = ke_modes;
    return( 0 );
}
#endif /* MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED */

/* From RFC 8446:
 *   struct {
 *       select (Handshake.msg_type) {
 *           case client_hello:
 *                ProtocolVersion versions<2..254>;
 *           case server_hello: // and HelloRetryRequest
 *                ProtocolVersion selected_version;
 *       };
 *   } SupportedVersions;
 */
static int ssl_tls13_write_supported_version_ext( mbedtls_ssl_context *ssl,
                                                  unsigned char *buf,
                                                  unsigned char *end,
                                                  size_t *olen )
{
    unsigned char *p = buf;
    *olen = 0;

    /* With only a single supported version we do not need the ssl structure. */
    ((void) ssl);

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "adding supported version extension" ) );

    if( end < p || (size_t)( end - p ) < 6 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "buffer too small" ) );
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    }

    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_SUPPORTED_VERSIONS, p, 0 );

    /* length */
    MBEDTLS_PUT_UINT16_BE( 2, p, 2 );

    /* For TLS 1.3 and for DTLS 1.3 we use 0x0304 */
    MBEDTLS_PUT_UINT16_BE( 0x0304, p, 4 );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "version [3:4]" ) );

    *olen = 6;

    return( 0 );
}

/* From RFC 8446:
 *   struct {
 *       select (Handshake.msg_type) {
 *           case client_hello:
 *                ProtocolVersion versions<2..254>;
 *           case server_hello: // and HelloRetryRequest
 *                ProtocolVersion selected_version;
 *       };
 *   } SupportedVersions;
 */
static int ssl_tls13_parse_supported_versions_ext( mbedtls_ssl_context *ssl,
                                                   const unsigned char *buf,
                                                   size_t len )
{
    size_t list_len;
    int tls13_supported = 0;

    if( len < 1 )
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );

    list_len = buf[0];
    len -= 1;
    buf += 1;

    if( len != list_len || list_len % 2 != 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Invalid supported version list length %" MBEDTLS_PRINTF_SIZET,
                                    list_len ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    while( len > 0 )
    {
        

        /* In this implementation we only support TLS 1.3 and DTLS 1.3. */
        if( mbedtls_ssl_read_version( buf, ssl->conf->transport ) ==
            MBEDTLS_SSL_VERSION_TLS1_3 )
        {
            tls13_supported = 1;
            break;
        }

        buf += 2;
        len -= 2;
    }

    if( ! tls13_supported )
    {
        /* When we support runtime negotiation of TLS 1.2 and TLS 1.3, we need
         * a graceful fallback to TLS 1.2 in this case. */

        MBEDTLS_SSL_DEBUG_MSG( 1, ( "TLS 1.3 is not supported by the client" ) );

        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_PROTOCOL_VERSION,
                                      MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION );
        return( MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION );
    }

    ssl->tls_version = MBEDTLS_SSL_VERSION_TLS1_3;

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Negotiated version. Supported is [%04x]",      
                              ssl->tls_version ) );

#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)
    /* Store minor version for later use with ticket serialization. */
    ssl->session_negotiate->tls_version = MBEDTLS_SSL_VERSION_TLS1_3;
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */

    return( 0 );
}

#if defined(MBEDTLS_SSL_ALPN)
static int ssl_tls13_parse_alpn_ext( mbedtls_ssl_context *ssl,
                                     const unsigned char *buf, size_t len )
{
    const unsigned char *end = buf + len;
    size_t list_len;

    const char **cur_ours;
    const unsigned char *cur_cli;
    size_t cur_cli_len;

    /* If ALPN not configured, just ignore the extension */
    if( ssl->conf->alpn_list == NULL )
        return( 0 );

    /*
     * opaque ProtocolName<1..2^8-1>;
     *
     * struct {
     *     ProtocolName protocol_name_list<2..2^16-1>
     * } ProtocolNameList;
     */

    if( len < 2 )
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    list_len = MBEDTLS_GET_UINT16_BE( buf, 0 );
    if( list_len != len - 2 )
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );

    buf += 2;
    len -= 2;

    /* Validate peer's list (lengths) */
    for( cur_cli = buf; cur_cli != end; cur_cli += cur_cli_len )
    {
        cur_cli_len = *cur_cli++;
        if( cur_cli_len > (size_t)( end - cur_cli ) )
            return( MBEDTLS_ERR_SSL_DECODE_ERROR );
        if( cur_cli_len == 0 )
            return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    /* Use our order of preference */
    for( cur_ours = ssl->conf->alpn_list; *cur_ours != NULL; cur_ours++ )
    {
        size_t const cur_ours_len = strlen( *cur_ours );
        for( cur_cli = buf; cur_cli != end; cur_cli += cur_cli_len )
        {
            cur_cli_len = *cur_cli++;

            if( cur_cli_len == cur_ours_len &&
                memcmp( cur_cli, *cur_ours, cur_ours_len ) == 0 )
            {
                ssl->alpn_chosen = *cur_ours;
                return( 0 );
            }
        }
    }

    /* If we get hhere, no match was found */
    MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_LEVEL_FATAL,
                                  MBEDTLS_SSL_ALERT_MSG_NO_APPLICATION_PROTOCOL );
    return( MBEDTLS_ERR_SSL_NO_APPLICATION_PROTOCOL );
}
#endif /* MBEDTLS_SSL_ALPN */

/*
 *
 * STATE HANDLING: NewSessionTicket message
 *
 */
#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)

/* Main state-handling entry point; orchestrates the other functions. */
static int ssl_tls13_write_new_session_ticket_process( mbedtls_ssl_context *ssl );

#define SSL_NEW_SESSION_TICKET_SKIP  0
#define SSL_NEW_SESSION_TICKET_WRITE 1

static int ssl_tls13_write_new_session_ticket_coordinate( mbedtls_ssl_context *ssl );

static int ssl_tls13_write_new_session_ticket_write( mbedtls_ssl_context *ssl,
                                                     unsigned char *buf,
                                                     size_t buflen,
                                                     size_t *olen );

/* Update the state after handling the incoming end of early data message. */
static int ssl_tls13_write_new_session_ticket_postprocess( mbedtls_ssl_context *ssl );

/*
 * Implementation
 */
static int ssl_tls13_write_new_session_ticket_process( mbedtls_ssl_context *ssl )
{
    int ret = 0;

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_write_new_session_ticket_coordinate( ssl ) );

    if( ret == SSL_NEW_SESSION_TICKET_WRITE )
    {
        unsigned char *buf;
        size_t buf_len, msg_len;

        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_start_handshake_msg( ssl,
                MBEDTLS_SSL_HS_NEW_SESSION_TICKET, &buf, &buf_len ) );

        MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_new_session_ticket_write(
                                  ssl, buf, buf_len, &msg_len ) );

        MBEDTLS_SSL_PROC_CHK(
            ssl_tls13_write_new_session_ticket_postprocess( ssl ) );

        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_finish_handshake_msg(
                                  ssl, buf_len, msg_len ) );
    }
    else
    {
        MBEDTLS_SSL_PROC_CHK(
            ssl_tls13_write_new_session_ticket_postprocess( ssl ) );
    }

cleanup:

    return( ret );
}

static int ssl_tls13_write_new_session_ticket_coordinate( mbedtls_ssl_context *ssl )
{
    /* Check whether the use of session tickets is enabled */
    if( ssl->conf->session_tickets == 0 )
    {
        return( SSL_NEW_SESSION_TICKET_SKIP );
    }

    return( SSL_NEW_SESSION_TICKET_WRITE );
}


static int ssl_tls13_write_new_session_ticket_postprocess( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_NEW_SESSION_TICKET_FLUSH );
    return( 0 );
}

/* This function creates a NewSessionTicket message in the following format:
 *
 * struct {
 *    uint32 ticket_lifetime;
 *    uint32 ticket_age_add;
 *    opaque ticket_nonce<0..255>;
 *    opaque ticket<1..2^16-1>;
 *    Extension extensions<0..2^16-2>;
 * } NewSessionTicket;
 *
 * The ticket inside the NewSessionTicket message is an encrypted container
 * carrying the necessary information so that the server is later able to
 * re-start the communication.
 *
 * The following fields are placed inside the ticket by the
 * f_ticket_write() function:
 *
 *  - creation time (start)
 *  - flags (flags)
 *  - lifetime (ticket_lifetime)
 *  - age add (ticket_age_add)
 *  - key (key)
 *  - key length (key_len)
 *  - ciphersuite (ciphersuite)
 *  - certificate of the peer (peer_cert)
 *
 */
static int ssl_tls13_write_new_session_ticket_write( mbedtls_ssl_context *ssl,
                                                     unsigned char *buf,
                                                     size_t buflen,
                                                     size_t *olen )
{
    int ret;
    size_t tlen;
    size_t ext_len = 0;
    unsigned char *p;
    unsigned char *end = buf + buflen;
    mbedtls_ssl_ciphersuite_t *suite_info;
    int hash_length;
    unsigned char *ticket_lifetime_ptr;

    size_t const total_length = 12 + MBEDTLS_SSL_TICKET_NONCE_LENGTH;
    p = buf;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write NewSessionTicket msg" ) );

    /* Do we have space for the fixed length part of the ticket */
    if( buflen < total_length )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Buffer for ticket too small" ) );
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    }

    suite_info = (mbedtls_ssl_ciphersuite_t *) ssl->handshake->ciphersuite_info;

    hash_length = mbedtls_hash_size_for_ciphersuite( suite_info );

    if( hash_length == -1 )
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

    /* In this code the psk key length equals the length of the hash */
    ssl->session->key_len = hash_length;
    ssl->session->ciphersuite = ssl->handshake->ciphersuite_info->id;

    /* Ticket Lifetime
     * (write it later)
     */
    ticket_lifetime_ptr = p;
    p+=4;

    /* Ticket Age Add */
    if( ( ret = ssl->conf->f_rng( ssl->conf->p_rng,
                                  (unsigned char*) &ssl->session->ticket_age_add,
                                  sizeof( ssl->session->ticket_age_add ) ) != 0 ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Failed to generate ticket" ) );
        return( ret );
    }

    MBEDTLS_PUT_UINT32_BE( ssl->session->ticket_age_add, p, 0 );
    p += 4;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "ticket->ticket_age_add: %u", ssl->session->ticket_age_add ) );

    /* Ticket Nonce */
    *(p++) = MBEDTLS_SSL_TICKET_NONCE_LENGTH;

    ret = ssl->conf->f_rng( ssl->conf->p_rng, p,
                            MBEDTLS_SSL_TICKET_NONCE_LENGTH );
    if( ret != 0 )
        return( ret );

    MBEDTLS_SSL_DEBUG_BUF( 3, "ticket_nonce:",
                           p, MBEDTLS_SSL_TICKET_NONCE_LENGTH );

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
               (const unsigned char *) p,
               MBEDTLS_SSL_TICKET_NONCE_LENGTH,
               ssl->session->key,
               hash_length );

    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 2, "Creating the ticket-resumed PSK failed", ret );
        return ( ret );
    }

    p += MBEDTLS_SSL_TICKET_NONCE_LENGTH;

    ssl->session->key_len = hash_length;

    MBEDTLS_SSL_DEBUG_BUF( 3, "Ticket-resumed PSK",
                           ssl->session->key, hash_length );

    /* Ticket */
    ret = ssl->conf->f_ticket_write( ssl->conf->p_ticket,
                                     ssl->session,
                                     p + 2, end,
                                     &tlen,
                                     &ssl->session->ticket_lifetime);
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "f_ticket_write", ret );
        return( ret );
    }

    /* Ticket lifetime */
    MBEDTLS_PUT_UINT16_BE( ssl->session->ticket_lifetime,
                           ticket_lifetime_ptr, 0 );
    ticket_lifetime_ptr += 4;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "ticket->ticket_lifetime: %d",
                               ssl->session->ticket_lifetime ) );

    /* Ticket Length */
    MBEDTLS_PUT_UINT16_BE( tlen, p, 0 );
    p += 2 + tlen;

    /* Ticket Extensions
     *
     * Note: We currently don't have any extensions.
     * Set length to zero.
     */
    MBEDTLS_PUT_UINT16_BE( ext_len, p, 0 );
    p += 2;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "NewSessionTicket (extension_length): %" MBEDTLS_PRINTF_SIZET , ext_len ) );

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "NewSessionTicket (ticket length): %" MBEDTLS_PRINTF_SIZET , tlen ) );

    *olen = p - buf;

    MBEDTLS_SSL_DEBUG_BUF( 4, "ticket", buf, *olen );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write new session ticket" ) );

    return( ret );
}
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */

/*
 *
 * STATE HANDLING: Parse End-of-Early Data
 *
 */

 /*
  * Overview
  */

  /* Main state-handling entry point; orchestrates the other functions. */
int ssl_tls13_read_end_of_early_data_process( mbedtls_ssl_context *ssl );

#define SSL_END_OF_EARLY_DATA_SKIP   0
#define SSL_END_OF_EARLY_DATA_EXPECT 1

static int ssl_tls13_read_end_of_early_data_coordinate( mbedtls_ssl_context *ssl );

#if defined(MBEDTLS_ZERO_RTT)
static int ssl_tls13_end_of_early_data_fetch( mbedtls_ssl_context *ssl );
#endif /* MBEDTLS_ZERO_RTT */

/* Update the state after handling the incoming end of early data message. */
static int ssl_tls13_read_end_of_early_data_postprocess( mbedtls_ssl_context *ssl );

/*
 * Implementation
 */

int ssl_tls13_read_end_of_early_data_process( mbedtls_ssl_context *ssl )
{
    int ret;
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse end_of_early_data" ) );

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_read_end_of_early_data_coordinate( ssl ) );
    if( ret == SSL_END_OF_EARLY_DATA_EXPECT )
    {
#if defined(MBEDTLS_ZERO_RTT)

#if defined(MBEDTLS_SSL_USE_MPS)
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_end_of_early_data_fetch( ssl ) );
        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_read_consume( &ssl->mps->l4 ) );
#else /* MBEDTLS_SSL_USE_MPS */
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_end_of_early_data_fetch( ssl ) );
#endif /* MBEDTLS_SSL_USE_MPS */

        mbedtls_ssl_add_hs_hdr_to_checksum( ssl, MBEDTLS_SSL_HS_END_OF_EARLY_DATA, 0 );

#else /* MBEDTLS_ZERO_RTT */

        /* Should never happen */
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

#endif /* MBEDTLS_ZERO_RTT */

    }

    /* Postprocessing step: Update state machine */
    MBEDTLS_SSL_PROC_CHK( ssl_tls13_read_end_of_early_data_postprocess( ssl ) );

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse end_of_early_data" ) );
    return( ret );

}

#if defined(MBEDTLS_ZERO_RTT)

#if defined(MBEDTLS_SSL_USE_MPS)
static int ssl_tls13_end_of_early_data_fetch( mbedtls_ssl_context *ssl )
{
    int ret;
    mbedtls_mps_handshake_in msg;

    MBEDTLS_SSL_PROC_CHK_NEG( mbedtls_mps_read( &ssl->mps->l4 ) );

    if( ret != MBEDTLS_MPS_MSG_HS )
        return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );

    MBEDTLS_SSL_PROC_CHK( mbedtls_mps_read_handshake( &ssl->mps->l4,
                                                      &msg ) );

    if( msg.type != MBEDTLS_SSL_HS_END_OF_EARLY_DATA ||
        msg.length != 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, (
             "Bad EndOfEarlyData message: Type %u (expect %u), "
             "Length %u (expect %u)",
             (unsigned) msg.type, MBEDTLS_SSL_HS_END_OF_EARLY_DATA,
             (unsigned) msg.length, 0 ) );
        return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
    }

cleanup:

    return( ret );
}
#else /* MBEDTLS_SSL_USE_MPS */
static int ssl_tls13_end_of_early_data_fetch( mbedtls_ssl_context *ssl )
{
    int ret;

    if( ( ret = mbedtls_ssl_read_record( ssl, 0 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_read_record", ret );
        goto cleanup;
    }

    if( ssl->in_msgtype != MBEDTLS_SSL_MSG_HANDSHAKE        ||
        ssl->in_msg[0]  != MBEDTLS_SSL_HS_END_OF_EARLY_DATA ||
        ssl->in_hslen   != 4 )
    {
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_UNEXPECTED_MESSAGE,
                                      MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
        ret = MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE;
        goto cleanup;
    }

cleanup:

    return( ret );
}
#endif /* MBEDTLS_SSL_USE_MPS */

#endif /* MBEDTLS_ZERO_RTT */

#if !defined(MBEDTLS_ZERO_RTT)
static int ssl_tls13_read_end_of_early_data_coordinate( mbedtls_ssl_context *ssl )
{
    ((void) ssl);
    return( SSL_END_OF_EARLY_DATA_SKIP );
}
#else /* MBEDTLS_ZERO_RTT */
static int ssl_tls13_read_end_of_early_data_coordinate( mbedtls_ssl_context *ssl )
{
    if( ssl->handshake->early_data != MBEDTLS_SSL_EARLY_DATA_ON )
        return( SSL_END_OF_EARLY_DATA_SKIP );

    return( SSL_END_OF_EARLY_DATA_EXPECT );
}
#endif /* MBEDTLS_ZERO_RTT */

static int ssl_tls13_read_end_of_early_data_postprocess( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_CERTIFICATE );
    return ( 0 );
}

/*
 *
 * STATE HANDLING: Parse Early Data
 *
 */

 /*
  * Overview
  */

  /* Main state-handling entry point; orchestrates the other functions. */
int ssl_tls13_read_early_data_process( mbedtls_ssl_context *ssl );

#define SSL_EARLY_DATA_SKIP   0
#define SSL_EARLY_DATA_EXPECT 1

#if defined(MBEDTLS_ZERO_RTT)
#if defined(MBEDTLS_SSL_USE_MPS)
static int ssl_tls13_early_data_fetch( mbedtls_ssl_context *ssl,
                                       mbedtls_mps_reader **reader );
#else
static int ssl_tls13_early_data_fetch( mbedtls_ssl_context *ssl,
                                       unsigned char **buf,
                                       size_t *buflen );
#endif /* MBEDTLS_SSL_USE_MPS */
#endif /* MBEDTLS_ZERO_RTT */

static int ssl_tls13_read_early_data_coordinate( mbedtls_ssl_context *ssl );

#if defined(MBEDTLS_ZERO_RTT)
/* Parse early data send by the peer. */
static int ssl_tls13_read_early_data_parse( mbedtls_ssl_context *ssl,
    unsigned char const *buf,
    size_t buflen );
#endif /* MBEDTLS_ZERO_RTT */

/* Update the state after handling the incoming early data message. */
static int ssl_tls13_read_early_data_postprocess( mbedtls_ssl_context *ssl );

/*
 * Implementation
 */

int ssl_tls13_read_early_data_process( mbedtls_ssl_context *ssl )
{
    int ret;
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse early data" ) );

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_read_early_data_coordinate( ssl ) );

    if( ret == SSL_EARLY_DATA_EXPECT )
    {
#if defined(MBEDTLS_ZERO_RTT)
        unsigned char *buf;
        size_t buflen;
#if defined(MBEDTLS_SSL_USE_MPS)
        mbedtls_mps_reader *rd;
#endif /* MBEDTLS_SSL_USE_MPS */

#if defined(MBEDTLS_SSL_USE_MPS)
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_early_data_fetch( ssl, &rd ) );
        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_reader_get( rd,
                                                  MBEDTLS_MPS_SIZE_MAX,
                                                  &buf,
                                                  &buflen ) );
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_read_early_data_parse( ssl, buf, buflen ) );
        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_reader_commit( rd ) );
        MBEDTLS_SSL_PROC_CHK( mbedtls_mps_read_consume( &ssl->mps->l4 ) );

#else /* MBEDTLS_SSL_USE_MPS */

        MBEDTLS_SSL_PROC_CHK( ssl_tls13_early_data_fetch( ssl, &buf, &buflen ) );
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_read_early_data_parse( ssl, buf, buflen ) );

#endif /* MBEDTLS_SSL_USE_MPS */

        /* No state machine update at this point -- we might receive
         * multiple 0-RTT messages. */

#else /* MBEDTLS_ZERO_RTT */

        /* Should never happen */
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

#endif /* MBEDTLS_ZERO_RTT */
    }
    else
    {
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_read_early_data_postprocess( ssl ) );
    }

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse early data" ) );
    return( ret );
}

#if defined(MBEDTLS_ZERO_RTT)
#if defined(MBEDTLS_SSL_USE_MPS)
static int ssl_tls13_early_data_fetch( mbedtls_ssl_context *ssl,
                                       mbedtls_mps_reader **rd )
{
    int ret;
    MBEDTLS_SSL_PROC_CHK_NEG( mbedtls_mps_read( &ssl->mps->l4 ) );

    if( ret != MBEDTLS_MPS_MSG_APP )
        return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );

    MBEDTLS_SSL_PROC_CHK( mbedtls_mps_read_application( &ssl->mps->l4, rd ) );

cleanup:

    return( ret );
}
#else /* MBEDTLS_SSL_USE_MPS */
static int ssl_tls13_early_data_fetch( mbedtls_ssl_context *ssl,
                                       unsigned char **buf,
                                       size_t *buflen )
{
    int ret;

    if( ( ret = mbedtls_ssl_read_record( ssl, 0 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_read_record", ret );
        goto cleanup;
    }

    if( ssl->in_msgtype != MBEDTLS_SSL_MSG_APPLICATION_DATA )
    {
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_UNEXPECTED_MESSAGE,
                                      MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
        ret = MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE;
        goto cleanup;
    }

    *buf    = ssl->in_msg;
    *buflen = ssl->in_hslen;

cleanup:

    return( ret );
}
#endif /* MBEDTLS_SSL_USE_MPS */
#endif /* MBEDTLS_ZERO_RTT */

#if !defined(MBEDTLS_ZERO_RTT)
static int ssl_tls13_read_early_data_coordinate( mbedtls_ssl_context *ssl )
{
    ((void) ssl);
    return( SSL_EARLY_DATA_SKIP );
}
#else /* MBEDTLS_ZERO_RTT */
static int ssl_tls13_read_early_data_coordinate( mbedtls_ssl_context *ssl )
{
    int ret;

    if( ssl->handshake->early_data != MBEDTLS_SSL_EARLY_DATA_ON )
        return( SSL_EARLY_DATA_SKIP );

    /* Activate early data transform */
    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to 0-RTT keys for inbound traffic" ) );

#if defined(MBEDTLS_SSL_USE_MPS)
    MBEDTLS_SSL_PROC_CHK( mbedtls_mps_set_incoming_keys( &ssl->mps->l4,
                                                   ssl->handshake->epoch_earlydata ) );

    MBEDTLS_SSL_PROC_CHK_NEG( mbedtls_mps_read( &ssl->mps->l4 ) );
    if( ret != MBEDTLS_MPS_MSG_APP )
        return( SSL_EARLY_DATA_SKIP );

    return( SSL_EARLY_DATA_EXPECT );

cleanup:

    return( ret );

#else /* MBEDTLS_SSL_USE_MPS */

    mbedtls_ssl_set_inbound_transform( ssl, ssl->handshake->transform_earlydata );

    /* Fetching step */
    if( ( ret = mbedtls_ssl_read_record( ssl, 0 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_read_record", ret );
        return( ret );
    }

    ssl->keep_current_message = 1;

    /* Check for EndOfEarlyData */
    if( ssl->in_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE )
        return( SSL_EARLY_DATA_SKIP );

    return( SSL_EARLY_DATA_EXPECT );

#endif /* MBEDTLS_SSL_USE_MPS */
}

static int ssl_tls13_read_early_data_parse( mbedtls_ssl_context *ssl,
                                            unsigned char const *buf,
                                            size_t buflen )
{
    /* Check whether we have enough buffer space. */
    if( buflen <= ssl->conf->max_early_data )
    {
        /* TODO: We need to check that we're not receiving more 0-RTT
         * than what the ticket allows. */

        /* copy data to staging area */
        memcpy( ssl->early_data_server_buf, buf, buflen );
        /* execute callback to process application data */
        ssl->conf->early_data_callback( ssl, ssl->early_data_server_buf,
                                        buflen );
    }
    else
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Buffer too small (recv %" MBEDTLS_PRINTF_SIZET " bytes, buffer %" MBEDTLS_PRINTF_SIZET " bytes)",
                                    buflen, ssl->conf->max_early_data ) );
        return ( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    return( 0 );
}
#endif /* MBEDTLS_ZERO_RTT */

static int ssl_tls13_read_early_data_postprocess( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_END_OF_EARLY_DATA );
    return ( 0 );
}


/*
 *
 * STATE HANDLING: ClientHello
 *
 * There are three possible classes of outcomes when parsing the CH:
 *
 * 1) The CH was well-formed and matched the server's configuration.
 *
 *    In this case, the server progresses to sending its ServerHello.
 *
 * 2) The CH was well-formed but didn't match the server's configuration.
 *
 *    For example, the client might not have offered a key share which
 *    the server supports, or the server might require a cookie.
 *
 *    In this case, the server sends a HelloRetryRequest.
 *
 * 3) The CH was ill-formed
 *
 *    In this case, we abort the handshake.
 *
 */

/*
 * Overview
 */

/* Main entry point from the state machine; orchestrates the otherfunctions. */
static int ssl_tls13_client_hello_process( mbedtls_ssl_context *ssl );

static int ssl_tls13_client_hello_parse( mbedtls_ssl_context *ssl,
                                         unsigned char *buf,
                                         size_t buflen );

/* Update the handshake state machine */
/* TODO: At the moment, this doesn't update the state machine - why? */
static int ssl_tls13_client_hello_postprocess( mbedtls_ssl_context *ssl,
                                               int hrr_required );

/*
 * Implementation
 */

#define SSL_CLIENT_HELLO_OK           0
#define SSL_CLIENT_HELLO_HRR_REQUIRED 1

static int ssl_tls13_client_hello_process( mbedtls_ssl_context *ssl )
{

    int ret = 0;
    int hrr_required = SSL_CLIENT_HELLO_OK;
    unsigned char *buf = NULL;
    size_t buflen = 0;
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse client hello" ) );

    ssl->tls_version = MBEDTLS_SSL_VERSION_TLS1_3;
    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_tls13_fetch_handshake_msg(
                              ssl, MBEDTLS_SSL_HS_CLIENT_HELLO,
                              &buf, &buflen ) );

    mbedtls_ssl_add_hs_hdr_to_checksum( ssl, MBEDTLS_SSL_HS_CLIENT_HELLO, buflen );

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_client_hello_parse( ssl, buf, buflen ) );
    hrr_required = ret;

#if defined(MBEDTLS_SSL_USE_MPS)
    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_mps_hs_consume_full_hs_msg( ssl ) );
#endif /* MBEDTLS_SSL_USE_MPS */

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "postprocess" ) );
    MBEDTLS_SSL_PROC_CHK( ssl_tls13_client_hello_postprocess( ssl, hrr_required ) );

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse client hello" ) );
    return( ret );
}

static void ssl_tls13_debug_print_client_hello_exts( mbedtls_ssl_context *ssl )
{
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "Supported Extensions:" ) );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- KEY_SHARE_EXTENSION ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_KEY_SHARE ) > 0 ) ?
                                "TRUE" : "FALSE" ) );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- PSK_KEY_EXCHANGE_MODES_EXTENSION ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_PSK_KEY_EXCHANGE_MODES ) > 0 ) ?
                                "TRUE" : "FALSE" ) );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- PRE_SHARED_KEY_EXTENSION ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_PRE_SHARED_KEY ) > 0 ) ?
                                "TRUE" : "FALSE" ) );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- SIGNATURE_ALGORITHM_EXTENSION ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_SIG_ALG ) > 0 ) ?
                                "TRUE" : "FALSE" ) );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- SUPPORTED_GROUPS_EXTENSION ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_SUPPORTED_GROUPS ) >0 ) ?
                                "TRUE" : "FALSE" ) );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- SUPPORTED_VERSION_EXTENSION ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_SUPPORTED_VERSIONS ) > 0 ) ?
                                "TRUE" : "FALSE" ) );
#if defined ( MBEDTLS_SSL_SERVER_NAME_INDICATION )
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- SERVERNAME_EXTENSION    ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_SERVERNAME ) > 0 ) ?
                                "TRUE" : "FALSE" ) );
#endif /* MBEDTLS_SSL_SERVER_NAME_INDICATION */
#if defined ( MBEDTLS_SSL_ALPN )
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- ALPN_EXTENSION   ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_ALPN ) > 0 ) ?
                                "TRUE" : "FALSE" ) );
#endif /* MBEDTLS_SSL_ALPN */
#if defined ( MBEDTLS_SSL_MAX_FRAGMENT_LENGTH )
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- MAX_FRAGMENT_LENGTH_EXTENSION  ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_MAX_FRAGMENT_LENGTH ) > 0 ) ?
                                "TRUE" : "FALSE" ) );
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */
#if defined ( MBEDTLS_SSL_COOKIE_C )
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- COOKIE_EXTENSION ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_COOKIE ) >0 ) ?
                                "TRUE" : "FALSE" ) );
#endif /* MBEDTLS_SSL_COOKIE_C */
#if defined(MBEDTLS_ZERO_RTT)
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "- EARLY_DATA_EXTENSION ( %s )",
                                ( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_EARLY_DATA ) > 0 ) ?
                                "TRUE" : "FALSE" ) );
#endif /* MBEDTLS_ZERO_RTT*/
}

static int ssl_tls13_client_hello_has_exts( mbedtls_ssl_context *ssl,
                                            int ext_id_mask )
{
    int masked = ssl->handshake->extensions_present & ext_id_mask;
    return( masked == ext_id_mask );
}

static int ssl_tls13_client_hello_has_psk_extensions( mbedtls_ssl_context *ssl )
{
    return( ssl_tls13_client_hello_has_exts( ssl,
                MBEDTLS_SSL_EXT_PRE_SHARED_KEY |
                MBEDTLS_SSL_EXT_PSK_KEY_EXCHANGE_MODES ) );
}

static int ssl_tls13_client_hello_has_key_share_extensions( mbedtls_ssl_context *ssl )
{
    return( ssl_tls13_client_hello_has_exts( ssl,
                          MBEDTLS_SSL_EXT_SUPPORTED_GROUPS |
                          MBEDTLS_SSL_EXT_KEY_SHARE ) );
}

static int ssl_tls13_client_hello_has_cert_extensions( mbedtls_ssl_context *ssl )
{
    return( ssl_tls13_client_hello_has_exts( ssl,
                          MBEDTLS_SSL_EXT_SUPPORTED_GROUPS |
                          MBEDTLS_SSL_EXT_KEY_SHARE        |
                          MBEDTLS_SSL_EXT_SIG_ALG ) );
}

static int ssl_tls13_check_psk_key_exchange( mbedtls_ssl_context *ssl )
{
    if( !ssl_tls13_client_hello_has_psk_extensions( ssl ) )
        return( 0 );

    /* Test whether pure PSK is offered by client and supported by us. */
    if( mbedtls_ssl_conf_tls13_psk_enabled( ssl ) &&
        mbedtls_ssl_tls13_psk_enabled( ssl ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "Using a PSK key exchange" ) );
        ssl->handshake->key_exchange = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK;
        return( 1 );
    }

    /* Test whether PSK-ephemeral is offered by client and supported by us. */
    if( mbedtls_ssl_conf_tls13_psk_ephemeral_enabled( ssl ) &&
        mbedtls_ssl_tls13_psk_ephemeral_enabled( ssl ) &&
        ssl_tls13_client_hello_has_key_share_extensions( ssl ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "Using a ECDHE-PSK key exchange" ) );
        ssl->handshake->key_exchange = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL;
        return( 1 );
    }

    /* Can't use PSK */
    return( 0 );
}

static int ssl_tls13_check_certificate_key_exchange( mbedtls_ssl_context *ssl )
{
    if( !mbedtls_ssl_conf_tls13_ephemeral_enabled( ssl ) )
        return( 0 );

    if( !ssl_tls13_client_hello_has_cert_extensions( ssl ) )
        return( 0 );

    ssl->handshake->key_exchange = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL;
    return( 1 );
}

#if defined(MBEDTLS_ZERO_RTT)
static int ssl_tls13_check_use_0rtt_handshake( mbedtls_ssl_context *ssl )
{
    /* Check if the user has enabled 0-RTT in the config */
    if( !mbedtls_ssl_conf_tls13_0rtt_enabled( ssl ) )
        return( 0 );

    /* Check if the client has indicated the use of 0-RTT */
    if( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_EARLY_DATA ) == 0 )
        return( 0 );

    /* If the client has indicated the use of 0-RTT but not sent
     * the PSK extensions, that's not conformant (and there's no
     * way to continue from here). */
    if( !ssl_tls13_client_hello_has_psk_extensions( ssl ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1,
            ( "Client indicated 0-RTT without offering PSK extensions" ) );
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    /* Accept 0-RTT */
    ssl->handshake->early_data = MBEDTLS_SSL_EARLY_DATA_ON;
    return( 0 );
}
#endif /* MBEDTLS_ZERO_RTT*/

static int ssl_tls13_client_hello_parse( mbedtls_ssl_context *ssl,
                                         unsigned char *buf,
                                         size_t buflen )
{
    int ret;
    size_t i, j;
    size_t comp_len, sess_len;
    size_t ciph_len, ext_len, ext_len_psk_ext = 0;
    unsigned char *orig_buf, *end = buf + buflen;
    unsigned char *ciph_offset;
    unsigned char *p = NULL;
    unsigned char *ext = NULL;
    unsigned char *ext_psk_ptr = NULL;

    const int *ciphersuites;
    const mbedtls_ssl_ciphersuite_t *ciphersuite_info;

    int hrr_required = 0;

    ssl->handshake->extensions_present = MBEDTLS_SSL_EXT_NONE;
    ssl->handshake->key_exchange = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_NONE;

    /* TBD: Refactor */
    orig_buf = buf;

    /*
     * ClientHello layer:
     *     0  .   1   protocol version
     *     2  .  33   random bytes ( starting with 4 bytes of Unix time )
     *    34  .  35   session id length ( 1 byte )
     *    35  . 34+x  session id
     *   35+x . 35+x  DTLS only: cookie length ( 1 byte )
     *   36+x .  ..   DTLS only: cookie
     *    ..  .  ..   ciphersuite list length ( 2 bytes )
     *    ..  .  ..   ciphersuite list
     *    ..  .  ..   compression alg. list length ( 1 byte )
     *    ..  .  ..   compression alg. list
     *    ..  .  ..   extensions length ( 2 bytes, optional )
     *    ..  .  ..   extensions ( optional )
     */

    /* TBD: Needs to be updated due to mandatory extensions
     * Minimal length ( with everything empty and extensions ommitted ) is
     * 2 + 32 + 1 + 2 + 1 = 38 bytes. Check that first, so that we can
     * read at least up to session id length without worrying.
     */
    if( buflen < 38 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    /*
     * We ignore the version field in the ClientHello.
     * We use the version field in the extension.
     */
    /* TODO: Parse this */
    buf += 2;

    /*
     * Save client random
     */
    MBEDTLS_SSL_DEBUG_BUF( 3, "client hello, random bytes", buf, 32 );

    memcpy( &ssl->handshake->randbytes[0], buf, 32 );
    buf += 32; /* skip random bytes */

    /*
     * Parse session ID
     */
    sess_len = buf[0];
    buf++; /* skip session id length */

    if( sess_len > 32 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    ssl->session_negotiate->id_len = sess_len;

    /* Note that this field is echoed even if
     * the client's value corresponded to a cached pre-TLS 1.3 session
     * which the server has chosen not to resume. A client which
     * receives a legacy_session_id_echo field that does not match what
     * it sent in the ClientHello MUST abort the handshake with an
     * "illegal_parameter" alert.
     */
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "client hello, session id length ( %" MBEDTLS_PRINTF_SIZET " )", sess_len ) );
    MBEDTLS_SSL_DEBUG_BUF( 3, "client hello, session id", buf, sess_len );

    memcpy( &ssl->session_negotiate->id[0], buf, sess_len ); /* write session id */
    buf += sess_len;

    ciph_len = MBEDTLS_GET_UINT16_BE( buf, 0 );

    /* Length check */
    if( buf + ciph_len > end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    /* store pointer to ciphersuite list */
    ciph_offset = buf;

    /* skip cipher length */
    buf += 2;

    MBEDTLS_SSL_DEBUG_BUF( 3, "client hello, ciphersuitelist",
                          buf, ciph_len );

    /* skip ciphersuites for now */
    buf += ciph_len;

    /*
     * For TLS 1.3 we are not using compression.
     */
    comp_len = buf[0];

    if( buf + comp_len > end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    buf++; /* skip compression length */
    MBEDTLS_SSL_DEBUG_BUF( 3, "client hello, compression",
                          buf, comp_len );

    /* Determine whether we are indeed using null compression */
    if( ( comp_len != 1 ) && ( buf[1] == 0 ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    /* skip compression */
    buf++;

    /*
     * Check the extension length
     */
    if( buf+2 > end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    ext_len = MBEDTLS_GET_UINT16_BE( buf, 0 );

    if( ( ext_len > 0 && ext_len < 4 ) ||
        buf + 2 + ext_len > end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    buf += 2;

    ext = buf;
    MBEDTLS_SSL_DEBUG_BUF( 3, "client hello extensions", ext, ext_len );

    while( ext_len != 0 )
    {
        unsigned int ext_id, ext_size;

        if( ext_len < 4 )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
            return( MBEDTLS_ERR_SSL_DECODE_ERROR );
        }

        /* The PSK extension must be the last in the ClientHello.
         * Fail if we've found it already but haven't yet reached
         * the end of the extension block. */
        if( ext_psk_ptr != NULL )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
            return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
        }

        ext_id   = MBEDTLS_GET_UINT16_BE( ext, 0 );
        ext_size = MBEDTLS_GET_UINT16_BE( ext, 2 );

        if( ext_size + 4 > ext_len )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad client hello message" ) );
            return( MBEDTLS_ERR_SSL_DECODE_ERROR );
        }

        switch( ext_id )
        {
#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
            case MBEDTLS_TLS_EXT_SERVERNAME:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found ServerName extension" ) );
                ret = ssl_tls13_parse_servername_ext( ssl, ext + 4, ext_size );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_parse_servername_ext", ret );
                    return( ret );
                }
                ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_SERVERNAME;
                break;
#endif /* MBEDTLS_SSL_SERVER_NAME_INDICATION */

#if defined(MBEDTLS_SSL_COOKIE_C)
            case MBEDTLS_TLS_EXT_COOKIE:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found cookie extension" ) );

                ret = ssl_tls13_parse_cookie_ext( ssl, ext + 4, ext_size );

                /* if cookie verification failed then we return a hello retry message */
                if( ret == MBEDTLS_ERR_SSL_HRR_REQUIRED )
                {
                    hrr_required = 1;
                }
                else if( ret == 0 ) /* cookie extension present and processed succesfully */
                {
                    ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_COOKIE;
                }
                break;
#endif /* MBEDTLS_SSL_COOKIE_C  */

#if defined(MBEDTLS_KEY_EXCHANGE_PSK_ENABLED)
            case MBEDTLS_TLS_EXT_PRE_SHARED_KEY:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found pre_shared_key extension" ) );
                /* Delay processing of the PSK identity once we have
                 * found out which algorithms to use. We keep a pointer
                 * to the buffer and the size for later processing.
                 */
                ext_len_psk_ext = ext_size;
                ext_psk_ptr = ext + 4;

                ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_PRE_SHARED_KEY;
                break;
#endif /* MBEDTLS_KEY_EXCHANGE_PSK_ENABLED */

#if defined(MBEDTLS_ZERO_RTT)
            case MBEDTLS_TLS_EXT_EARLY_DATA:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found early_data extension" ) );

                ret = ssl_tls13_parse_early_data_ext( ssl, ext + 4, ext_size );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_parse_early_data_ext", ret );
                    return( ret );
                }

                ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_EARLY_DATA;
                break;
#endif /* MBEDTLS_ZERO_RTT */

#if defined(MBEDTLS_ECDH_C) || defined(MBEDTLS_ECDSA_C)
            case MBEDTLS_TLS_EXT_SUPPORTED_GROUPS:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found supported group extension" ) );

                /* Supported Groups Extension
                 *
                 * When sent by the client, the "supported_groups" extension
                 * indicates the named groups which the client supports,
                 * ordered from most preferred to least preferred.
                 */
                ret = mbedtls_ssl_tls13_parse_supported_groups_ext( ssl, ext + 4,
                        ext_size );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_parse_supported_groups_ext", ret );
                    return( ret );
                }

                ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_SUPPORTED_GROUPS;
                break;
#endif /* MBEDTLS_ECDH_C || MBEDTLS_ECDSA_C */

#if defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)
            case MBEDTLS_TLS_EXT_PSK_KEY_EXCHANGE_MODES:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found psk key exchange modes extension" ) );

                ret = ssl_tls13_parse_key_exchange_modes_ext( ssl, ext + 4, ext_size );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_parse_key_exchange_modes_ext", ret );
                    return( ret );
                }

                ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_PSK_KEY_EXCHANGE_MODES;
                break;
#endif /* MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED */

#if ( defined(MBEDTLS_ECDH_C) || defined(MBEDTLS_ECDSA_C) )
            case MBEDTLS_TLS_EXT_KEY_SHARE:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found key share extension" ) );

                /*
                 * Key Share Extension
                 *
                 * When sent by the client, the "key_share" extension
                 * contains the endpoint's cryptographic parameters for
                 * ECDHE/DHE key establishment methods.
                 */
                ret = ssl_tls13_parse_key_shares_ext( ssl, ext + 4, ext_size );
                if( ret == MBEDTLS_ERR_SSL_HRR_REQUIRED )
                {
                    hrr_required = 1;
                    ret = 0;
                }

                if( ret != 0 )
                    return( ret );

                ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_KEY_SHARE;
                break;
#endif /* MBEDTLS_ECDH_C || MBEDTLS_ECDSA_C */

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
            case MBEDTLS_TLS_EXT_MAX_FRAGMENT_LENGTH:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found max fragment length extension" ) );

                ret = ssl_tls13_parse_max_fragment_length_ext( ssl, ext + 4, ext_size );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, ( "ssl_tls13_parse_max_fragment_length_ext" ), ret );
                    return( ret );
                }
                ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_MAX_FRAGMENT_LENGTH;
                break;
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */

            case MBEDTLS_TLS_EXT_SUPPORTED_VERSIONS:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found supported versions extension" ) );

                ret = ssl_tls13_parse_supported_versions_ext( ssl, ext + 4, ext_size );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, ( "ssl_tls13_parse_supported_versions_ext" ), ret );
                    return( ret );
                }
                ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_SUPPORTED_VERSIONS;
                break;

#if defined(MBEDTLS_SSL_ALPN)
            case MBEDTLS_TLS_EXT_ALPN:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found alpn extension" ) );

                ret = ssl_tls13_parse_alpn_ext( ssl, ext + 4, ext_size );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1, ( "ssl_tls13_parse_alpn_ext" ), ret );
                    return( ret );
                }
                ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_ALPN;
                break;
#endif /* MBEDTLS_SSL_ALPN */

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
            case MBEDTLS_TLS_EXT_SIG_ALG:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found signature_algorithms extension" ) );

                ret = mbedtls_ssl_tls13_parse_sig_alg_ext( ssl, ext + 4, ext + 4 + ext_size );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_MSG( 1, ( "mbedtls_ssl_tls13_parse_sig_alg_ext ( %d )", ret ) );
                    return( ret );
                }
                ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_SIG_ALG;
                break;
#endif /* MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED */

            default:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "unknown extension found: %d ( ignoring )", ext_id ) );
        }

        ext_len -= 4 + ext_size;
        ext     += 4 + ext_size;
    }

    /* Update checksum with either
     * - The entire content of the CH message, if no PSK extension is present
     * - The content up to but excluding the PSK extension, if present.
     */
    {
        unsigned char *ch_without_psk;
        if( ext_psk_ptr == NULL )
            ch_without_psk = ext;
        else
            ch_without_psk = ext_psk_ptr;

        ssl->handshake->update_checksum( ssl,
                                         orig_buf,
                                         ch_without_psk - orig_buf );
    }

    /*
     * Search for a matching ciphersuite
     */
    ciphersuites = ssl->conf->ciphersuite_list;
    ciphersuite_info = NULL;
#if defined(MBEDTLS_SSL_SRV_RESPECT_CLIENT_PREFERENCE)
    for ( j = 0, p = ciph_offset + 2; j < ciph_len; j += 2, p += 2 )
    {
        for ( i = 0; ciphersuites[i] != 0; i++ )
#else
    for ( i = 0; ciphersuites[i] != 0; i++ )
    {
        for ( j = 0, p = ciph_offset + 2; j < ciph_len; j += 2, p += 2 )
#endif /* MBEDTLS_SSL_SRV_RESPECT_CLIENT_PREFERENCE */
        {
            if( MBEDTLS_GET_UINT16_BE( p, 0 ) != ciphersuites[i] )
                continue;

            ciphersuite_info = mbedtls_ssl_ciphersuite_from_id( ciphersuites[i] );

            if( ciphersuite_info == NULL )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "mbedtls_ssl_ciphersuite_from_id: should never happen" ) );
                return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
            }

            goto have_ciphersuite;
            /*
              if( ( ret = ssl_tls13_ciphersuite_match( ssl, ciphersuites[i],
              &ciphersuite_info ) ) != 0 )
              return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

              if( ciphersuite_info != NULL )
              goto have_ciphersuite;
            */

        }
    }

    return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );

    have_ciphersuite:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "selected ciphersuite: %s",
                                ciphersuite_info->name ) );

    ssl->session_negotiate->ciphersuite = ciphersuites[i];
    ssl->handshake->ciphersuite_info = ciphersuite_info;

    /* List all the extensions we have received */
    ssl_tls13_debug_print_client_hello_exts( ssl );

    /*
     * Determine the key exchange algorithm to use.
     * There are three types of key exchanges supported in TLS 1.3:
     * - (EC)DH with ECDSA,
     * - (EC)DH with PSK,
     * - plain PSK.
     *
     * The PSK-based key exchanges may additionally be used with 0-RTT.
     *
     * Our built-in order of preference is
     *  1 ) Plain PSK Mode
     *  2 ) (EC)DHE-PSK Mode
     *  3 ) Certificate Mode
     */

    ssl->handshake->key_exchange = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_NONE;

    if( !ssl_tls13_check_psk_key_exchange( ssl ) &&
        !ssl_tls13_check_certificate_key_exchange( ssl ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "ClientHello message misses mandatory extensions." ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_MISSING_EXTENSION ,
                                      MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

#if defined(MBEDTLS_ZERO_RTT)
    ret = ssl_tls13_check_use_0rtt_handshake( ssl );
    if( ret != 0 )
        return( ret );
#endif /* MBEDTLS_ZERO_RTT */

    /* If we've settled on a PSK-based exchange, parse PSK identity ext */
    if( mbedtls_ssl_tls13_kex_with_psk( ssl ) )
    {
        ret = mbedtls_ssl_tls13_parse_client_psk_identity_ext( ssl,
                                                               ext_psk_ptr,
                                                               ext_len_psk_ext );
        if( ret != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, ( "mbedtls_ssl_tls13_parse_client_psk_identity" ),
                                   ret );
            return( ret );
        }
    }

#if defined(MBEDTLS_SSL_COOKIE_C)
    /* If we failed to see a cookie extension, and we required it through the
     * configuration settings ( rr_config ), then we need to send a HRR msg.
     * Conceptually, this is similiar to having received a cookie that failed
     * the verification check.
     */
    if( ( ssl->conf->rr_config == MBEDTLS_SSL_FORCE_RR_CHECK_ON ) &&
        !( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_COOKIE ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "Cookie extension missing. Need to send a HRR." ) );
        hrr_required = 1;
    }
#endif /* MBEDTLS_SSL_COOKIE_C */

    if( hrr_required == 1 )
        return( SSL_CLIENT_HELLO_HRR_REQUIRED );

    return( 0 );
}

static int ssl_tls13_client_hello_postprocess( mbedtls_ssl_context *ssl,
                                               int hrr_required )
{
    int ret = 0;
#if defined(MBEDTLS_ZERO_RTT)
    mbedtls_ssl_key_set traffic_keys;
#endif /* MBEDTLS_ZERO_RTT */

    if( ssl->handshake->hello_retry_requests_sent == 0 &&
        ssl->conf->rr_config == MBEDTLS_SSL_FORCE_RR_CHECK_ON )
    {
        hrr_required = SSL_CLIENT_HELLO_HRR_REQUIRED;
    }

    if( hrr_required == SSL_CLIENT_HELLO_HRR_REQUIRED )
    {
        /*
         * Create stateless transcript hash for HRR
         */
        MBEDTLS_SSL_DEBUG_MSG( 4, ( "Reset transcript for HRR" ) );
        ret = mbedtls_ssl_reset_transcript_for_hrr( ssl );
        if( ret != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_reset_transcript_for_hrr", ret );
            return( ret );
        }
        mbedtls_ssl_session_reset_msg_layer( ssl, 0 );

        /* Transmit Hello Retry Request */
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_HELLO_RETRY_REQUEST );
        return( 0 );
    }

    ret = mbedtls_ssl_tls13_key_schedule_stage_early( ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1,
             "mbedtls_ssl_tls13_key_schedule_stage_early", ret );
        return( ret );
    }

#if defined(MBEDTLS_ZERO_RTT)
    if( ssl->handshake->early_data == MBEDTLS_SSL_EARLY_DATA_ON )
    {
        mbedtls_ssl_transform *transform_earlydata;

        MBEDTLS_SSL_DEBUG_MSG( 3, ( "Generate 0-RTT keys" ) );

        ret = mbedtls_ssl_tls13_generate_early_data_keys(
            ssl, &traffic_keys );
        if( ret != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1,
                      "mbedtls_ssl_tls13_generate_early_data_keys", ret );
            return( ret );
        }

        transform_earlydata = mbedtls_calloc( 1, sizeof( mbedtls_ssl_transform ) );
        if( transform_earlydata == NULL )
            return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

        ret = mbedtls_ssl_tls13_populate_transform(
            transform_earlydata, ssl->conf->endpoint,
            ssl->session_negotiate->ciphersuite, &traffic_keys, ssl );
        if( ret != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_populate_transform", ret );
            return( ret );
        }

#if !defined(MBEDTLS_SSL_USE_MPS)
        ssl->handshake->transform_earlydata = transform_earlydata;
#else /* MBEDTLS_SSL_USE_MPS */
        /* Register transform with MPS. */
        ret = mbedtls_mps_add_key_material( &ssl->mps->l4,
                                            transform_earlydata,
                                            &ssl->handshake->epoch_earlydata );
        if( ret != 0 )
            return( ret );
#endif /* MBEDTLS_SSL_USE_MPS */
    }

    mbedtls_platform_zeroize( &traffic_keys, sizeof( traffic_keys ) );

#endif /* MBEDTLS_ZERO_RTT */

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_HELLO );
    return( 0 );

}


#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
static int ssl_tls13_write_max_fragment_length_ext( mbedtls_ssl_context *ssl,
                                                    unsigned char *buf,
                                                    size_t buflen,
                                                    size_t *olen )
{
    unsigned char *p = buf;

    *olen = 0;

    if( ( ssl->handshake->extensions_present &
          MBEDTLS_SSL_EXT_MAX_FRAGMENT_LENGTH ) == 0 )
    {
        return( 0 );
    }

    if( ssl->session_negotiate->mfl_code == MBEDTLS_SSL_MAX_FRAG_LEN_NONE )
    {
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3,
        ( "adding max_fragment_length extension" ) );

    MBEDTLS_SSL_CHK_BUF_PTR( p, buf + buflen, 5 );

    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_MAX_FRAGMENT_LENGTH, p, 0 );
    MBEDTLS_PUT_UINT16_BE( 1, p, 2 );
    p[4] = ssl->session_negotiate->mfl_code;

    *olen = 5;

    return( 0 );
}
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */



#if defined(MBEDTLS_SSL_ALPN)
static int ssl_tls13_write_alpn_ext(
    mbedtls_ssl_context *ssl,
    unsigned char *buf, size_t buflen, size_t *olen )
{
    *olen = 0;

    if( ( ssl->handshake->extensions_present & MBEDTLS_SSL_EXT_ALPN ) == 0 ||
        ssl->alpn_chosen == NULL )
    {
        return( 0 );
    }

    if( buflen < 7 + strlen( ssl->alpn_chosen ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "buffer too small" ) );
        return ( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "server hello, adding alpn extension" ) );
    /*
     * 0 . 1    ext identifier
     * 2 . 3    ext length
     * 4 . 5    protocol list length
     * 6 . 6    protocol name length
     * 7 . 7+n  protocol name
     */
    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_ALPN, buf, 0 );

    *olen = 7 + strlen( ssl->alpn_chosen );

    MBEDTLS_PUT_UINT16_BE( *olen - 4, buf, 2 );
    MBEDTLS_PUT_UINT16_BE( *olen - 6, buf, 4 );
    buf[6] = MBEDTLS_BYTE_0( *olen - 7 );

    memcpy( buf + 7, ssl->alpn_chosen, *olen - 7 );
    return ( 0 );
}
#endif /* MBEDTLS_SSL_ALPN */



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
static int ssl_tls13_encrypted_extensions_process( mbedtls_ssl_context *ssl );

static int ssl_tls13_encrypted_extensions_prepare( mbedtls_ssl_context *ssl );
static int ssl_tls13_encrypted_extensions_write( mbedtls_ssl_context *ssl,
                                                 unsigned char *buf,
                                                 size_t buflen,
                                                 size_t *olen );
static int ssl_tls13_encrypted_extensions_postprocess( mbedtls_ssl_context *ssl );

static int ssl_tls13_encrypted_extensions_process( mbedtls_ssl_context *ssl )
{
    int ret;
    unsigned char *buf;
    size_t buf_len, msg_len;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write encrypted extension" ) );

    if( ssl->handshake->state_local.encrypted_extensions_out.preparation_done == 0 )
    {
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_encrypted_extensions_prepare( ssl ) );
        ssl->handshake->state_local.encrypted_extensions_out.preparation_done = 1;
    }

    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_start_handshake_msg( ssl,
                       MBEDTLS_SSL_HS_ENCRYPTED_EXTENSIONS, &buf, &buf_len ) );

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_encrypted_extensions_write(
                              ssl, buf, buf_len, &msg_len ) );

    mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_ENCRYPTED_EXTENSIONS,
                                        buf, msg_len );

    /* Update state */
    MBEDTLS_SSL_PROC_CHK( ssl_tls13_encrypted_extensions_postprocess( ssl ) );

    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_finish_handshake_msg(
                              ssl, buf_len, msg_len ) );

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write encrypted extension" ) );
    return( ret );
}

static int ssl_tls13_encrypted_extensions_prepare( mbedtls_ssl_context *ssl )
{
    int ret;
    mbedtls_ssl_key_set traffic_keys;
    mbedtls_ssl_transform *transform_handshake;

    /* Compute handshake secret */
    ret = mbedtls_ssl_tls13_key_schedule_stage_handshake( ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_key_schedule_stage_handshake", ret );
        return( ret );
    }

    /* Derive handshake key material */
    ret = mbedtls_ssl_tls13_generate_handshake_keys( ssl, &traffic_keys );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1,
                "mbedtls_ssl_tls13_generate_handshake_keys", ret );
        return( ret );
    }

    transform_handshake = mbedtls_calloc( 1, sizeof( mbedtls_ssl_transform ) );
    if( transform_handshake == NULL )
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );

    /* Setup transform from handshake key material */
    ret = mbedtls_ssl_tls13_populate_transform(
                               transform_handshake,
                               ssl->conf->endpoint,
                               ssl->session_negotiate->ciphersuite,
                               &traffic_keys,
                               ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_populate_transform", ret );
        return( ret );
    }

#if !defined(MBEDTLS_SSL_USE_MPS)
    ssl->handshake->transform_handshake = transform_handshake;
    mbedtls_ssl_set_outbound_transform( ssl, ssl->handshake->transform_handshake );
#else /* MBEDTLS_SSL_USE_MPS */
    /* Register transform with MPS. */
    ret = mbedtls_mps_add_key_material( &ssl->mps->l4,
                                        transform_handshake,
                                        &ssl->handshake->epoch_handshake );
    if( ret != 0 )
        return( ret );

    /* Use new transform for outgoing data. */
    ret = mbedtls_mps_set_outgoing_keys( &ssl->mps->l4,
                                         ssl->handshake->epoch_handshake );
    if( ret != 0 )
        return( ret );
#endif /* MBEDTLS_SSL_USE_MPS */

    /*
     * Switch to our negotiated transform and session parameters for outbound
     * data.
     */
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "switching to new transform spec for outbound data" ) );

#if !defined(MBEDTLS_SSL_USE_MPS)
    memset( ssl->out_ctr, 0, 8 );
#endif /* MBEDTLS_SSL_USE_MPS */

    return( 0 );
}

static int ssl_tls13_encrypted_extensions_write( mbedtls_ssl_context *ssl,
                                                 unsigned char *buf,
                                                 size_t buflen,
                                                 size_t *olen )
{
    int ret;
    size_t n, enc_ext_len;
    unsigned char *p, *end, *len;

    /* If all extensions are disabled then olen is 0. */
    *olen = 0;

    end = buf + buflen;
    p = buf;

    /*
     * struct {
     *    Extension extensions<0..2 ^ 16 - 1>;
     * } EncryptedExtensions;
     *
     */

    /* Skip extension length; first write extensions, then update length */
    len = p;
    p += 2;

#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
    ret = ssl_tls13_write_sni_server_ext( ssl, p, end - p, &n );
    if( ret != 0 )
        return( ret );
    p += n;
#endif /* MBEDTLS_SSL_SERVER_NAME_INDICATION */

#if defined(MBEDTLS_SSL_ALPN)
    ret = ssl_tls13_write_alpn_ext( ssl, p, end - p, &n );
    if( ret != 0 )
        return( ret );
    p  += n;
#endif /* MBEDTLS_SSL_ALPN */

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
    ret = ssl_tls13_write_max_fragment_length_ext( ssl, p, end - p, &n );
    if( ret != 0 )
        return( ret );
    p += n;
#endif /* MBEDTLS_SSL_MAX_FRAGMENT_LENGTH */

#if defined(MBEDTLS_ZERO_RTT)
    ret = mbedtls_ssl_tls13_write_early_data_ext( ssl, p, end, &n );
    if( ret != 0 )
        return( ret );
    p += n;
#endif /* MBEDTLS_ZERO_RTT */

    *olen = p - buf;
    enc_ext_len = (size_t)( ( p - len ) - 2 );

    MBEDTLS_PUT_UINT16_BE( enc_ext_len, len, 0 );

    return( 0 );
}

static int ssl_tls13_encrypted_extensions_postprocess( mbedtls_ssl_context *ssl )
{
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CERTIFICATE_REQUEST );
    return( 0 );
}

/*
 *
 * HelloRetryRequest message
 *
 * Servers send this message in response to a ClientHello message when
 * the server was able to find an acceptable set of algorithms and groups
 * that are mutually supported, but the client's KeyShare did not contain
 * an acceptable offer.
 *
 * We also send this message with DTLS 1.3 to perform a return-routability
 * check (and we include a cookie).
 */

/*
 * Overview
 */

/* Main entry point; orchestrates the other functions */
static int ssl_tls13_write_hello_retry_request_process( mbedtls_ssl_context *ssl );

static int ssl_tls13_write_hello_retry_request_coordinate( mbedtls_ssl_context *ssl );
static int ssl_tls13_write_hello_retry_request_write( mbedtls_ssl_context *ssl,
                                                      unsigned char *buf,
                                                      size_t buflen,
                                                      size_t *olen );
static int ssl_tls13_write_hello_retry_request_postprocess( mbedtls_ssl_context *ssl );

static int ssl_tls13_write_hello_retry_request_process( mbedtls_ssl_context *ssl )
{
    int ret;
    unsigned char *buf;
    size_t buf_len, msg_len;

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_hello_retry_request_coordinate( ssl ) );

    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_start_handshake_msg( ssl,
                       MBEDTLS_SSL_HS_SERVER_HELLO, &buf, &buf_len ) );

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_hello_retry_request_write(
                              ssl, buf, buf_len, &msg_len ) );

    mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_SERVER_HELLO,
                                        buf, msg_len );

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_write_hello_retry_request_postprocess( ssl ) );

    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_finish_handshake_msg(
                              ssl, buf_len, msg_len ) );
cleanup:

    return( ret );
}


static int ssl_tls13_write_hello_retry_request_coordinate( mbedtls_ssl_context *ssl )
{
    if( ssl->handshake->hello_retry_requests_sent > 1 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Too many HRRs" ) );
        return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
    }

    return( 0 );
}

static int ssl_tls13_reset_ecdhe_share( mbedtls_ssl_context *ssl )
{
    mbedtls_ecdh_free( &ssl->handshake->ecdh_ctx );
    return( 0 );
}

static int ssl_tls13_reset_key_share( mbedtls_ssl_context *ssl )
{
    uint16_t group_id = ssl->handshake->offered_group_id;

    if( mbedtls_ssl_tls13_named_group_is_ecdhe( group_id ) )
        return( ssl_tls13_reset_ecdhe_share( ssl ) );
    else if( 0 /* other KEMs? */ )
    {
        /* Do something */
    }

    return( 0 );
}

static int ssl_tls13_write_hello_retry_request_postprocess( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    ssl->handshake->hello_retry_requests_sent++;

    /* Reset everything that's going to be re-generated in the new ClientHello.
     *
     * Currently, we're always resetting the key share, even if the server
     * was fine with it. Once we have separated key share generation from
     * key share writing, we can confine this to the case where the server
     * requested a different share. */
    ret = ssl_tls13_reset_key_share( ssl );
    if( ret != 0 )
        return( ret );

#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_CCS_AFTER_HRR );
#else
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_HELLO );
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

    return( 0 );
}

static int ssl_tls13_write_hrr_key_share_ext( mbedtls_ssl_context *ssl,
                                              unsigned char *buf,
                                              unsigned char *end,
                                              size_t *olen )
{
    const uint16_t *group_list;
    const mbedtls_ecp_curve_info **curve = NULL;

    size_t total_len = 0;

    /* key_share Extension
     *
     *  struct {
     *    select (Handshake.msg_type) {
     *      ...
     *      case hello_retry_request:
     *          NamedGroup selected_group;
     *      ...
     *    };
     * } KeyShare;
     */

    *olen = 0;

    /* For a pure PSK-based ciphersuite there is no key share to declare. */
    if( !mbedtls_ssl_tls13_kex_with_ephemeral( ssl ) )
        return( 0 );

    /* We should only send the key_share extension if the client's initial
     * key share was not acceptable. */
    if( ssl->handshake->offered_group_id != 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 4, ( "Skip key_share extension in HRR" ) );
        return( 0 );
    }

    total_len = 6; /* extension header, extension length, NamedGroup value */

    if( (size_t)( end - buf ) < total_len )
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );

    /* Write extension header */
    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_KEY_SHARE, buf, 0 );
    /* Write extension length */
    MBEDTLS_PUT_UINT16_BE( 2, buf, 2 );
    buf += 4;

    /* Find common curve */
    for( group_list = mbedtls_ssl_get_groups( ssl );
         *group_list != 0; group_list++ )
    {
        for( curve = ssl->handshake->curves; *curve != NULL; curve++ )
        {
            if( (*curve)->tls_id == *group_list )
                goto curve_matching_done;
        }
    }

curve_matching_done:
    if( curve == NULL || *curve == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "no matching named group found" ) );
        return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
    }

    /* Write selected group */
    MBEDTLS_PUT_UINT16_BE( (*curve)->tls_id, buf, 0 );

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "NamedGroup in HRR: %s", (*curve)->name ) );
    *olen = total_len;
    return( 0 );
}

static int ssl_tls13_write_hello_retry_request_write( mbedtls_ssl_context *ssl,
                                                      unsigned char *buf,
                                                      size_t buflen,
                                                      size_t *olen )
{
    int ret;
    unsigned char *p = buf;
    unsigned char *end = buf + buflen;
    unsigned char *ext_len_byte;
    size_t ext_length;
    size_t total_ext_len = 0;
    unsigned char *extension_start;
    const char magic_hrr_string[32] =
               { 0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE,
                 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2,
                 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09,
                 0xE2, 0xC8, 0xA8, 0x33 ,0x9C };

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write hello retry request" ) );

    /*
     * struct {
     *    ProtocolVersion legacy_version = 0x0303;
     *    Random random ( with magic value );
     *    opaque legacy_session_id_echo<0..32>;
     *    CipherSuite cipher_suite;
     *    uint8 legacy_compression_method = 0;
     *    Extension extensions<0..2^16-1>;
     * } ServerHello; --- aka HelloRetryRequest
     */


    /* For TLS 1.3 we use the legacy version number {0x03, 0x03}
     *  instead of the true version number.
     *
     *  For DTLS 1.3 we use the legacy version number
     *  {254,253}.
     *
     *  In cTLS the version number is elided.
     */
    *p++ = 0x03;
    *p++ = 0x03;
    MBEDTLS_SSL_DEBUG_BUF( 3, "server version", p - 2, 2 );

    /* write magic string (as a replacement for the random value) */
    memcpy( p, &magic_hrr_string[0], 32 );
    MBEDTLS_SSL_DEBUG_BUF( 3, "Random bytes in HelloRetryRequest", p, 32 );
    p += 32;

    /* write legacy_session_id_echo */
    *p++ = (unsigned char) ssl->session_negotiate->id_len;
    memcpy( p, &ssl->session_negotiate->id[0], ssl->session_negotiate->id_len );
    MBEDTLS_SSL_DEBUG_BUF( 3, "session id", p, ssl->session_negotiate->id_len );
    p += ssl->session_negotiate->id_len;

    /* write ciphersuite (2 bytes) */
    MBEDTLS_PUT_UINT16_BE( ssl->session_negotiate->ciphersuite, p, 0 );
    MBEDTLS_SSL_DEBUG_BUF( 3, "ciphersuite", p, 2 );
    p += 2;

    /* write legacy_compression_method (0) */
    *p++ = 0x0;
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "legacy compression method: [%d]", *( p-1 ) ) );

    /* write extensions */
    extension_start = p;
    /* Extension starts with a 2 byte length field; we skip it and write it later */
    p += 2;

#if defined(MBEDTLS_SSL_COOKIE_C)

    /* Cookie Extension
     *
     * struct {
     *    opaque cookie<0..2^16-1>;
     * } Cookie;
     *
     */

    /* Write extension header */
    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_COOKIE, p, 0 );
    p += 2;

    /* Skip writing the extension and the cookie length */
    ext_len_byte = p;
    p += 4;

    /* If we get here, f_cookie_check is not null */
    if( ssl->conf->f_cookie_write == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "inconsistent cookie callbacks" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

    if( ( ret = ssl->conf->f_cookie_write( ssl->conf->p_cookie,
                                           &p, end,
                                           ssl->cli_id,
                                           ssl->cli_id_len ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "f_cookie_write", ret );
        return( ret );
    }

    ext_length = ( p - ( ext_len_byte + 4 ) );

    MBEDTLS_SSL_DEBUG_BUF( 3, "Cookie", ext_len_byte + 4, ext_length );

    /* Write extension length */
    MBEDTLS_PUT_UINT16_BE( ext_length + 2, ext_len_byte, 0 );

    /* Write cookie length */
    MBEDTLS_PUT_UINT16_BE( ext_length, ext_len_byte, 2 );

    /* 2 bytes for extension type,
     * 2 bytes for extension length field,
     * 2 bytes for cookie length */
    total_ext_len += ext_length + 6;
#endif /* MBEDTLS_SSL_COOKIE_C */

    /* Add supported_version extension */
    if( ( ret = ssl_tls13_write_supported_version_ext( ssl, p, end,
                                                       &ext_length ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_write_supported_version_ext", ret );
        return( ret );
    }

    total_ext_len += ext_length;
    p += ext_length;

    /* Add key_share extension, if necessary */
    ret = ssl_tls13_write_hrr_key_share_ext( ssl, p, end, &ext_length );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_write_hrr_key_share_ext", ret );
        return( ret );
    }
    total_ext_len += ext_length;
    p += ext_length;

    MBEDTLS_PUT_UINT16_BE( total_ext_len, extension_start, 0 );

    *olen = p - buf;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write hello retry request" ) );
    return( 0 );
}

/*
 *
 * STATE HANDLING: ServerHello
 *
 */

/*
 * Overview
 */

/* Main entry point; orchestrates the other functions */
static int ssl_tls13_server_hello_process( mbedtls_ssl_context *ssl );

/* ServerHello handling sub-routines */
static int ssl_tls13_server_hello_prepare( mbedtls_ssl_context *ssl );
static int ssl_tls13_server_hello_write( mbedtls_ssl_context *ssl,
                                         unsigned char *buf,
                                         size_t buflen,
                                         size_t *olen );
static int ssl_tls13_server_hello_postprocess( mbedtls_ssl_context *ssl );

static int ssl_tls13_server_hello_process( mbedtls_ssl_context *ssl ) {

    int ret = 0;
    unsigned char *buf;
    size_t buf_len, msg_len;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write server hello" ) );

    /* Preprocessing */

    /* This might lead to ssl_tls13_process_server_hello() being called
     * multiple times. The implementation of
     * ssl_tls13_process_server_hello_preprocess() must either be safe to be
     * called multiple times, or we need to add state to omit this call once
     * we're calling ssl_tls13_process_server_hello() multiple times.
     */
    MBEDTLS_SSL_PROC_CHK( ssl_tls13_server_hello_prepare( ssl ) );

        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_start_handshake_msg( ssl,
                MBEDTLS_SSL_HS_SERVER_HELLO, &buf, &buf_len ) );

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_server_hello_write( ssl, buf, buf_len,
                                                        &msg_len ) );

    mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_SERVER_HELLO,
                                        buf, msg_len );

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_server_hello_postprocess( ssl ) );

    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_finish_handshake_msg(
                              ssl, buf_len, msg_len ) );
cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write server hello" ) );
    return( ret );
}


/* IMPORTANT: This function can currently be called multiple times
 *            in case the call to mbedtls_ssl_flush_output( ) that
 *            follows it in ssl_tls13_process_server_hello( ) fails.
 *
 *            Make sure that the preparations in this function
 *            can safely be repeated multiple times, or add logic
 *            to ssl_tls13_process_server_hello( ) to never call it twice.
 */
static int ssl_tls13_server_hello_prepare( mbedtls_ssl_context *ssl )
{
    int ret;

    if( ( ret = ssl->conf->f_rng( ssl->conf->p_rng, ssl->handshake->randbytes + 32, 32 ) ) != 0 )
        return( ret );

    MBEDTLS_SSL_DEBUG_BUF( 3, "server hello, random bytes", ssl->handshake->randbytes + 32, 32 );

#if defined(MBEDTLS_HAVE_TIME)
    ssl->session_negotiate->start = time( NULL );
#endif /* MBEDTLS_HAVE_TIME */


    /* Check for session resumption
     * <TBD>
     */

    return( 0 );
}

static int ssl_tls13_server_hello_write( mbedtls_ssl_context *ssl,
                                         unsigned char *buf,
                                         size_t buflen,
                                         size_t *olen )
{
    int ret=0;
    /* Extensions */

    /* extension_start
     *    Used during extension writing where the
     *    buffer pointer to the beginning of the
     *    extension list must be kept to write
     *    the total extension list size in the end.
     */
    unsigned char *extension_start;
    size_t cur_ext_len;          /* Size of the current extension */
    size_t total_ext_len;        /* Size of list of extensions    */
    size_t rand_bytes_len;

    /* Buffer management */
    unsigned char *start = buf;
    unsigned char *end = buf + buflen;

    rand_bytes_len = 32;

    /* Ensure we have enough room for ServerHello
     * up to but excluding the extensions. */
    if( buflen < ( 4+32+2+2+1+ssl->session_negotiate->id_len+1+1 ) ) /* TBD: FIXME */
    {
        return( MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL );
    }

    /* Version */
    *buf++ = (unsigned char)0x3;
    *buf++ = (unsigned char)0x3;
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "server hello, chosen version: [0x3:0x3]" ) );
    buflen -= 2;

    /* Write random bytes */
    memcpy( buf, ssl->handshake->randbytes + 32, rand_bytes_len );
    MBEDTLS_SSL_DEBUG_BUF( 3, "server hello, random bytes", buf, rand_bytes_len );

    buf += rand_bytes_len;
    buflen -= rand_bytes_len;

#if defined(MBEDTLS_HAVE_TIME)
    ssl->session_negotiate->start = time( NULL );
#endif /* MBEDTLS_HAVE_TIME */

    /* Write legacy session id */
    *buf++ = (unsigned char)ssl->session_negotiate->id_len;
    buflen--;
    memcpy( buf, &ssl->session_negotiate->id[0], ssl->session_negotiate->id_len );
    buf += ssl->session_negotiate->id_len;
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "session id length ( %" MBEDTLS_PRINTF_SIZET " )", ssl->session_negotiate->id_len ) );
    MBEDTLS_SSL_DEBUG_BUF( 3, "session id", ssl->session_negotiate->id, ssl->session_negotiate->id_len );
    buflen -= ssl->session_negotiate->id_len;

    /* write selected ciphersuite ( 2 bytes ) */
    MBEDTLS_PUT_UINT16_BE( ssl->session_negotiate->ciphersuite, buf, 0 );
    buf += 2;
    buflen -= 2;
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "server hello, chosen ciphersuite: %s ( id=%d )",
        mbedtls_ssl_get_ciphersuite_name( ssl->session_negotiate->ciphersuite ),
        ssl->session_negotiate->ciphersuite ) );

    /* write legacy_compression_method ( 0 ) */
    *buf++ = 0x0;
    buflen--;

    /* First write extensions, then the total length */
    extension_start = buf;
    total_ext_len = 0;
    buf += 2;

#if defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)
    if( mbedtls_ssl_tls13_kex_with_psk( ssl ) )
    {
        ret = ssl_tls13_write_server_pre_shared_key_ext( ssl, buf, end,
                                                         &cur_ext_len );
        if( ret != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_write_server_pre_shared_key_ext",
                                   ret );
            return( ret );
        }

        total_ext_len += cur_ext_len;
        buf += cur_ext_len;
    }
#endif /* MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED */

#if ( defined(MBEDTLS_ECDH_C) || defined(MBEDTLS_ECDSA_C) )
    if( mbedtls_ssl_tls13_kex_with_ephemeral( ssl ) )
    {
        if( ( ret = ssl_tls13_write_key_shares_ext( ssl, buf, end, &cur_ext_len ) ) != 0 )
        {
            MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_write_key_shares_ext", ret );
            return( ret );
        }

        total_ext_len += cur_ext_len;
        buf += cur_ext_len;
    }
#endif /* ( MBEDTLS_ECDH_C || MBEDTLS_ECDSA_C */

    /* Add supported_version extension */
    if( ( ret = ssl_tls13_write_supported_version_ext( ssl, buf, end, &cur_ext_len ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_write_supported_version_ext", ret );
        return( ret );
    }

    total_ext_len += cur_ext_len;
    buf += cur_ext_len;

    MBEDTLS_SSL_DEBUG_BUF( 4, "server hello extensions", extension_start, total_ext_len );

    /* Write length information */
    MBEDTLS_PUT_UINT16_BE( total_ext_len, extension_start, 0 );
    buflen -= 2 + total_ext_len;

    *olen = buf - start;

    MBEDTLS_SSL_DEBUG_BUF( 3, "server hello", start, *olen );

    return( ret );
}

static int ssl_tls13_server_hello_postprocess( mbedtls_ssl_context *ssl )
{
#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
    if( ssl->handshake->ccs_sent > 1 )
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_CCS_AFTER_SERVER_HELLO );
    else
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */
    {
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_ENCRYPTED_EXTENSIONS );
    }
    return( 0 );
}

/*
 *
 * STATE HANDLING: CertificateRequest
 *
 */

/* Main entry point; orchestrates the other functions */
static int ssl_tls13_certificate_request_process( mbedtls_ssl_context *ssl );

/* Coordination:
 * Check whether a CertificateRequest message should be written.
 * Returns a negative error code on failure, or one of
 * - SSL_CERTIFICATE_REQUEST_EXPECT_WRITE or
 * - SSL_CERTIFICATE_REQUEST_SKIP
 * indicating if the writing of the CertificateRequest
 * should be skipped or not.
 */
#define SSL_CERTIFICATE_REQUEST_SEND 0
#define SSL_CERTIFICATE_REQUEST_SKIP 1
static int ssl_tls13_certificate_request_coordinate( mbedtls_ssl_context* ssl );
#if defined(MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED)
static int ssl_tls13_certificate_request_write( mbedtls_ssl_context *ssl,
                                                unsigned char *buf,
                                                size_t buflen,
                                                size_t *olen );
#endif /* MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED */
static int ssl_tls13_certificate_request_postprocess( mbedtls_ssl_context *ssl );


/*
 * Implementation
 */

static int ssl_tls13_certificate_request_process( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> write certificate request" ) );

    /* Coordination step: Check if we need to send a CertificateRequest */
    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_certificate_request_coordinate( ssl ) );

#if defined(MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED)
    if( ret == SSL_CERTIFICATE_REQUEST_SEND )
    {
        unsigned char *buf;
        size_t buf_len, msg_len;

        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_start_handshake_msg( ssl,
                MBEDTLS_SSL_HS_CERTIFICATE_REQUEST, &buf, &buf_len ) );

        MBEDTLS_SSL_PROC_CHK( ssl_tls13_certificate_request_write(
                                  ssl, buf, buf_len, &msg_len ) );

        mbedtls_ssl_add_hs_msg_to_checksum(
            ssl, MBEDTLS_SSL_HS_CERTIFICATE_REQUEST, buf, msg_len );

        /* TODO: Logically this should come at the end, but the non-MPS msg
         *       layer impl'n of mbedtls_ssl_finish_handshake_msg() can fail. */
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_certificate_request_postprocess( ssl ) );
        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_finish_handshake_msg(
                                  ssl, buf_len, msg_len ) );

    }
    else
#endif /* MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED */
    if( ret == SSL_CERTIFICATE_REQUEST_SKIP )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= skip write certificate request" ) );

        /* Update state */
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_certificate_request_postprocess( ssl ) );
    }
    else
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= write certificate request" ) );
    return( ret );
}

static int ssl_tls13_certificate_request_coordinate( mbedtls_ssl_context *ssl )
{
    int authmode;

    if( mbedtls_ssl_tls13_kex_with_psk( ssl ) )
        return( SSL_CERTIFICATE_REQUEST_SKIP );

#if !defined(MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED)
    ( ( void )authmode );
    MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
    return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
#else

#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
    if( ssl->handshake->sni_authmode != MBEDTLS_SSL_VERIFY_UNSET )
        authmode = ssl->handshake->sni_authmode;
    else
#endif
        authmode = ssl->conf->authmode;

    if( authmode == MBEDTLS_SSL_VERIFY_NONE )
        return( SSL_CERTIFICATE_REQUEST_SKIP );

    return( SSL_CERTIFICATE_REQUEST_SEND );

#endif /* MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED */
}

static int ssl_tls13_certificate_request_write( mbedtls_ssl_context *ssl,
                                                unsigned char *buf,
                                                size_t buflen,
                                                size_t *olen )
{
    int ret;
    size_t ext_size;
    unsigned char *p;
    unsigned char *end = buf + buflen;

    p = buf;

    if( p + 1 + 2 > end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "buffer too small" ) );
        return ( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    /*
     *
     * struct {
     *   opaque certificate_request_context<0..2^8-1>;
     *   Extension extensions<2..2^16-1>;
     * } CertificateRequest;
     *
     */

    /*
     * Write certificate_request_context
     */

    /*
     * We use a zero length context for the normal handshake
     * messages. For post-authentication handshake messages
     * this request context would be set to a non-zero value.
     */
    *p++ = 0x0;

    /*
     * Write extensions
     */

    /* The extensions must contain the signature_algorithms. */
    /* Currently we don't use any other extension */
    ret = mbedtls_ssl_write_sig_alg_ext( ssl, p + 2, end, &ext_size );
    if( ret != 0 )
        return( ret );

    /* length field for all extensions */
    MBEDTLS_PUT_UINT16_BE( ext_size, p, 0 );
    p += 2 + ext_size;

    *olen = p - buf;

    return( ret );
}


static int ssl_tls13_certificate_request_postprocess( mbedtls_ssl_context *ssl )
{
    /* next state */
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_CERTIFICATE );
    return( 0 );
}

/*
 * TLS and DTLS 1.3 State Maschine -- server side
 */
int mbedtls_ssl_tls13_handshake_server_step( mbedtls_ssl_context *ssl )
{
    int ret = 0;

    if( ssl->state == MBEDTLS_SSL_HANDSHAKE_OVER || ssl->handshake == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "tls13 server state: %s(%d)",
                                mbedtls_ssl_states_str( ssl->state ),
                                ssl->state ) );

    switch( ssl->state )
    {
        /* start state */
        case MBEDTLS_SSL_HELLO_REQUEST:
            ssl->handshake->hello_retry_requests_sent = 0;
            mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_HELLO );

#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
            ssl->handshake->ccs_sent = 0;
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

            break;

            /* ----- READ CLIENT HELLO ----*/

        case MBEDTLS_SSL_CLIENT_HELLO:

#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)
            ssl->session_negotiate->tls_version = ssl->tls_version;
            ssl->session_negotiate->endpoint = ssl->conf->endpoint;
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */

            ret = ssl_tls13_client_hello_process( ssl );
            if( ret != 0 )
                MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_client_hello_process", ret );

            break;

            /* ----- WRITE EARLY APP DATA  ----*/
        case MBEDTLS_SSL_EARLY_APP_DATA:

            ret = ssl_tls13_read_early_data_process( ssl );
            if( ret != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_read_early_data_process", ret );
                return ( ret );
            }

            break;

            /* ----- WRITE HELLO RETRY REQUEST ----*/

        case MBEDTLS_SSL_HELLO_RETRY_REQUEST:

            ret = ssl_tls13_write_hello_retry_request_process( ssl );
            if( ret != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_write_hello_retry_request", ret );
                return( ret );
            }

            break;

            /* ----- WRITE CHANGE CIPHER SPEC ----*/

#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
        case MBEDTLS_SSL_SERVER_CCS_AFTER_HRR:
            ret = mbedtls_ssl_tls13_write_change_cipher_spec( ssl );
            if( ret != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_write_change_cipher_spec", ret );
                return( ret );
            }

            break;
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

            /* ----- READ 2nd CLIENT HELLO ----*/
        case MBEDTLS_SSL_SECOND_CLIENT_HELLO:

            ret = ssl_tls13_client_hello_process( ssl );

            switch( ret )
            {
                case 0:
                    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_HELLO );
                    break;
                case MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION:
                    MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_PROTOCOL_VERSION,
                                                  MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION );
                    break;
                case MBEDTLS_ERR_SSL_CONTINUE_PROCESSING:
                    /* Stay in this state */
                    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SECOND_CLIENT_HELLO );
                    ret = 0;
                    break;
                default:
                    return( ret );
            }

            break;
            /* ----- WRITE SERVER HELLO ----*/

        case MBEDTLS_SSL_SERVER_HELLO:
            ret = ssl_tls13_server_hello_process( ssl );
            if( ret != 0 )
                break;


            break;

            /* ----- WRITE CHANGE CIPHER SPEC ----*/

#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
        case MBEDTLS_SSL_SERVER_CCS_AFTER_SERVER_HELLO:
            ret = mbedtls_ssl_tls13_write_change_cipher_spec(ssl);
            if( ret != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_write_change_cipher_spec", ret );
                return( ret );
            }

            break;
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

            /* ----- WRITE ENCRYPTED EXTENSIONS ----*/

        case MBEDTLS_SSL_ENCRYPTED_EXTENSIONS:
            ret = ssl_tls13_encrypted_extensions_process( ssl );
            break;

            /* ----- WRITE CERTIFICATE REQUEST ----*/

        case MBEDTLS_SSL_CERTIFICATE_REQUEST:
            ret = ssl_tls13_certificate_request_process( ssl );
            break;

            /* ----- WRITE SERVER CERTIFICATE ----*/

        case MBEDTLS_SSL_SERVER_CERTIFICATE:
            ret = mbedtls_ssl_tls13_write_certificate( ssl );
            break;

            /* ----- WRITE SERVER CERTIFICATE VERIFY ----*/

        case MBEDTLS_SSL_CERTIFICATE_VERIFY:
            ret = mbedtls_ssl_tls13_write_certificate_verify( ssl );
            break;

            /* ----- WRITE FINISHED ----*/

        case MBEDTLS_SSL_SERVER_FINISHED:
            ret = mbedtls_ssl_tls13_write_finished_message( ssl );
            mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_EARLY_APP_DATA );
            break;

            /* ----- READ CLIENT CERTIFICATE ----*/

        case MBEDTLS_SSL_CLIENT_CERTIFICATE:
            ret = mbedtls_ssl_tls13_process_certificate( ssl );
            if( ret == 0 )
            {
                mbedtls_ssl_handshake_set_state(
                    ssl, MBEDTLS_SSL_CLIENT_CERTIFICATE_VERIFY );
            }
            break;

            /* ----- READ CLIENT CERTIFICATE VERIFY ----*/

        case MBEDTLS_SSL_CLIENT_CERTIFICATE_VERIFY:
            ret = mbedtls_ssl_tls13_process_certificate_verify( ssl );
            if( ret == 0 )
            {
                mbedtls_ssl_handshake_set_state(
                    ssl, MBEDTLS_SSL_CLIENT_FINISHED );
            }
            break;

        case MBEDTLS_SSL_END_OF_EARLY_DATA:
            ret = ssl_tls13_read_end_of_early_data_process( ssl );
            break;

            /* ----- READ FINISHED ----*/

        case MBEDTLS_SSL_CLIENT_FINISHED:
            ret = mbedtls_ssl_tls13_process_finished_message( ssl );
            if( ret == 0 )
            {
                mbedtls_ssl_handshake_set_state(
                    ssl, MBEDTLS_SSL_HANDSHAKE_WRAPUP );
            }
            break;

        case MBEDTLS_SSL_HANDSHAKE_WRAPUP:
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "handshake: done" ) );

            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to application keys for all traffic" ) );

#if defined(MBEDTLS_SSL_USE_MPS)
            ret = mbedtls_mps_set_incoming_keys( &ssl->mps->l4,
                                                 ssl->epoch_application );
            if( ret != 0 )
                return( ret );

            ret = mbedtls_mps_set_outgoing_keys( &ssl->mps->l4,
                                                 ssl->epoch_application );
            if( ret != 0 )
                return( ret );
#else
            mbedtls_ssl_set_inbound_transform ( ssl, ssl->transform_application );
            mbedtls_ssl_set_outbound_transform( ssl, ssl->transform_application );
#endif /* MBEDTLS_SSL_USE_MPS */

            mbedtls_ssl_tls13_handshake_wrapup( ssl );
            mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_NEW_SESSION_TICKET );

            break;

        case MBEDTLS_SSL_SERVER_NEW_SESSION_TICKET:

#if defined(MBEDTLS_SSL_NEW_SESSION_TICKET)

            ret = ssl_tls13_write_new_session_ticket_process( ssl );
            if( ret != 0 )
            {
                MBEDTLS_SSL_DEBUG_RET( 1, "ssl_tls13_write_new_session_ticket ", ret );
                return( ret );
            }
#endif /* MBEDTLS_SSL_NEW_SESSION_TICKET */

            break;

        case MBEDTLS_SSL_SERVER_NEW_SESSION_TICKET_FLUSH:
            ret = mbedtls_ssl_flush_output( ssl );
            if( ret != 0 )
                return( ret );
            mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_HANDSHAKE_OVER );
            break;

        default:
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "invalid state %d", ssl->state ) );
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    return( ret );
}

#endif /* MBEDTLS_SSL_SRV_C && MBEDTLS_SSL_PROTO_TLS1_3 */
