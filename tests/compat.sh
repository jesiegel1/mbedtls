#!/bin/sh

# compat.sh
#
# Copyright The Mbed TLS Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may
# not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Purpose
#
# Test interoperbility with OpenSSL, GnuTLS as well as itself.
#
# Check each common ciphersuite, with each version, both ways (client/server),
# with and without client authentication.

set -u

# Limit the size of each log to 10 GiB, in case of failures with this script
# where it may output seemingly unlimited length error logs.
ulimit -f 20971520

# initialise counters
TESTS=0
FAILED=0
SKIPPED=0
SRVMEM=0

# default commands, can be overridden by the environment
: ${M_SRV:=../programs/ssl/ssl_server2}
: ${M_CLI:=../programs/ssl/ssl_client2}
: ${OPENSSL_CMD:=openssl} # OPENSSL would conflict with the build system
: ${GNUTLS_CLI:=gnutls-cli}
: ${GNUTLS_SERV:=gnutls-serv}

# do we have a recent enough GnuTLS?
if ( which $GNUTLS_CLI && which $GNUTLS_SERV ) >/dev/null 2>&1; then
    G_VER="$( $GNUTLS_CLI --version | head -n1 )"
    if echo "$G_VER" | grep '@VERSION@' > /dev/null; then # git version
        PEER_GNUTLS=" GnuTLS"
    else
        eval $( echo $G_VER | sed 's/.* \([0-9]*\)\.\([0-9]\)*\.\([0-9]*\)$/MAJOR="\1" MINOR="\2" PATCH="\3"/' )
        if [ $MAJOR -lt 3 -o \
            \( $MAJOR -eq 3 -a $MINOR -lt 2 \) -o \
            \( $MAJOR -eq 3 -a $MINOR -eq 2 -a $PATCH -lt 15 \) ]
        then
            PEER_GNUTLS=""
        else
            PEER_GNUTLS=" GnuTLS"
            if [ $MINOR -lt 4 ]; then
                GNUTLS_MINOR_LT_FOUR='x'
            fi
        fi
    fi
else
    PEER_GNUTLS=""
fi

# default values for options
# /!\ keep this synchronised with:
# - basic-build-test.sh
# - all.sh (multiple components)
MODES="tls12 dtls12"
VERIFIES="NO YES"
TYPES="ECDSA RSA PSK"
FILTER=""
# By default, exclude:
# - NULL: excluded from our default config + requires OpenSSL legacy
# - ARIA: requires OpenSSL >= 1.1.1
# - ChachaPoly: requires OpenSSL >= 1.1.0
EXCLUDE='NULL\|ARIA\|CHACHA20-POLY1305'
VERBOSE=""
MEMCHECK=0
PEERS="OpenSSL$PEER_GNUTLS mbedTLS"

# hidden option: skip DTLS with OpenSSL
# (travis CI has a version that doesn't work for us)
: ${OSSL_NO_DTLS:=0}

print_usage() {
    echo "Usage: $0"
    printf "  -h|--help\tPrint this help.\n"
    printf "  -f|--filter\tOnly matching ciphersuites are tested (Default: '%s')\n" "$FILTER"
    printf "  -e|--exclude\tMatching ciphersuites are excluded (Default: '%s')\n" "$EXCLUDE"
    printf "  -m|--modes\tWhich modes to perform (Default: '%s')\n" "$MODES"
    printf "  -t|--types\tWhich key exchange type to perform (Default: '%s')\n" "$TYPES"
    printf "  -V|--verify\tWhich verification modes to perform (Default: '%s')\n" "$VERIFIES"
    printf "  -p|--peers\tWhich peers to use (Default: '%s')\n" "$PEERS"
    printf "            \tAlso available: GnuTLS (needs v3.2.15 or higher)\n"
    printf "  -M|--memcheck\tCheck memory leaks and errors.\n"
    printf "  -v|--verbose\tSet verbose output.\n"
}

get_options() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -f|--filter)
                shift; FILTER=$1
                ;;
            -e|--exclude)
                shift; EXCLUDE=$1
                ;;
            -m|--modes)
                shift; MODES=$1
                ;;
            -t|--types)
                shift; TYPES=$1
                ;;
            -V|--verify)
                shift; VERIFIES=$1
                ;;
            -p|--peers)
                shift; PEERS=$1
                ;;
            -v|--verbose)
                VERBOSE=1
                ;;
            -M|--memcheck)
                MEMCHECK=1
                ;;
            -h|--help)
                print_usage
                exit 0
                ;;
            *)
                echo "Unknown argument: '$1'"
                print_usage
                exit 1
                ;;
        esac
        shift
    done

    # sanitize some options (modes checked later)
    VERIFIES="$( echo $VERIFIES | tr [a-z] [A-Z] )"
    TYPES="$( echo $TYPES | tr [a-z] [A-Z] )"
}

log() {
  if [ "X" != "X$VERBOSE" ]; then
    echo ""
    echo "$@"
  fi
}

# is_dtls <mode>
is_dtls()
{
    test "$1" = "dtls12"
}

# minor_ver <mode>
minor_ver()
{
    case "$1" in
        tls12|dtls12)
            echo 3
            ;;
        tls13)
            echo 4
            ;;
        *)
            echo "error: invalid mode: $MODE" >&2
            # exiting is no good here, typically called in a subshell
            echo -1
    esac
}

filter()
{
  LIST="$1"
  NEW_LIST=""

  EXCLMODE="$EXCLUDE"

  for i in $LIST;
  do
    NEW_LIST="$NEW_LIST $( echo "$i" | grep "$FILTER" | grep -v "$EXCLMODE" )"
  done

  # normalize whitespace
  echo "$NEW_LIST" | sed -e 's/[[:space:]][[:space:]]*/ /g' -e 's/^ //' -e 's/ $//'
}

# OpenSSL 1.0.1h with -Verify wants a ClientCertificate message even for
# PSK ciphersuites with DTLS, which is incorrect, so disable them for now
check_openssl_server_bug()
{
    if test "X$VERIFY" = "XYES" && is_dtls "$MODE" && \
        echo "$1" | grep "^TLS-PSK" >/dev/null;
    then
        SKIP_NEXT="YES"
    fi
}

