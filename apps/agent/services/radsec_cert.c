#include "radsec_cert.h"
#include "core/console.h"
#include "core/result.h"
#include "core/retry.h"
#include "core/script_runner.h"
#include "crypto/cert_audit.h"
#include "crypto/csr.h"
#include "crypto/key_pair.h"
#include "http/http-requests.h"
#include "services/access_token.h"
#include "services/config/config.h"
#include "services/registration.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RADSEC_CA_ENDPOINT "certificate-signing/ca/radsec"
#define RADSEC_SIGN_ENDPOINT "certificate-signing/sign/radsec"

static Console csl = {
    .topic = "radsec cert",
};

bool get_radsec_ca_cert(void *params) {
    if (params == NULL) return false;
    AccessToken *access_token = (AccessToken *)params;

    char url[256];
    snprintf(url, sizeof(url), "%s/%s", config.accounting_api, RADSEC_CA_ENDPOINT);
    console_debug(&csl, "Getting RadSec CA certificate from: %s", url);

    char ca_cert_path[256];
    snprintf(ca_cert_path, sizeof(ca_cert_path), "%s/%s", config.data_path, RADSEC_CA_FILE_NAME);

    HttpDownloadOptions get_ca_options = {
        .url = url,
        .bearer_token = access_token->token,
        .download_path = ca_cert_path,
    };

    HttpResult result = http_download(&get_ca_options);
    if (result.is_error) {
        console_error(&csl, "Failed to download RadSec CA certificate: %s", result.error);
        return false;
    } else {
        console_info(&csl, "RadSec CA certificate downloaded successfully");
    }

    // Verify that the downloaded RadSec CA certificate is valid
    int verify_result = validate_ca_cert(ca_cert_path);
    return verify_result == 1 ? true : false;
}

bool attempt_radsec_ca_cert(AccessToken *access_token) {
    RetryConfig config;
    config.retry_func = get_radsec_ca_cert;
    config.retry_params = access_token;
    config.attempts = 3;
    config.delay_seconds = 30;
    bool result = retry(&config);
    if (result == true) {
        console_debug(&csl, "RadSec CA certificate is valid");
        return true;
    } else {
        console_error(&csl, "Failed to download RadSec CA certificate after %d attempts ... exiting", config.attempts);
        return false;
    }
}

typedef struct {
    AccessToken *access_token;
    Registration *registration;
} RadSecSignParams;

