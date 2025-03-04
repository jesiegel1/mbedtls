/*
 *  Message Processing Stack, Record Transformation Mechanisms
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

#include "mps_transform.h"

/* TODO: Use dummy default functions. */
mbedtls_mps_transform_free_t          *mbedtls_mps_transform_free    = NULL;
mbedtls_mps_transform_decrypt_t       *mbedtls_mps_transform_decrypt = NULL;
mbedtls_mps_transform_encrypt_t       *mbedtls_mps_transform_encrypt = NULL;
mbedtls_mps_transform_get_expansion_t *mbedtls_mps_transform_get_expansion = NULL;