filter_ciphersuites()
{
    if [ "X" != "X$FILTER" -o "X" != "X$EXCLUDE" ];
    then
        # Ciphersuite for mbed TLS
        M_CIPHERS=$( filter "$M_CIPHERS" )

        # Ciphersuite for OpenSSL
        O_CIPHERS=$( filter "$O_CIPHERS" )

        # Ciphersuite for GnuTLS
        G_CIPHERS=$( filter "$G_CIPHERS" )
    fi

    # For GnuTLS client -> mbed TLS server,
    # we need to force IPv4 by connecting to 127.0.0.1 but then auth fails
    if [ "X$VERIFY" = "XYES" ] && is_dtls "$MODE"; then
        G_CIPHERS=""
    fi
}

reset_ciphersuites()
{
    M_CIPHERS=""
    O_CIPHERS=""
    G_CIPHERS=""
}

check_translation()
{
    if [ $1 -ne 0 ]; then
        echo "translate_ciphers.py failed with exit code $1" >&2
        echo "$2" >&2
        exit 1
    fi
}

# Ciphersuites that can be used with all peers.
# Since we currently have three possible peers, each ciphersuite should appear
# three times: in each peer's list (with the name that this peer uses).
add_common_ciphersuites()
{
    CIPHERS=""
    case $TYPE in

        "ECDSA")
            CIPHERS="$CIPHERS                           \
                TLS-ECDHE-ECDSA-WITH-AES-128-CBC-SHA    \
                TLS-ECDHE-ECDSA-WITH-AES-128-CBC-SHA256 \
                TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256 \
                TLS-ECDHE-ECDSA-WITH-AES-256-CBC-SHA    \
                TLS-ECDHE-ECDSA-WITH-AES-256-CBC-SHA384 \
                TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384 \
                TLS-ECDHE-ECDSA-WITH-NULL-SHA           \
                "
            ;;

        "RSA")
            CIPHERS="$CIPHERS                           \
                TLS-DHE-RSA-WITH-AES-128-CBC-SHA        \
                TLS-DHE-RSA-WITH-AES-128-CBC-SHA256     \
                TLS-DHE-RSA-WITH-AES-128-GCM-SHA256     \
                TLS-DHE-RSA-WITH-AES-256-CBC-SHA        \
                TLS-DHE-RSA-WITH-AES-256-CBC-SHA256     \
                TLS-DHE-RSA-WITH-AES-256-GCM-SHA384     \
                TLS-DHE-RSA-WITH-CAMELLIA-128-CBC-SHA   \
                TLS-DHE-RSA-WITH-CAMELLIA-256-CBC-SHA   \
                TLS-ECDHE-RSA-WITH-AES-128-CBC-SHA      \
                TLS-ECDHE-RSA-WITH-AES-128-CBC-SHA256   \
                TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256   \
                TLS-ECDHE-RSA-WITH-AES-256-CBC-SHA      \
                TLS-ECDHE-RSA-WITH-AES-256-CBC-SHA384   \
                TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384   \
                TLS-ECDHE-RSA-WITH-NULL-SHA             \
                TLS-RSA-WITH-AES-128-CBC-SHA            \
                TLS-RSA-WITH-AES-128-CBC-SHA256         \
                TLS-RSA-WITH-AES-128-GCM-SHA256         \
                TLS-RSA-WITH-AES-256-CBC-SHA            \
                TLS-RSA-WITH-AES-256-CBC-SHA256         \
                TLS-RSA-WITH-AES-256-GCM-SHA384         \
                TLS-RSA-WITH-CAMELLIA-128-CBC-SHA       \
                TLS-RSA-WITH-CAMELLIA-256-CBC-SHA       \
                TLS-RSA-WITH-NULL-MD5                   \
                TLS-RSA-WITH-NULL-SHA                   \
                TLS-RSA-WITH-NULL-SHA256                \
                "
            ;;

        "PSK")
            CIPHERS="$CIPHERS                           \
                TLS-PSK-WITH-AES-128-CBC-SHA            \
                TLS-PSK-WITH-AES-256-CBC-SHA            \
                "
            ;;
    esac

    M_CIPHERS="$M_CIPHERS $CIPHERS"

    T=$(./scripts/translate_ciphers.py g $CIPHERS)
    check_translation $? "$T"
    G_CIPHERS="$G_CIPHERS $T"

    T=$(./scripts/translate_ciphers.py o $CIPHERS)
    check_translation $? "$T"
    O_CIPHERS="$O_CIPHERS $T"
}

# Ciphersuites usable only with Mbed TLS and OpenSSL
# A list of ciphersuites in the Mbed TLS convention is compiled and
# appended to the list of Mbed TLS ciphersuites $M_CIPHERS. The same list
# is translated to the OpenSSL naming convention and appended to the list of
# OpenSSL ciphersuites $O_CIPHERS.
#
# NOTE: for some reason RSA-PSK doesn't work with OpenSSL,
# so RSA-PSK ciphersuites need to go in other sections, see
# https://github.com/Mbed-TLS/mbedtls/issues/1419
#
# ChachaPoly suites are here rather than in "common", as they were added in
# GnuTLS in 3.5.0 and the CI only has 3.4.x so far.
add_openssl_ciphersuites()
{
    CIPHERS=""
    case $TYPE in

        "ECDSA")
            CIPHERS="$CIPHERS                                   \
                TLS-ECDH-ECDSA-WITH-AES-128-CBC-SHA             \
                TLS-ECDH-ECDSA-WITH-AES-128-CBC-SHA256          \
                TLS-ECDH-ECDSA-WITH-AES-128-GCM-SHA256          \
                TLS-ECDH-ECDSA-WITH-AES-256-CBC-SHA             \
                TLS-ECDH-ECDSA-WITH-AES-256-CBC-SHA384          \
                TLS-ECDH-ECDSA-WITH-AES-256-GCM-SHA384          \
                TLS-ECDH-ECDSA-WITH-NULL-SHA                    \
                TLS-ECDHE-ECDSA-WITH-ARIA-128-GCM-SHA256        \
                TLS-ECDHE-ECDSA-WITH-ARIA-256-GCM-SHA384        \
                TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256   \
                "
            ;;

        "RSA")
            CIPHERS="$CIPHERS                                   \
                TLS-DHE-RSA-WITH-ARIA-128-GCM-SHA256            \
                TLS-DHE-RSA-WITH-ARIA-256-GCM-SHA384            \
                TLS-DHE-RSA-WITH-CHACHA20-POLY1305-SHA256       \
                TLS-ECDHE-RSA-WITH-ARIA-128-GCM-SHA256          \
                TLS-ECDHE-RSA-WITH-ARIA-256-GCM-SHA384          \
                TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256     \
                TLS-RSA-WITH-ARIA-128-GCM-SHA256                \
                TLS-RSA-WITH-ARIA-256-GCM-SHA384                \
                "
            ;;

        "PSK")
            CIPHERS="$CIPHERS                                   \
                TLS-DHE-PSK-WITH-ARIA-128-GCM-SHA256            \
                TLS-DHE-PSK-WITH-ARIA-256-GCM-SHA384            \
                TLS-DHE-PSK-WITH-CHACHA20-POLY1305-SHA256       \
                TLS-ECDHE-PSK-WITH-CHACHA20-POLY1305-SHA256     \
                TLS-PSK-WITH-ARIA-128-GCM-SHA256                \
                TLS-PSK-WITH-ARIA-256-GCM-SHA384                \
                TLS-PSK-WITH-CHACHA20-POLY1305-SHA256           \
                "
            ;;
    esac

    M_CIPHERS="$M_CIPHERS $CIPHERS"

    T=$(./scripts/translate_ciphers.py o $CIPHERS)
    check_translation $? "$T"
    O_CIPHERS="$O_CIPHERS $T"
}

