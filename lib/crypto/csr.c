#include "csr.h"
#include "result.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

static const unsigned char DEFAULT_COUNTRY[] = "US";
static const unsigned char DEFAULT_STATE[] = "Florida";
static const unsigned char DEFAULT_LOCALITY[] = "Boca Raton";
static const unsigned char DEFAULT_ORGANIZATION[] = "Fry Networks";
static const unsigned char DEFAULT_ORGANIZATIONAL_UNIT[] = "Engineering - Firmware";
static const unsigned char DEFAULT_COMMON_NAME[] = "Test Cert fry.network";

Result generate_csr(EVP_PKEY *pkey, const char *csr_filepath, CSRInfo *info) {
    X509_REQ *req = NULL;
    X509_NAME *name = NULL;
    BIO *bio = NULL;

    if (!pkey || !csr_filepath) {
        return error(1, "pkey and csr_filepath must not be NULL");
    }

    // Create a new X509_REQ object
    req = X509_REQ_new();
    if (!req) {
        return error(2, "Failed to create X509_REQ object");
    }

    // Set the public key for the request
    if (X509_REQ_set_pubkey(req, pkey) != 1) {
        X509_REQ_free(req);
        return error(3, "Failed to set public key in X509_REQ");
    }

    // Create a new X509_NAME object
    name = X509_NAME_new();
    if (!name) {
        X509_REQ_free(req);
        return error(4, "Failed to create X509_NAME object");
    }

    // Use provided info or default values
    const unsigned char *country = (info && info->country) ? info->country : DEFAULT_COUNTRY;
    const unsigned char *state = (info && info->state) ? info->state : DEFAULT_STATE;
    const unsigned char *locality = (info && info->locality) ? info->locality : DEFAULT_LOCALITY;
    const unsigned char *organization = (info && info->organization) ? info->organization : DEFAULT_ORGANIZATION;
    const unsigned char *organizational_unit =
        (info && info->organizational_unit) ? info->organizational_unit : DEFAULT_ORGANIZATIONAL_UNIT;
    const unsigned char *common_name = (info && info->common_name) ? info->common_name : DEFAULT_COMMON_NAME;

    // Add entries to the subject name
    if (X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, country, -1, -1, 0) != 1) {
        X509_NAME_free(name);
        X509_REQ_free(req);
        return error(5, "Failed to add country to X509_NAME");
    }

    if (X509_NAME_add_entry_by_txt(name, "ST", MBSTRING_ASC, state, -1, -1, 0) != 1) {
        X509_NAME_free(name);
        X509_REQ_free(req);
        return error(6, "Failed to add state to X509_NAME");
    }

    if (X509_NAME_add_entry_by_txt(name, "L", MBSTRING_ASC, locality, -1, -1, 0) != 1) {
        X509_NAME_free(name);
        X509_REQ_free(req);
        return error(7, "Failed to add locality to X509_NAME");
    }

    if (X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, organization, -1, -1, 0) != 1) {
        X509_NAME_free(name);
        X509_REQ_free(req);
        return error(8, "Failed to add organization to X509_NAME");
    }

    if (X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC, organizational_unit, -1, -1, 0) != 1) {
        X509_NAME_free(name);
        X509_REQ_free(req);
        return error(9, "Failed to add organizational unit to X509_NAME");
    }

    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, common_name, -1, -1, 0) != 1) {
        X509_NAME_free(name);
        X509_REQ_free(req);
        return error(10, "Failed to add common name to X509_NAME");
    }

    // Set the subject name in the request
    if (X509_REQ_set_subject_name(req, name) != 1) {
        X509_NAME_free(name);
        X509_REQ_free(req);
        return error(11, "Failed to set subject name in X509_REQ");
    }

    // Sign the request with the private key
    if (X509_REQ_sign(req, pkey, EVP_sha256()) <= 0) {
        X509_NAME_free(name);
        X509_REQ_free(req);
        return error(12, "Failed to sign X509_REQ");
    }

    // Write the CSR to a file
    bio = BIO_new_file(csr_filepath, "w");
    if (!bio) {
        X509_NAME_free(name);
        X509_REQ_free(req);
        return error(13, "Failed to open CSR file for writing");
    }

    if (PEM_write_bio_X509_REQ(bio, req) != 1) {
        BIO_free(bio);
        X509_NAME_free(name);
        X509_REQ_free(req);
        return error(14, "Failed to write CSR to file");
    }

    // Cleanup
    BIO_free(bio);
    X509_NAME_free(name);
    X509_REQ_free(req);

    // Return success with no data
    return ok(NULL);
}
