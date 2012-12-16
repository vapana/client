/*
 * Copyright (C) 2012 Tobias Brunner
 * Copyright (C) 2005 Jan Hutter, Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <string.h>
#include <stdlib.h>

#include <library.h>
#include <utils/debug.h>
#include <asn1/asn1.h>
#include <asn1/asn1_parser.h>
#include <asn1/oid.h>
#include <crypto/pkcs9.h>
#include <crypto/rngs/rng.h>
#include <crypto/hashers/hasher.h>

#include "scep.h"

static const char *pkiStatus_values[] = { "0", "2", "3" };

static const char *pkiStatus_names[] = {
	"SUCCESS",
	"FAILURE",
	"PENDING",
	"UNKNOWN"
};

static const char *msgType_values[] = { "3", "19", "20", "21", "22" };

static const char *msgType_names[] = {
	"CertRep",
	"PKCSReq",
	"GetCertInitial",
	"GetCert",
	"GetCRL",
	"Unknown"
};

static const char *failInfo_reasons[] = {
	"badAlg - unrecognized or unsupported algorithm identifier",
	"badMessageCheck - integrity check failed",
	"badRequest - transaction not permitted or supported",
	"badTime - Message time field was not sufficiently close to the system time",
	"badCertId - No certificate could be identified matching the provided criteria"
};

const scep_attributes_t empty_scep_attributes = {
	SCEP_Unknown_MSG   , /* msgType */
	SCEP_UNKNOWN       , /* pkiStatus */
	SCEP_unknown_REASON, /* failInfo */
	{ NULL, 0 }        , /* transID */
	{ NULL, 0 }        , /* senderNonce */
	{ NULL, 0 }        , /* recipientNonce */
};

/**
 * Extract X.501 attributes
 */
void extract_attributes(pkcs7_t *pkcs7, scep_attributes_t *attrs)
{
	pkcs9_t *attributes = pkcs7->get_attributes(pkcs7);
	chunk_t attr;

	attr = attributes->get_attribute(attributes, OID_PKI_MESSAGE_TYPE);
	if (attr.ptr)
	{
		scep_msg_t m;

		for (m = SCEP_CertRep_MSG; m < SCEP_Unknown_MSG; m++)
		{
			if (strncmp(msgType_values[m], attr.ptr, attr.len) == 0)
			{
				attrs->msgType = m;
			}
		}
		DBG2(DBG_APP, "messageType:  %s", msgType_names[attrs->msgType]);
	}
	attr = attributes->get_attribute(attributes, OID_PKI_STATUS);
	if (attr.ptr)
	{
		pkiStatus_t s;

		for (s = SCEP_SUCCESS; s < SCEP_UNKNOWN; s++)
		{
			if (strncmp(pkiStatus_values[s], attr.ptr, attr.len) == 0)
			{
				attrs->pkiStatus = s;
			}
		}
		DBG2(DBG_APP, "pkiStatus:    %s", pkiStatus_names[attrs->pkiStatus]);
	}
	attr = attributes->get_attribute(attributes, OID_PKI_FAIL_INFO);
	if (attr.ptr)
	{
		if (attr.len == 1 && *attr.ptr >= '0' && *attr.ptr <= '4')
		{
			attrs->failInfo = (failInfo_t)(*attr.ptr - '0');
		}
		if (attrs->failInfo != SCEP_unknown_REASON)
		{
			DBG1(DBG_APP, "failInfo:     %s", failInfo_reasons[attrs->failInfo]);
		}
	}
	attrs->senderNonce = attributes->get_attribute(attributes,
											OID_PKI_SENDER_NONCE);
	attrs->recipientNonce = attributes->get_attribute(attributes,
											OID_PKI_RECIPIENT_NONCE);
	attrs->transID = attributes->get_attribute(attributes,
											OID_PKI_TRANS_ID);
}

/**
 * Generates a unique fingerprint of the pkcs10 request
 * by computing an MD5 hash over it
 */
chunk_t scep_generate_pkcs10_fingerprint(chunk_t pkcs10)
{
	chunk_t digest = chunk_alloca(HASH_SIZE_MD5);
	hasher_t *hasher;

	hasher = lib->crypto->create_hasher(lib->crypto, HASH_MD5);
	if (!hasher || !hasher->get_hash(hasher, pkcs10, digest.ptr))
	{
		DESTROY_IF(hasher);
		return chunk_empty;
	}
	hasher->destroy(hasher);

	return chunk_to_hex(digest, NULL, FALSE);
}