# Ciphersuites usable only with Mbed TLS and GnuTLS
# A list of ciphersuites in the Mbed TLS convention is compiled and
# appended to the list of Mbed TLS ciphersuites $M_CIPHERS. The same list
# is translated to the GnuTLS naming convention and appended to the list of
# GnuTLS ciphersuites $G_CIPHERS.
add_gnutls_ciphersuites()
{
    CIPHERS=""
    case $TYPE in

        "ECDSA")
            CIPHERS="$CIPHERS                                       \
                TLS-ECDHE-ECDSA-WITH-AES-128-CCM                    \
                TLS-ECDHE-ECDSA-WITH-AES-128-CCM-8                  \
                TLS-ECDHE-ECDSA-WITH-AES-256-CCM                    \
                TLS-ECDHE-ECDSA-WITH-AES-256-CCM-8                  \
                TLS-ECDHE-ECDSA-WITH-CAMELLIA-128-CBC-SHA256        \
                TLS-ECDHE-ECDSA-WITH-CAMELLIA-128-GCM-SHA256        \
                TLS-ECDHE-ECDSA-WITH-CAMELLIA-256-CBC-SHA384        \
                TLS-ECDHE-ECDSA-WITH-CAMELLIA-256-GCM-SHA384        \
                "
            ;;

        "RSA")
            CIPHERS="$CIPHERS                               \
                TLS-DHE-RSA-WITH-AES-128-CCM                \
                TLS-DHE-RSA-WITH-AES-128-CCM-8              \
                TLS-DHE-RSA-WITH-AES-256-CCM                \
                TLS-DHE-RSA-WITH-AES-256-CCM-8              \
                TLS-DHE-RSA-WITH-CAMELLIA-128-CBC-SHA256    \
                TLS-DHE-RSA-WITH-CAMELLIA-128-GCM-SHA256    \
                TLS-DHE-RSA-WITH-CAMELLIA-256-CBC-SHA256    \
                TLS-DHE-RSA-WITH-CAMELLIA-256-GCM-SHA384    \
                TLS-ECDHE-RSA-WITH-CAMELLIA-128-CBC-SHA256  \
                TLS-ECDHE-RSA-WITH-CAMELLIA-128-GCM-SHA256  \
                TLS-ECDHE-RSA-WITH-CAMELLIA-256-CBC-SHA384  \
                TLS-ECDHE-RSA-WITH-CAMELLIA-256-GCM-SHA384  \
                TLS-RSA-WITH-AES-128-CCM                    \
                TLS-RSA-WITH-AES-128-CCM-8                  \
                TLS-RSA-WITH-AES-256-CCM                    \
                TLS-RSA-WITH-AES-256-CCM-8                  \
                TLS-RSA-WITH-CAMELLIA-128-CBC-SHA256        \
                TLS-RSA-WITH-CAMELLIA-128-GCM-SHA256        \
                TLS-RSA-WITH-CAMELLIA-256-CBC-SHA256        \
                TLS-RSA-WITH-CAMELLIA-256-GCM-SHA384        \
                "
            ;;

        "PSK")
            CIPHERS="$CIPHERS                               \
                TLS-DHE-PSK-WITH-AES-128-CBC-SHA            \
                TLS-DHE-PSK-WITH-AES-128-CBC-SHA256         \
                TLS-DHE-PSK-WITH-AES-128-CCM                \
                TLS-DHE-PSK-WITH-AES-128-CCM-8              \
                TLS-DHE-PSK-WITH-AES-128-GCM-SHA256         \
                TLS-DHE-PSK-WITH-AES-256-CBC-SHA            \
                TLS-DHE-PSK-WITH-AES-256-CBC-SHA384         \
                TLS-DHE-PSK-WITH-AES-256-CCM                \
                TLS-DHE-PSK-WITH-AES-256-CCM-8              \
                TLS-DHE-PSK-WITH-AES-256-GCM-SHA384         \
                TLS-DHE-PSK-WITH-CAMELLIA-128-CBC-SHA256    \
                TLS-DHE-PSK-WITH-CAMELLIA-128-GCM-SHA256    \
                TLS-DHE-PSK-WITH-CAMELLIA-256-CBC-SHA384    \
                TLS-DHE-PSK-WITH-CAMELLIA-256-GCM-SHA384    \
                TLS-DHE-PSK-WITH-NULL-SHA256                \
                TLS-DHE-PSK-WITH-NULL-SHA384                \
                TLS-ECDHE-PSK-WITH-AES-128-CBC-SHA          \
                TLS-ECDHE-PSK-WITH-AES-128-CBC-SHA256       \
                TLS-ECDHE-PSK-WITH-AES-256-CBC-SHA          \
                TLS-ECDHE-PSK-WITH-AES-256-CBC-SHA384       \
                TLS-ECDHE-PSK-WITH-CAMELLIA-128-CBC-SHA256  \
                TLS-ECDHE-PSK-WITH-CAMELLIA-256-CBC-SHA384  \
                TLS-ECDHE-PSK-WITH-NULL-SHA256              \
                TLS-ECDHE-PSK-WITH-NULL-SHA384              \
                TLS-PSK-WITH-AES-128-CBC-SHA256             \
                TLS-PSK-WITH-AES-128-CCM                    \
                TLS-PSK-WITH-AES-128-CCM-8                  \
                TLS-PSK-WITH-AES-128-GCM-SHA256             \
                TLS-PSK-WITH-AES-256-CBC-SHA384             \
                TLS-PSK-WITH-AES-256-CCM                    \
                TLS-PSK-WITH-AES-256-CCM-8                  \
                TLS-PSK-WITH-AES-256-GCM-SHA384             \
                TLS-PSK-WITH-CAMELLIA-128-CBC-SHA256        \
                TLS-PSK-WITH-CAMELLIA-128-GCM-SHA256        \
                TLS-PSK-WITH-CAMELLIA-256-CBC-SHA384        \
                TLS-PSK-WITH-CAMELLIA-256-GCM-SHA384        \
                TLS-PSK-WITH-NULL-SHA256                    \
                TLS-PSK-WITH-NULL-SHA384                    \
                TLS-RSA-PSK-WITH-AES-128-CBC-SHA            \
                TLS-RSA-PSK-WITH-AES-128-CBC-SHA256         \
                TLS-RSA-PSK-WITH-AES-128-GCM-SHA256         \
                TLS-RSA-PSK-WITH-AES-256-CBC-SHA            \
                TLS-RSA-PSK-WITH-AES-256-CBC-SHA384         \
                TLS-RSA-PSK-WITH-AES-256-GCM-SHA384         \
                TLS-RSA-PSK-WITH-CAMELLIA-128-CBC-SHA256    \
                TLS-RSA-PSK-WITH-CAMELLIA-128-GCM-SHA256    \
                TLS-RSA-PSK-WITH-CAMELLIA-256-CBC-SHA384    \
                TLS-RSA-PSK-WITH-CAMELLIA-256-GCM-SHA384    \
                TLS-RSA-PSK-WITH-NULL-SHA256                \
                TLS-RSA-PSK-WITH-NULL-SHA384                \
                "
            ;;
    esac

    M_CIPHERS="$M_CIPHERS $CIPHERS"

    T=$(./scripts/translate_ciphers.py g $CIPHERS)
    check_translation $? "$T"
    G_CIPHERS="$G_CIPHERS $T"
}