bool generate_and_sign_radsec_cert(void *params) {
    if (params == NULL) return false;
    RadSecSignParams *radsec_params = (RadSecSignParams *)params;

    char key_path[256];
    char csr_path[256];
    char cert_path[256];
    char ca_path[256];
    char backend_url[256];

    snprintf(key_path, sizeof(key_path), "%s/%s", config.data_path, RADSEC_KEY_FILE_NAME);
    snprintf(csr_path, sizeof(csr_path), "%s/%s", config.data_path, RADSEC_CSR_FILE_NAME);
    snprintf(cert_path, sizeof(cert_path), "%s/%s", config.data_path, RADSEC_CERT_FILE_NAME);
    snprintf(ca_path, sizeof(ca_path), "%s/%s", config.data_path, RADSEC_CA_FILE_NAME);
    snprintf(backend_url, sizeof(backend_url), "%s/%s", config.accounting_api, RADSEC_SIGN_ENDPOINT);

    // Print the paths for debugging
    console_debug(&csl, "Key path: %s", key_path);
    console_debug(&csl, "CSR path: %s", csr_path);
    console_debug(&csl, "Cert path: %s", cert_path);
    console_debug(&csl, "CA path: %s", ca_path);
    console_debug(&csl, "Backend URL: %s", backend_url);

    console_debug(&csl, "Checking if the RadSec certificate already exists and is valid ...");
    int initial_verify_result = verify_certificate(cert_path, ca_path);

    console_debug(&csl, "Checking if existing certificate matches key ...");
    int initial_key_cert_match_result = validate_key_cert_match(key_path, cert_path);

    if (initial_verify_result == 1 && initial_key_cert_match_result == 1) {
        console_debug(&csl, "RadSec certificate already exists and is valid. No further action required.");
        return true;
    } else {
        console_debug(&csl, "RadSec certificate does not exist or is invalid. Generating a new one.");
    }

    // Generate private key
    console_debug(&csl, "Generating private key ...");
    EVP_PKEY *pkey = generate_key_pair(Rsa);
    bool save_pkey_result = save_private_key_in_pem(pkey, key_path);
    console_debug(&csl, "Save private key result: %d", save_pkey_result);

    // Generate CSR
    console_debug(&csl, "Generating CSR ...");
    Result csr_result = generate_csr(pkey, csr_path, NULL);
    if (!csr_result.ok) {
        console_error(&csl, "Failed to generate CSR: %s", csr_result.error);
        return false;
    }

    console_debug(&csl, "Sending CSR to backend so it can be signed ...");
    HttpPostOptions post_cert_sign_options = {
        .url = backend_url,
        .upload_file_path = csr_path,
        .bearer_token = radsec_params->access_token->token,
    };

    HttpResult sign_result = http_post(&post_cert_sign_options);
    if (sign_result.is_error) {
        console_error(&csl, "Failed to sign RadSec certificate: %s", sign_result.error);
        return false;
    }

    if (sign_result.response_buffer == NULL) {
        console_error(&csl, "Failed to sign RadSec certificate: no response");
        return false;
    }

    // Save the signed certificate
    FILE *cert_file = fopen(cert_path, "wb");
    if (cert_file == NULL) {
        console_error(&csl, "Failed to open certificate file for writing: %s", cert_path);
        free(sign_result.response_buffer);
        return false;
    }

    console_debug(&csl, "Writing signed certificate to file %s", cert_path);

    fwrite(sign_result.response_buffer, 1, strlen(sign_result.response_buffer), cert_file);
    fclose(cert_file);
    free(sign_result.response_buffer);

    // Check that the written certificate is valid with the CA and with the key
    console_debug(&csl, "Checking if the signed certificate is valid ...");
    int verify_result = verify_certificate(cert_path, ca_path);
    if (verify_result == 1) {
        console_debug(&csl, "RadSec certificate signed and saved successfully");
    } else {
        console_error(&csl, "RadSec certificate is not valid");
        return false;
    }

    console_debug(&csl, "Checking if the certificate matches the key ...");
    int key_cert_match_result = validate_key_cert_match(key_path, cert_path);
    if (key_cert_match_result == 1) {
        console_debug(&csl, "RadSec certificate matches the key");
        return true;
    } else {
        console_error(&csl, "RadSec certificate does not match the key");
        return false;
    }
}

bool attempt_generate_and_sign_radsec(AccessToken *access_token, Registration *registration) {
    RadSecSignParams *radsec_params = (RadSecSignParams *)malloc(sizeof(RadSecSignParams));
    if (radsec_params == NULL) {
        console_error(&csl, "Failed to allocate memory for RadSec certificate generation");
        return false;
    }

    radsec_params->access_token = access_token;
    radsec_params->registration = registration;

    RetryConfig config;
    config.retry_func = generate_and_sign_radsec_cert;
    config.retry_params = radsec_params;
    config.attempts = 3;
    config.delay_seconds = 30;
    bool result = retry(&config);

    free(radsec_params);

    if (result == true) {
        console_info(&csl, "RadSec certificate is ready");
        return true;
    } else {
        console_error(&csl, "Failed to generate and sign RadSec certificate after %d attempts ... exiting",
                      config.attempts);
        return false;
    }
}

// This function restarts radsecproxy; configuration is not distributed here, but through openwisp
// @todo: distribute radsecproxy and configuration through fry-os-services
// @todo: check if radsecproxy is installed with opkg
void install_radsec_cert() {
    if (config.dev_env) {
        console_debug(&csl, "Running in dev environment, skipping RadSec certificate installation");
        return;
    }

    const char *is_installed = run_script("opkg list-installed | grep radsecproxy");
    console_debug(&csl, "Is radsecproxy installed?: %s", is_installed);

    run_script("service radsecproxy stop");
    sleep(5);
    run_script("service radsecproxy start");
}