/**
 * Generate a transaction id as the MD5 hash of an public key
 * the transaction id is also used as a unique serial number
 */
void scep_generate_transaction_id(public_key_t *key, chunk_t *transID,
								  chunk_t *serialNumber)
{
	chunk_t digest = chunk_alloca(HASH_SIZE_MD5);
	chunk_t keyEncoding = chunk_empty, keyInfo;
	hasher_t *hasher;
	bool msb_set;
	u_char *pos;

	key->get_encoding(key, PUBKEY_ASN1_DER, &keyEncoding);

	keyInfo = asn1_wrap(ASN1_SEQUENCE, "mm",
						asn1_algorithmIdentifier(OID_RSA_ENCRYPTION),
						asn1_bitstring("m", keyEncoding));

	hasher = lib->crypto->create_hasher(lib->crypto, HASH_MD5);
	if (!hasher || !hasher->get_hash(hasher, keyInfo, digest.ptr))
	{
		memset(digest.ptr, 0, digest.len);
	}
	DESTROY_IF(hasher);
	free(keyInfo.ptr);

	/* is the most significant bit of the digest set? */
	msb_set = (*digest.ptr & 0x80) == 0x80;

	/* allocate space for the serialNumber */
	serialNumber->len = msb_set + digest.len;
	serialNumber->ptr = malloc(serialNumber->len);

	/* the serial number as the two's complement of the digest */
	pos = serialNumber->ptr;
	if (msb_set)
	{
		*pos++ = 0x00;
	}
	memcpy(pos, digest.ptr, digest.len);

	/* the transaction id is the serial number in hex format */
	*transID = chunk_to_hex(digest, NULL, TRUE);
}

/**
 * Adds a senderNonce attribute to the given pkcs9 attribute list
 */
static bool add_senderNonce_attribute(pkcs9_t *pkcs9)
{
	const size_t nonce_len = 16;
	u_char nonce_buf[nonce_len];
	chunk_t senderNonce = { nonce_buf, nonce_len };
	rng_t *rng;

	rng = lib->crypto->create_rng(lib->crypto, RNG_WEAK);
	if (!rng || !rng->get_bytes(rng, nonce_len, nonce_buf))
	{
		DESTROY_IF(rng);
		return FALSE;
	}
	rng->destroy(rng);

	pkcs9->set_attribute(pkcs9, OID_PKI_SENDER_NONCE, senderNonce);
	return TRUE;
}

/**
 * Builds a pkcs7 enveloped and signed scep request
 */
chunk_t scep_build_request(chunk_t data, chunk_t transID, scep_msg_t msg,
					certificate_t *enc_cert, encryption_algorithm_t enc_alg,
					size_t key_size, certificate_t *signer_cert,
					hash_algorithm_t digest_alg, private_key_t *private_key)
{
	chunk_t request, msgType = {
		(u_char*)msgType_values[msg],
		strlen(msgType_values[msg]),
	};
	pkcs7_t *pkcs7;
	pkcs9_t *pkcs9;

	pkcs7 = pkcs7_create_from_data(data);
	if (!pkcs7->build_envelopedData(pkcs7, enc_cert, enc_alg, key_size))
	{
		pkcs7->destroy(pkcs7);
		return chunk_empty;
	}

	pkcs9 = pkcs9_create();
	pkcs9->set_attribute(pkcs9, OID_PKI_TRANS_ID, transID);
	pkcs9->set_attribute(pkcs9, OID_PKI_MESSAGE_TYPE, msgType);
	if (!add_senderNonce_attribute(pkcs9))
	{
		pkcs9->destroy(pkcs9);
		pkcs7->destroy(pkcs7);
		return chunk_empty;
	}

	pkcs7->set_attributes(pkcs7, pkcs9);
	pkcs7->set_certificate(pkcs7, signer_cert->get_ref(signer_cert));
	if (!pkcs7->build_signedData(pkcs7, private_key, digest_alg))
	{
		pkcs7->destroy(pkcs7);
		return chunk_empty;
	}
	request = pkcs7->get_contentInfo(pkcs7);
	pkcs7->destroy(pkcs7);
	return request;
}

/**
 * Converts a binary request to base64 with 64 characters per line
 * newline and '+' characters are escaped by %0A and %2B, respectively
 */