# Ciphersuites usable only with Mbed TLS (not currently supported by another
# peer usable in this script). This provide only very rudimentaty testing, as
# this is not interop testing, but it's better than nothing.
add_mbedtls_ciphersuites()
{
    case $TYPE in

        "ECDSA")
            M_CIPHERS="$M_CIPHERS                               \
                TLS-ECDH-ECDSA-WITH-ARIA-128-CBC-SHA256         \
                TLS-ECDH-ECDSA-WITH-ARIA-128-GCM-SHA256         \
                TLS-ECDH-ECDSA-WITH-ARIA-256-CBC-SHA384         \
                TLS-ECDH-ECDSA-WITH-ARIA-256-GCM-SHA384         \
                TLS-ECDH-ECDSA-WITH-CAMELLIA-128-CBC-SHA256     \
                TLS-ECDH-ECDSA-WITH-CAMELLIA-128-GCM-SHA256     \
                TLS-ECDH-ECDSA-WITH-CAMELLIA-256-CBC-SHA384     \
                TLS-ECDH-ECDSA-WITH-CAMELLIA-256-GCM-SHA384     \
                TLS-ECDHE-ECDSA-WITH-ARIA-128-CBC-SHA256        \
                TLS-ECDHE-ECDSA-WITH-ARIA-256-CBC-SHA384        \
                "
            ;;

        "RSA")
            M_CIPHERS="$M_CIPHERS                               \
                TLS-DHE-RSA-WITH-ARIA-128-CBC-SHA256            \
                TLS-DHE-RSA-WITH-ARIA-256-CBC-SHA384            \
                TLS-ECDHE-RSA-WITH-ARIA-128-CBC-SHA256          \
                TLS-ECDHE-RSA-WITH-ARIA-256-CBC-SHA384          \
                TLS-RSA-WITH-ARIA-128-CBC-SHA256                \
                TLS-RSA-WITH-ARIA-256-CBC-SHA384                \
                "
            ;;

        "PSK")
            # *PSK-NULL-SHA suites supported by GnuTLS 3.3.5 but not 3.2.15
            M_CIPHERS="$M_CIPHERS                               \
                TLS-DHE-PSK-WITH-ARIA-128-CBC-SHA256            \
                TLS-DHE-PSK-WITH-ARIA-256-CBC-SHA384            \
                TLS-DHE-PSK-WITH-NULL-SHA                       \
                TLS-ECDHE-PSK-WITH-ARIA-128-CBC-SHA256          \
                TLS-ECDHE-PSK-WITH-ARIA-256-CBC-SHA384          \
                TLS-ECDHE-PSK-WITH-NULL-SHA                     \
                TLS-PSK-WITH-ARIA-128-CBC-SHA256                \
                TLS-PSK-WITH-ARIA-256-CBC-SHA384                \
                TLS-PSK-WITH-NULL-SHA                           \
                TLS-RSA-PSK-WITH-ARIA-128-CBC-SHA256            \
                TLS-RSA-PSK-WITH-ARIA-128-GCM-SHA256            \
                TLS-RSA-PSK-WITH-ARIA-256-CBC-SHA384            \
                TLS-RSA-PSK-WITH-ARIA-256-GCM-SHA384            \
                TLS-RSA-PSK-WITH-CHACHA20-POLY1305-SHA256       \
                TLS-RSA-PSK-WITH-NULL-SHA                       \
                "
            ;;
    esac
}

setup_arguments()
{
    O_MODE=""
    G_MODE=""
    case "$MODE" in
        "tls12")
            O_MODE="tls1_2"
            G_PRIO_MODE="+VERS-TLS1.2"
            ;;
        "tls13")
            G_PRIO_MODE="+VERS-TLS1.3"
            O_MODE="tls1_3"
            OPENSSL_CMD=${OPENSSL_NEXT}
            GNUTLS_CLI=${GNUTLS_NEXT_CLI}
            GNUTLS_SERV=${GNUTLS_NEXT_SERV}
            ;;
        "dtls12")
            O_MODE="dtls1_2"
            G_PRIO_MODE="+VERS-DTLS1.2"
            G_MODE="-u"
            ;;
        *)
            echo "error: invalid mode: $MODE" >&2
            exit 1;
    esac

    # GnuTLS < 3.4 will choke if we try to allow CCM-8
    if [ -z "${GNUTLS_MINOR_LT_FOUR-}" ]; then
        G_PRIO_CCM="+AES-256-CCM-8:+AES-128-CCM-8:"
    else
        G_PRIO_CCM=""
    fi

    if [ `minor_ver "$MODE"` -ge 4 ]
    then
        O_SERVER_ARGS="-accept $PORT -ciphersuites TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_CCM_SHA256:TLS_AES_128_CCM_8_SHA256 --$O_MODE"
        M_SERVER_ARGS="server_port=$PORT server_addr=0.0.0.0 force_version=$MODE"
        G_SERVER_PRIO="NORMAL:${G_PRIO_CCM}${G_PRIO_MODE}"
    else
        M_SERVER_ARGS="server_port=$PORT server_addr=0.0.0.0 force_version=$MODE"
        O_SERVER_ARGS="-accept $PORT -cipher NULL,ALL -$O_MODE"
        G_SERVER_PRIO="NORMAL:${G_PRIO_CCM}+NULL:+MD5:+PSK:+DHE-PSK:+ECDHE-PSK:+SHA256:+SHA384:+RSA-PSK:-VERS-TLS-ALL:$G_PRIO_MODE"
    fi

    G_SERVER_ARGS="-p $PORT --http $G_MODE"

    # The default prime for `openssl s_server` depends on the version:
    # * OpenSSL <= 1.0.2a: 512-bit
    # * OpenSSL 1.0.2b to 1.1.1b: 1024-bit
    # * OpenSSL >= 1.1.1c: 2048-bit
    # Mbed TLS wants >=1024, so force that for older versions. Don't force
    # it for newer versions, which reject a 1024-bit prime. Indifferently
    # force it or not for intermediate versions.
    case $($OPENSSL_CMD version) in
        "OpenSSL 1.0"*)
            O_SERVER_ARGS="$O_SERVER_ARGS -dhparam data_files/dhparams.pem"
            ;;
    esac

    # with OpenSSL 1.0.1h, -www, -WWW and -HTTP break DTLS handshakes
    if is_dtls "$MODE"; then
        O_SERVER_ARGS="$O_SERVER_ARGS"
    else
        O_SERVER_ARGS="$O_SERVER_ARGS -www"
    fi

    M_CLIENT_ARGS="server_port=$PORT server_addr=127.0.0.1 force_version=$MODE"
    O_CLIENT_ARGS="-connect localhost:$PORT -$O_MODE"
    G_CLIENT_ARGS="-p $PORT --debug 3 $G_MODE"
    G_CLIENT_PRIO="NONE:$G_PRIO_MODE:+COMP-NULL:+CURVE-ALL:+SIGN-ALL"

    if [ "X$VERIFY" = "XYES" ];
    then
        M_SERVER_ARGS="$M_SERVER_ARGS ca_file=data_files/test-ca_cat12.crt auth_mode=required"
        O_SERVER_ARGS="$O_SERVER_ARGS -CAfile data_files/test-ca_cat12.crt -Verify 10"
        G_SERVER_ARGS="$G_SERVER_ARGS --x509cafile data_files/test-ca_cat12.crt --require-client-cert"

        M_CLIENT_ARGS="$M_CLIENT_ARGS ca_file=data_files/test-ca_cat12.crt auth_mode=required"
        O_CLIENT_ARGS="$O_CLIENT_ARGS -CAfile data_files/test-ca_cat12.crt -verify 10"
        G_CLIENT_ARGS="$G_CLIENT_ARGS --x509cafile data_files/test-ca_cat12.crt"
    else
        # don't request a client cert at all
        M_SERVER_ARGS="$M_SERVER_ARGS ca_file=none auth_mode=none"
        G_SERVER_ARGS="$G_SERVER_ARGS --disable-client-cert"

        M_CLIENT_ARGS="$M_CLIENT_ARGS ca_file=none auth_mode=none"
        O_CLIENT_ARGS="$O_CLIENT_ARGS"
        G_CLIENT_ARGS="$G_CLIENT_ARGS --insecure"
    fi

    case $TYPE in
        "ECDSA")
            M_SERVER_ARGS="$M_SERVER_ARGS crt_file=data_files/server5.crt key_file=data_files/server5.key"
            O_SERVER_ARGS="$O_SERVER_ARGS -cert data_files/server5.crt -key data_files/server5.key"
            G_SERVER_ARGS="$G_SERVER_ARGS --x509certfile data_files/server5.crt --x509keyfile data_files/server5.key"

            if [ "X$VERIFY" = "XYES" ]; then
                M_CLIENT_ARGS="$M_CLIENT_ARGS crt_file=data_files/server6.crt key_file=data_files/server6.key"
                O_CLIENT_ARGS="$O_CLIENT_ARGS -cert data_files/server6.crt -key data_files/server6.key"
                G_CLIENT_ARGS="$G_CLIENT_ARGS --x509certfile data_files/server6.crt --x509keyfile data_files/server6.key"
            else
                M_CLIENT_ARGS="$M_CLIENT_ARGS crt_file=none key_file=none"
            fi
            ;;

        "RSA")
            M_SERVER_ARGS="$M_SERVER_ARGS crt_file=data_files/server2-sha256.crt key_file=data_files/server2.key"
            O_SERVER_ARGS="$O_SERVER_ARGS -cert data_files/server2-sha256.crt -key data_files/server2.key"
            G_SERVER_ARGS="$G_SERVER_ARGS --x509certfile data_files/server2-sha256.crt --x509keyfile data_files/server2.key"

            if [ "X$VERIFY" = "XYES" ]; then
                M_CLIENT_ARGS="$M_CLIENT_ARGS crt_file=data_files/cert_sha256.crt key_file=data_files/server1.key"
                O_CLIENT_ARGS="$O_CLIENT_ARGS -cert data_files/cert_sha256.crt -key data_files/server1.key"
                G_CLIENT_ARGS="$G_CLIENT_ARGS --x509certfile data_files/cert_sha256.crt --x509keyfile data_files/server1.key"
            else
                M_CLIENT_ARGS="$M_CLIENT_ARGS crt_file=none key_file=none"
            fi
            ;;

        "PSK")
            # give RSA-PSK-capable server a RSA cert
            # (should be a separate type, but harder to close with openssl)
            M_SERVER_ARGS="$M_SERVER_ARGS psk=6162636465666768696a6b6c6d6e6f70 ca_file=none crt_file=data_files/server2-sha256.crt key_file=data_files/server2.key"
            O_SERVER_ARGS="$O_SERVER_ARGS -psk 6162636465666768696a6b6c6d6e6f70 -nocert"
            G_SERVER_ARGS="$G_SERVER_ARGS --x509certfile data_files/server2-sha256.crt --x509keyfile data_files/server2.key --pskpasswd data_files/passwd.psk"

            M_CLIENT_ARGS="$M_CLIENT_ARGS psk=6162636465666768696a6b6c6d6e6f70 crt_file=none key_file=none"
            O_CLIENT_ARGS="$O_CLIENT_ARGS -psk 6162636465666768696a6b6c6d6e6f70"
            G_CLIENT_ARGS="$G_CLIENT_ARGS --pskusername Client_identity --pskkey=6162636465666768696a6b6c6d6e6f70"
            ;;
    esac
}