static char* escape_http_request(chunk_t req)
{
	char *escaped_req = NULL;
	char *p1, *p2;
	int lines = 0;
	int plus  = 0;
	int n     = 0;

	/* compute and allocate the size of the base64-encoded request */
	int len = 1 + 4 * ((req.len + 2) / 3);
	char *encoded_req = malloc(len);

	/* do the base64 conversion */
	chunk_t base64 = chunk_to_base64(req, encoded_req);
	len = base64.len + 1;

	/* compute newline characters to be inserted every 64 characters */
	lines = (len - 2) / 64;

	/* count number of + characters to be escaped */
	p1 = encoded_req;
	while (*p1 != '\0')
	{
		if (*p1++ == '+')
		{
			plus++;
		}
	}

	escaped_req = malloc(len + 3 * (lines + plus));

	/* escape special characters in the request */
	p1 = encoded_req;
	p2 = escaped_req;
	while (*p1 != '\0')
	{
		if (n == 64)
		{
			memcpy(p2, "%0A", 3);
			p2 += 3;
			n = 0;
		}
		if (*p1 == '+')
		{
			memcpy(p2, "%2B", 3);
			p2 += 3;
		}
		else
		{
			*p2++ = *p1;
		}
		p1++;
		n++;
	}
	*p2 = '\0';
	free(encoded_req);
	return escaped_req;
}

/**
 * Send a SCEP request via HTTP and wait for a response
 */
bool scep_http_request(const char *url, chunk_t msg, scep_op_t op,
					   bool http_get_request, chunk_t *response)
{
	int len;
	status_t status;
	char *complete_url = NULL;

	/* initialize response */
	*response = chunk_empty;

	DBG2(DBG_APP, "sending scep request to '%s'", url);

	if (op == SCEP_PKI_OPERATION)
	{
		const char operation[] = "PKIOperation";

		if (http_get_request)
		{
			char *escaped_req = escape_http_request(msg);

			/* form complete url */
			len = strlen(url) + 20 + strlen(operation) + strlen(escaped_req) + 1;
			complete_url = malloc(len);
			snprintf(complete_url, len, "%s?operation=%s&message=%s"
					, url, operation, escaped_req);
			free(escaped_req);

			status = lib->fetcher->fetch(lib->fetcher, complete_url, response,
										 FETCH_HTTP_VERSION_1_0,
										 FETCH_REQUEST_HEADER, "Pragma:",
										 FETCH_REQUEST_HEADER, "Host:",
										 FETCH_REQUEST_HEADER, "Accept:",
										 FETCH_END);
		}
		else /* HTTP_POST */
		{
			/* form complete url */
			len = strlen(url) + 11 + strlen(operation) + 1;
			complete_url = malloc(len);
			snprintf(complete_url, len, "%s?operation=%s", url, operation);

			status = lib->fetcher->fetch(lib->fetcher, complete_url, response,
										 FETCH_HTTP_VERSION_1_0,
										 FETCH_REQUEST_DATA, msg,
										 FETCH_REQUEST_TYPE, "",
										 FETCH_REQUEST_HEADER, "Expect:",
										 FETCH_END);
		}
	}
	else  /* SCEP_GET_CA_CERT */
	{
		const char operation[] = "GetCACert";
		int i;

		/* escape spaces, TODO: complete URL escape */
		for (i = 0; i < msg.len; i++)
		{
			if (msg.ptr[i] == ' ')
			{
				msg.ptr[i] = '+';
			}
		}

		/* form complete url */
		len = strlen(url) + 32 + strlen(operation) + msg.len + 1;
		complete_url = malloc(len);
		snprintf(complete_url, len, "%s?operation=%s&message=%.*s",
				 url, operation, (int)msg.len, msg.ptr);

		status = lib->fetcher->fetch(lib->fetcher, complete_url, response,
									 FETCH_HTTP_VERSION_1_0,
									 FETCH_END);
	}

	free(complete_url);
	return (status == SUCCESS);
}

err_t scep_parse_response(chunk_t response, chunk_t transID, pkcs7_t **data,
						  scep_attributes_t *attrs, certificate_t *signer_cert)
{
	pkcs7_t *pkcs7;

	pkcs7 = pkcs7_create_from_chunk(response, 0);
	if (!pkcs7 || !pkcs7->parse_signedData(pkcs7, signer_cert))
	{
		DESTROY_IF(pkcs7);
		return "error parsing the scep response";
	}
	extract_attributes(pkcs7, attrs);
	if (!chunk_equals(transID, attrs->transID))
	{
		pkcs7->destroy(pkcs7);
		return "transaction ID of scep response does not match";
	}
	*data = pkcs7;
	return NULL;
}