# is_mbedtls <cmd_line>
is_mbedtls() {
    echo "$1" | grep 'ssl_server2\|ssl_client2' > /dev/null
}

# has_mem_err <log_file_name>
has_mem_err() {
    if ( grep -F 'All heap blocks were freed -- no leaks are possible' "$1" &&
         grep -F 'ERROR SUMMARY: 0 errors from 0 contexts' "$1" ) > /dev/null
    then
        return 1 # false: does not have errors
    else
        return 0 # true: has errors
    fi
}

# Wait for process $2 to be listening on port $1
if type lsof >/dev/null 2>/dev/null; then
    wait_server_start() {
        START_TIME=$(date +%s)
        if is_dtls "$MODE"; then
            proto=UDP
        else
            proto=TCP
        fi
        while ! lsof -a -n -b -i "$proto:$1" -p "$2" >/dev/null 2>/dev/null; do
              if [ $(( $(date +%s) - $START_TIME )) -gt $DOG_DELAY ]; then
                  echo "SERVERSTART TIMEOUT"
                  echo "SERVERSTART TIMEOUT" >> $SRV_OUT
                  break
              fi
              # Linux and *BSD support decimal arguments to sleep. On other
              # OSes this may be a tight loop.
              sleep 0.1 2>/dev/null || true
        done
    }
else
    echo "Warning: lsof not available, wait_server_start = sleep"
    wait_server_start() {
        sleep 2
    }
fi


# start_server <name>
# also saves name and command
start_server() {
    case $1 in
        [Oo]pen*)
            SERVER_CMD="$OPENSSL_CMD s_server $O_SERVER_ARGS"
            ;;
        [Gg]nu*)
            SERVER_CMD="$GNUTLS_SERV $G_SERVER_ARGS --priority $G_SERVER_PRIO"
            ;;
        mbed*)
            SERVER_CMD="$M_SRV $M_SERVER_ARGS"
            if [ "$MEMCHECK" -gt 0 ]; then
                SERVER_CMD="valgrind --leak-check=full $SERVER_CMD"
            fi
            ;;
        *)
            echo "error: invalid server name: $1" >&2
            exit 1
            ;;
    esac
    SERVER_NAME=$1

    log "$SERVER_CMD"
    echo "$SERVER_CMD" > $SRV_OUT
    # for servers without -www or equivalent
    while :; do echo bla; sleep 1; done | $SERVER_CMD >> $SRV_OUT 2>&1 &
    PROCESS_ID=$!

    wait_server_start "$PORT" "$PROCESS_ID"
}

# terminate the running server
stop_server() {
    kill $PROCESS_ID 2>/dev/null
    wait $PROCESS_ID 2>/dev/null

    if [ "$MEMCHECK" -gt 0 ]; then
        if is_mbedtls "$SERVER_CMD" && has_mem_err $SRV_OUT; then
            echo "  ! Server had memory errors"
            SRVMEM=$(( $SRVMEM + 1 ))
            return
        fi
    fi

    rm -f $SRV_OUT
}

# kill the running server (used when killed by signal)
cleanup() {
    rm -f $SRV_OUT $CLI_OUT
    kill $PROCESS_ID >/dev/null 2>&1
    kill $WATCHDOG_PID >/dev/null 2>&1
    exit 1
}

# wait for client to terminate and set EXIT
# must be called right after starting the client
wait_client_done() {
    CLI_PID=$!

    ( sleep "$DOG_DELAY"; echo "TIMEOUT" >> $CLI_OUT; kill $CLI_PID ) &
    WATCHDOG_PID=$!

    wait $CLI_PID
    EXIT=$?

    kill $WATCHDOG_PID
    wait $WATCHDOG_PID

    echo "EXIT: $EXIT" >> $CLI_OUT
}

# run_client <name> <cipher>
run_client() {
    # announce what we're going to do
    TESTS=$(( $TESTS + 1 ))
    VERIF=$(echo $VERIFY | tr '[:upper:]' '[:lower:]')
    TITLE="`echo $1 | head -c1`->`echo $SERVER_NAME | head -c1`"
    TITLE="$TITLE $MODE,$VERIF $2"
    printf "%s " "$TITLE"
    LEN=$(( 72 - `echo "$TITLE" | wc -c` ))
    for i in `seq 1 $LEN`; do printf '.'; done; printf ' '

    # should we skip?
    if [ "X$SKIP_NEXT" = "XYES" ]; then
        SKIP_NEXT="NO"
        echo "SKIP"
        SKIPPED=$(( $SKIPPED + 1 ))
        return
    fi

    # run the command and interpret result
    case $1 in
        [Oo]pen*)
            if [ `minor_ver "$MODE"` -ge 4 ]
            then
                CLIENT_CMD="$OPENSSL_CMD s_client $O_CLIENT_ARGS -ciphersuites $2"
            else
                CLIENT_CMD="$OPENSSL_CMD s_client $O_CLIENT_ARGS -cipher $2"
            fi
            log "$CLIENT_CMD"
            echo "$CLIENT_CMD" > $CLI_OUT
            printf 'GET HTTP/1.0\r\n\r\n' | $CLIENT_CMD >> $CLI_OUT 2>&1 &
            wait_client_done

            if [ $EXIT -eq 0 ]; then
                RESULT=0
            else
                # If the cipher isn't supported...
                if grep 'Cipher is (NONE)' $CLI_OUT >/dev/null; then
                    RESULT=1
                else
                    RESULT=2
                fi
            fi
            ;;

        [Gg]nu*)
            # need to force IPv4 with UDP, but keep localhost for auth
            if is_dtls "$MODE"; then
                G_HOST="127.0.0.1"
            else
                G_HOST="localhost"
            fi

            if [ `minor_ver "$MODE"` -ge 4 ]
            then
                G_CLIENT_PRIO="NONE:${2}:+GROUP-SECP256R1:+GROUP-SECP384R1:+CTYPE-ALL:+ECDHE-ECDSA:+CIPHER-ALL:+MAC-ALL:-SHA1:-AES-128-CBC:+SIGN-ECDSA-SECP256R1-SHA256:+SIGN-ECDSA-SECP384R1-SHA384:+ECDHE-ECDSA:${G_PRIO_MODE}"
                CLIENT_CMD="$GNUTLS_CLI $G_CLIENT_ARGS --priority $G_CLIENT_PRIO $G_HOST"
            else
                CLIENT_CMD="$GNUTLS_CLI $G_CLIENT_ARGS --priority $G_PRIO_MODE:$2 $G_HOST"
            fi

            log "$CLIENT_CMD"
            echo "$CLIENT_CMD" > $CLI_OUT
            printf 'GET HTTP/1.0\r\n\r\n' | $CLIENT_CMD >> $CLI_OUT 2>&1 &
            wait_client_done

            if [ $EXIT -eq 0 ]; then
                RESULT=0
            else
                RESULT=2
                # interpret early failure, with a handshake_failure alert
                # before the server hello, as "no ciphersuite in common"
                if grep -F 'Received alert [40]: Handshake failed' $CLI_OUT; then
                    if grep -i 'SERVER HELLO .* was received' $CLI_OUT; then :
                    else
                        RESULT=1
                    fi
                fi >/dev/null
            fi
            ;;

        mbed*)
            CLIENT_CMD="$M_CLI $M_CLIENT_ARGS force_ciphersuite=$2"
            if [ "$MEMCHECK" -gt 0 ]; then
                CLIENT_CMD="valgrind --leak-check=full $CLIENT_CMD"
            fi
            log "$CLIENT_CMD"
            echo "$CLIENT_CMD" > $CLI_OUT
            $CLIENT_CMD >> $CLI_OUT 2>&1 &
            wait_client_done

            case $EXIT in
                # Success
                "0")    RESULT=0    ;;

                # Ciphersuite not supported
                "2")    RESULT=1    ;;

                # Error
                *)      RESULT=2    ;;
            esac

            if [ "$MEMCHECK" -gt 0 ]; then
                if is_mbedtls "$CLIENT_CMD" && has_mem_err $CLI_OUT; then
                    RESULT=2
                fi
            fi

            ;;

        *)
            echo "error: invalid client name: $1" >&2
            exit 1
            ;;
    esac

    echo "EXIT: $EXIT" >> $CLI_OUT

    # report and count result
    case $RESULT in
        "0")
            echo PASS
            ;;
        "1")
            echo SKIP
            SKIPPED=$(( $SKIPPED + 1 ))
            ;;
        "2")
            echo FAIL
            cp $SRV_OUT c-srv-${TESTS}.log
            cp $CLI_OUT c-cli-${TESTS}.log
            echo "  ! outputs saved to c-srv-${TESTS}.log, c-cli-${TESTS}.log"

            if [ "${LOG_FAILURE_ON_STDOUT:-0}" != 0 ]; then
                echo "  ! server output:"
                cat c-srv-${TESTS}.log
                echo "  ! ==================================================="
                echo "  ! client output:"
                cat c-cli-${TESTS}.log
            fi

            FAILED=$(( $FAILED + 1 ))
            ;;
    esac

    rm -f $CLI_OUT
}

#
# MAIN
#

if cd $( dirname $0 ); then :; else
    echo "cd $( dirname $0 ) failed" >&2
    exit 1
fi

get_options "$@"

# sanity checks, avoid an avalanche of errors
if [ ! -x "$M_SRV" ]; then
    echo "Command '$M_SRV' is not an executable file" >&2
    exit 1
fi
if [ ! -x "$M_CLI" ]; then
    echo "Command '$M_CLI' is not an executable file" >&2
    exit 1
fi

if echo "$PEERS" | grep -i openssl > /dev/null; then
    if which "$OPENSSL_CMD" >/dev/null 2>&1; then :; else
        echo "Command '$OPENSSL_CMD' not found" >&2
        exit 1
    fi
fi

if echo "$PEERS" | grep -i gnutls > /dev/null; then
    for CMD in "$GNUTLS_CLI" "$GNUTLS_SERV"; do
        if which "$CMD" >/dev/null 2>&1; then :; else
            echo "Command '$CMD' not found" >&2
            exit 1
        fi
    done
fi

for PEER in $PEERS; do
    case "$PEER" in
        mbed*|[Oo]pen*|[Gg]nu*)
            ;;
        *)
            echo "Unknown peers: $PEER" >&2
            exit 1
    esac
done

# Pick a "unique" port in the range 10000-19999.
PORT="0000$$"
PORT="1$(echo $PORT | tail -c 5)"

# Also pick a unique name for intermediate files
SRV_OUT="srv_out.$$"
CLI_OUT="cli_out.$$"

# client timeout delay: be more patient with valgrind
if [ "$MEMCHECK" -gt 0 ]; then
    DOG_DELAY=30
else
    DOG_DELAY=10
fi

SKIP_NEXT="NO"

trap cleanup INT TERM HUP

for VERIFY in $VERIFIES; do
    for MODE in $MODES; do
        for TYPE in $TYPES; do
            for PEER in $PEERS; do

            setup_arguments

            case "$PEER" in

                [Oo]pen*)

                    if test "$OSSL_NO_DTLS" -gt 0 && is_dtls "$MODE"; then
                        continue;
                    fi

                    # OpenSSL <1.0.2 doesn't support DTLS 1.2. Check if OpenSSL
                    # supports $O_MODE from the s_server help. (The s_client
                    # help isn't accurate as of 1.0.2g: it supports DTLS 1.2
                    # but doesn't list it. But the s_server help seems to be
                    # accurate.)
                    if ! $OPENSSL_CMD s_server -help 2>&1 | grep -q "^ *-$O_MODE "; then
                        continue;
                    fi

                    reset_ciphersuites
                    if [ `minor_ver "$MODE"` -ge 4 ]
                    then
                        M_CIPHERS="$M_CIPHERS               \
                            TLS1-3-AES-128-GCM-SHA256          \
                            TLS1-3-AES-256-GCM-SHA384          \
                            TLS1-3-AES-128-CCM-SHA256          \
                            TLS1-3-AES-128-CCM-8-SHA256        \
                            TLS1-3-CHACHA20-POLY1305-SHA256    \
                            "
                        O_CIPHERS="$O_CIPHERS               \
                            TLS_AES_128_GCM_SHA256          \
                            TLS_AES_256_GCM_SHA384          \
                            TLS_AES_128_CCM_SHA256          \
                            TLS_AES_128_CCM_8_SHA256        \
                            TLS_CHACHA20_POLY1305_SHA256    \
                            "
                    else
                            add_common_ciphersuites
                            add_openssl_ciphersuites
                    fi
                    filter_ciphersuites

                    if [ "X" != "X$M_CIPHERS" ]; then
                        start_server "OpenSSL"
                        for i in $M_CIPHERS; do
                            check_openssl_server_bug $i
                            run_client mbedTLS $i
                        done
                        stop_server
                    fi

                    if [ "X" != "X$O_CIPHERS" ]; then
                        start_server "mbedTLS"
                        for i in $O_CIPHERS; do
                            run_client OpenSSL $i
                        done
                        stop_server
                    fi

                    ;;

                [Gg]nu*)

                    reset_ciphersuites
                    if [ `minor_ver "$MODE"` -ge 4 ]
                    then
                        M_CIPHERS="$M_CIPHERS                  \
                            TLS1-3-AES-128-GCM-SHA256          \
                            TLS1-3-AES-256-GCM-SHA384          \
                            TLS1-3-AES-128-CCM-SHA256          \
                            TLS1-3-AES-128-CCM-8-SHA256        \
                            TLS1-3-CHACHA20-POLY1305-SHA256    \
                            "
                        G_CIPHERS="$G_CIPHERS                \
                            +AES-128-GCM:+SHA256             \
                            +AES-256-GCM:+SHA384             \
                            +AES-128-CCM:+SHA256             \
                            +AES-128-CCM-8:+SHA256           \
                            +CHACHA20-POLY1305:+SHA256       \
                            "
                    else
                            add_common_ciphersuites
                            add_gnutls_ciphersuites
                    fi
                    filter_ciphersuites

                    if [ "X" != "X$M_CIPHERS" ]; then
                        start_server "GnuTLS"
                        for i in $M_CIPHERS; do
                            run_client mbedTLS $i
                        done
                        stop_server
                    fi

                    if [ "X" != "X$G_CIPHERS" ]; then
                        start_server "mbedTLS"
                        for i in $G_CIPHERS; do
                            run_client GnuTLS $i
                        done
                        stop_server
                    fi

                    ;;

                mbed*)

                    reset_ciphersuites
                    if [ `minor_ver "$MODE"` -ge 4 ]
                    then
                        M_CIPHERS="$M_CIPHERS               \
                            TLS1-3-AES-128-GCM-SHA256       \
                            TLS1-3-AES-256-GCM-SHA384       \
                            TLS1-3-AES-128-CCM-SHA256       \
                            TLS1-3-AES-128-CCM-8-SHA256     \
                            TLS1-3-CHACHA20-POLY1305-SHA256 \
                            "
                        O_CIPHERS="$O_CIPHERS               \
                            TLS_AES_128_GCM_SHA256          \
                            TLS_AES_256_GCM_SHA384          \
                            TLS_AES_128_CCM_SHA256          \
                            TLS_AES_128_CCM_8_SHA256        \
                            TLS_CHACHA20_POLY1305_SHA256    \
                            "
                    else
                            add_common_ciphersuites
                            add_openssl_ciphersuites
                            add_gnutls_ciphersuites
                            add_mbedtls_ciphersuites
                    fi
                    filter_ciphersuites

                    if [ "X" != "X$M_CIPHERS" ]; then
                        start_server "mbedTLS"
                        for i in $M_CIPHERS; do
                            run_client mbedTLS $i
                        done
                        stop_server
                    fi

                    ;;

                *)
                    echo "Unknown peer: $PEER" >&2
                    exit 1
                    ;;

                esac

            done
        done
    done
done

echo "------------------------------------------------------------------------"

if [ $FAILED -ne 0 -o $SRVMEM -ne 0 ];
then
    printf "FAILED"
else
    printf "PASSED"
fi

if [ "$MEMCHECK" -gt 0 ]; then
    MEMREPORT=", $SRVMEM server memory errors"
else
    MEMREPORT=""
fi

PASSED=$(( $TESTS - $FAILED ))
echo " ($PASSED / $TESTS tests ($SKIPPED skipped$MEMREPORT))"

FAILED=$(( $FAILED + $SRVMEM ))
exit $FAILED
