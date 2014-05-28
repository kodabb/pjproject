/* $Id$ */
/*
 * Copyright (C) 2009-2011 Teluu Inc. (http://www.teluu.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <pj/ssl_sock.h>
#include <pj/activesock.h>
#include <pj/compat/socket.h>
#include <pj/assert.h>
#include <pj/errno.h>
#include <pj/list.h>
#include <pj/lock.h>
#include <pj/log.h>
#include <pj/math.h>
#include <pj/os.h>
#include <pj/pool.h>
#include <pj/string.h>
#include <pj/timer.h>
#include <pj/file_io.h>


#define LOG_LEVEL 1
/* Only build when PJ_HAS_SSL_SOCK is enabled */
#if defined(PJ_HAS_SSL_SOCK) && PJ_HAS_SSL_SOCK!=0

#define THIS_FILE               "ssl_sock_ossl.c"

/* Workaround for ticket #985 */
#define DELAYED_CLOSE_TIMEOUT   200

/* Maximum ciphers */
#define MAX_CIPHERS             100

/*
 * Include OpenSSL headers
 */
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <gnutls/abstract.h>

#ifdef _MSC_VER
#  pragma comment( lib, "libeay32")
#  pragma comment( lib, "ssleay32")
#endif


/*
 * SSL/TLS state enumeration.
 */
enum ssl_state {
    SSL_STATE_NULL,
    SSL_STATE_HANDSHAKING,
    SSL_STATE_ESTABLISHED
};

/*
 * Internal timer types.
 */
enum timer_id
{
    TIMER_NONE,
    TIMER_HANDSHAKE_TIMEOUT,
    TIMER_CLOSE
};

/*
 * Structure of SSL socket read buffer.
 */
typedef struct read_data_t
{
    void                 *data;
    pj_size_t             len;
} read_data_t;

/*
 * Get the offset of pointer to read-buffer of SSL socket from read-buffer
 * of active socket. Note that both SSL socket and active socket employ
 * different but correlated read-buffers (as much as async_cnt for each),
 * and to make it easier/faster to find corresponding SSL socket's read-buffer
 * from known active socket's read-buffer, the pointer of corresponding
 * SSL socket's read-buffer is stored right after the end of active socket's
 * read-buffer.
 */
#define OFFSET_OF_READ_DATA_PTR(ssock, asock_rbuf) \
                                        (read_data_t**) \
                                        ((pj_int8_t*)(asock_rbuf) + \
                                        ssock->param.read_buffer_size)

/*
 * Structure of SSL socket write data.
 */
typedef struct write_data_t {
    PJ_DECL_LIST_MEMBER(struct write_data_t);
    pj_ioqueue_op_key_t  key;
    pj_size_t            record_len;
    pj_ioqueue_op_key_t *app_key;
    pj_size_t            plain_data_len;
    pj_size_t            data_len;
    unsigned             flags;
    union {
        char             content[1];
        const char      *ptr;
    } data;
} write_data_t;

/*
 * Structure of SSL socket write buffer (circular buffer).
 */
typedef struct send_buf_t {
    char                *buf;
    pj_size_t            max_len;
    char                *start;
    pj_size_t            len;
} send_buf_t;

/*
 * Secure socket structure definition.
 */
struct pj_ssl_sock_t
{
    pj_pool_t            *pool;
    pj_ssl_sock_t        *parent;
    pj_ssl_sock_param     param;
    pj_ssl_cert_t        *cert;

    pj_ssl_cert_info      local_cert_info;
    pj_ssl_cert_info      remote_cert_info;

    pj_bool_t             is_server;
    enum ssl_state        ssl_state;
    pj_ioqueue_op_key_t   handshake_op_key;
    pj_timer_entry        timer;
    pj_status_t           verify_status;

    unsigned long         last_err;

    pj_sock_t             sock;
    pj_activesock_t      *asock;

    pj_sockaddr           local_addr;
    pj_sockaddr           rem_addr;
    int                   addr_len;

    pj_bool_t             read_started;
    pj_size_t             read_size;
    pj_uint32_t           read_flags;
    void                **asock_rbuf;
    read_data_t          *ssock_rbuf;

    write_data_t          write_pending;/* list of pending write to OpenSSL */
    write_data_t          write_pending_empty; /* cache for write_pending   */
    pj_bool_t             flushing_write_pend; /* flag of flushing is ongoing*/
    send_buf_t            send_buf;
    write_data_t          send_pending; /* list of pending write to network */
    pj_lock_t            *write_mutex;  /* protect write BIO and send_buf   */

    SSL_CTX              *ossl_ctx;
    SSL                  *ossl_ssl;
    BIO                  *ossl_rbio;
    BIO                  *ossl_wbio;
    gnutls_session_t      session;
    gnutls_certificate_credentials_t xcred;
    void                 *read_buf;
    int read_buflen;

    int                   tls_init_count; /* library initialization counter */
};


/*
 * Certificate/credential structure definition.
 */
struct pj_ssl_cert_t
{
    pj_str_t CA_file;
    pj_str_t cert_file;
    pj_str_t privkey_file;
    pj_str_t privkey_pass;
};


//static write_data_t* alloc_send_data(pj_ssl_sock_t *ssock, pj_size_t len);
static void free_send_data(pj_ssl_sock_t *ssock, write_data_t *wdata);
static pj_status_t flush_delayed_send(pj_ssl_sock_t *ssock);

/*
 *******************************************************************
 * Static/internal functions.
 *******************************************************************
 */

/**
 * Mapping from OpenSSL error codes to pjlib error space.
 */

#define PJ_SSL_ERRNO_START              (PJ_ERRNO_START_USER + \
                                         PJ_ERRNO_SPACE_SIZE*6)

#define PJ_SSL_ERRNO_SPACE_SIZE         PJ_ERRNO_SPACE_SIZE

/* Expected maximum value of reason component in OpenSSL error code */
#define MAX_OSSL_ERR_REASON             1200

static pj_status_t STATUS_FROM_SSL_ERR(pj_ssl_sock_t *ssock,
                                       unsigned long err)
{
    pj_status_t status;

    /* General SSL error, dig more from OpenSSL error queue */
    if (err == SSL_ERROR_SSL)
        err = ERR_get_error();

    /* OpenSSL error range is much wider than PJLIB errno space, so
     * if it exceeds the space, only the error reason will be kept.
     * Note that the last native error will be kept as is and can be
     * retrieved via SSL socket info.
     */
    status = ERR_GET_LIB(err)*MAX_OSSL_ERR_REASON + ERR_GET_REASON(err);
    if (status > PJ_SSL_ERRNO_SPACE_SIZE)
        status = ERR_GET_REASON(err);

    status += PJ_SSL_ERRNO_START;
    ssock->last_err = err;
    return status;
}

static pj_status_t GET_SSL_STATUS(pj_ssl_sock_t *ssock)
{
    return STATUS_FROM_SSL_ERR(ssock, ERR_get_error());
}


/*
 * Get error string of OpenSSL.
 */
static pj_str_t ssl_strerror(pj_status_t status,
                             char *buf, pj_size_t bufsize)
{
    pj_str_t errstr;
    unsigned long ssl_err = status;

    if (ssl_err) {
        unsigned long l, r;
        ssl_err -= PJ_SSL_ERRNO_START;
        l = ssl_err / MAX_OSSL_ERR_REASON;
        r = ssl_err % MAX_OSSL_ERR_REASON;
        ssl_err = ERR_PACK(l, 0, r);
    }

#if defined(PJ_HAS_ERROR_STRING) && (PJ_HAS_ERROR_STRING != 0)

    {
        const char *tmp = NULL;
        tmp = ERR_reason_error_string(ssl_err);
        if (tmp) {
            pj_ansi_strncpy(buf, tmp, bufsize);
            errstr = pj_str(buf);
            return errstr;
        }
    }

#endif  /* PJ_HAS_ERROR_STRING */

    errstr.ptr = buf;
    errstr.slen = pj_ansi_snprintf(buf, bufsize,
                                   "Unknown OpenSSL error %lu",
                                   ssl_err);
    if (errstr.slen < 1 || errstr.slen >= (int)bufsize)
        errstr.slen = bufsize - 1;
    return errstr;
}

/* OpenSSL available ciphers */
static unsigned openssl_cipher_num;
static struct openssl_ciphers_t {
    pj_ssl_cipher    id;
    const char      *name;
} openssl_ciphers[MAX_CIPHERS];

static void print_logs(int level, const char* msg)
{
    fprintf(stderr, "GnuTLS [%d]: %s", level, msg);
}

/* Initialize OpenSSL */
static pj_status_t init_openssl(void)
{


    /* Register error subsystem */
    /*status = pj_register_strerror(PJ_SSL_ERRNO_START,
                                  PJ_SSL_ERRNO_SPACE_SIZE,
                                  &ssl_strerror);*/
    //pj_assert(status == PJ_SUCCESS);

    /* Init OpenSSL lib */
    gnutls_global_init();
    gnutls_global_set_log_level(LOG_LEVEL);
    gnutls_global_set_log_function(print_logs);

    /* Init available ciphers */
    if (openssl_cipher_num == 0) {
        unsigned i;

        for (i = 0; ; i++) {
            unsigned char id[2];
            const char *suite = gnutls_cipher_suite_info(i, (unsigned char *)id,
                                                         NULL, NULL, NULL, NULL);
            openssl_ciphers[i].id = 0;
            if (suite != NULL && i < PJ_ARRAY_SIZE(openssl_ciphers)) {
                openssl_ciphers[i].id = (pj_ssl_cipher)
                    (pj_uint32_t) ((id[0] << 8) | id[1]);
                openssl_ciphers[i].name = suite;
            } else
                break;
        }

        openssl_cipher_num = i;
    }

    /* Create OpenSSL application data index for SSL socket */
    //sslsock_idx = SSL_get_ex_new_index(0, "SSL socket", NULL, NULL, NULL);

    return PJ_SUCCESS;
}


/* Shutdown OpenSSL */
static void shutdown_openssl(void)
{
    gnutls_global_deinit();
}


/* SSL password callback. */
static int verify_callback(gnutls_session_t session)
{
    pj_ssl_sock_t *ssock;
    int ret;
    unsigned int status;

    /* Get SSL instance */
    /* Get SSL socket instance */
    ssock = (pj_ssl_sock_t *)gnutls_session_get_ptr(session);
    pj_assert(ssock);

    /* Support only x509 format */
    ret = gnutls_certificate_type_get(session) != GNUTLS_CRT_X509;
    if (ret < 0) {
        ssock->verify_status |= PJ_SSL_CERT_EINVALID_FORMAT;
        return GNUTLS_E_CERTIFICATE_ERROR;
    }

    /* Store verification status */
    ret = gnutls_certificate_verify_peers2(session, &status);
    if (ret < 0) {
        ssock->verify_status |= PJ_SSL_CERT_EUNKNOWN;
        return GNUTLS_E_CERTIFICATE_ERROR;
    }
    if (status & GNUTLS_CERT_INVALID) {
        if (status & GNUTLS_CERT_SIGNER_NOT_FOUND)
            ssock->verify_status |= PJ_SSL_CERT_EISSUER_NOT_FOUND;
        else if (status & GNUTLS_CERT_EXPIRED ||
                 status & GNUTLS_CERT_NOT_ACTIVATED)
            ssock->verify_status |= PJ_SSL_CERT_EVALIDITY_PERIOD;
        else if (status & GNUTLS_CERT_SIGNER_NOT_CA ||
                 status & GNUTLS_CERT_INSECURE_ALGORITHM)
            ssock->verify_status |= PJ_SSL_CERT_EUNTRUSTED;
#if GNUTLS_VERSION_MAJOR >= 3
        else if (status & GNUTLS_CERT_UNEXPECTED_OWNER ||
                 status & GNUTLS_CERT_MISMATCH)
            ssock->verify_status |= PJ_SSL_CERT_EISSUER_MISMATCH;
#endif
        else if (status & GNUTLS_CERT_REVOKED)
            ssock->verify_status |= PJ_SSL_CERT_EREVOKED;
        else
            ssock->verify_status |= PJ_SSL_CERT_EUNKNOWN;

        return GNUTLS_E_CERTIFICATE_ERROR;
    }

    /* When verification is not requested just return ok here, however
     * application can still get the verification status.  */
    if (ssock->param.verify_peer) {
        gnutls_x509_crt_t cert;
        unsigned int cert_list_size;
        const gnutls_datum_t *cert_list;

        if (gnutls_x509_crt_init(&cert) < 0) {
            fprintf(stderr, "Error in initialization\n");
            goto fail;
        }

        cert_list = gnutls_certificate_get_peers(session, &cert_list_size);
        if (cert_list == NULL) {
            fprintf(stderr, "No certificate found!\n");
            goto fail;
        }

        /* TODO verify whole chain perhaps? */
        ret = gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER);
        if (ret < 0)
            ret = gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_PEM);
        if (ret < 0) {
            fprintf(stderr, "Error parsing certificate %s!\n", gnutls_strerror(ret));
            ssock->verify_status |= PJ_SSL_CERT_EINVALID_FORMAT;
            return GNUTLS_E_CERTIFICATE_ERROR;
        }
        ret = gnutls_x509_crt_check_hostname(cert, ssock->param.server_name.ptr);
        if (ret < 0) {
            fprintf(stderr, "The certificate's owner does not match hostname '%s'. %s!\n",
                    ssock->param.server_name.ptr, gnutls_strerror(ret));
            goto fail;
        }
        gnutls_x509_crt_deinit(cert);

        /* notify gnutls to continue handshake normally */
        return 0;

fail:
        ssock->verify_status |= PJ_SSL_CERT_EUNKNOWN;
        return GNUTLS_E_CERTIFICATE_ERROR;
    }

    return 0;
}


static ssize_t data_push(gnutls_transport_ptr_t ptr, const void *data, size_t len)
{
    pj_ssl_sock_t *ssock = (pj_ssl_sock_t *)ptr;
    pj_sock_send(ssock->sock, data, (pj_ssize_t *)&len, 0);
    return len;
}

// GnuTLS calls this function to receive data from the transport layer. We set
// this callback with gnutls_transport_set_pull_function(). It should act like
// recv() (see the manual for specifics).
static ssize_t data_pull(gnutls_transport_ptr_t ptr, void *data, size_t len)
{
    pj_ssl_sock_t *ssock = (pj_ssl_sock_t *)ptr;
    pj_status_t status;
    size_t orig_len = len;
    if (ssock->read_buf) {
        memcpy(data, ssock->read_buf, len);
        ssock->read_buf += len;
        ssock->read_buflen -= len;
        return ssock->read_buflen < 0 ? -1 : len;
    } else {
        status = pj_sock_recv(ssock->sock, data, (pj_ssize_t *)&len, 0);
        return status == PJ_SUCCESS ? len : -1;
    }
}

#if 0
static pj_status_t tls_load_file(pj_pool_t *pool, const char *path,
                                 gnutls_datum_t *dt)
{
    pj_oshandle_t fd;
    ssize_t bytes_read;
    size_t file_size;
    pj_status_t status;
    unsigned int out_len = 0;

    status = pj_file_open(pool, path, PJ_O_RDONLY, &fd);
    if (status != PJ_SUCCESS) {
        status = PJ_EINVAL;
        goto out;
    }

    pj_file_setpos(fd, 0, PJ_SEEK_END);
    pj_file_getpos(fd, (pj_off_t *)&file_size);
    pj_file_setpos(fd, 0, PJ_SEEK_SET);

    dt->data = pj_pool_calloc(pool, file_size, sizeof(uint8_t));
    if (!dt->data) {
        fprintf(stderr, "Not enough memory to read file '%s'.", path);
        status = PJ_ENOMEM;
        goto out;
    }

    do {
        bytes_read = file_size - out_len;
        status = pj_file_read(fd, &(dt->data[out_len]), &bytes_read);
        if (bytes_read < 0) {
            fprintf(stderr, "Failed to read file '%s'.", path);
            status = PJ_EINVAL;
            goto out;
        }
        out_len += bytes_read;
    } while ((bytes_read != 0) && (status != PJ_SUCCESS));

    dt->size = out_len;
    status = PJ_SUCCESS;
out:
    pj_file_close(fd);
    return status;
}
#endif

static pj_status_t tls_str_append_once(pj_str_t *dst, pj_str_t *src)
{
    if (pj_strstr(dst, src) == NULL) {
        /* Check buffer size */
        if (dst->slen + src->slen + 3 > 1024) {
            pj_assert(!"Insufficient temporary buffer for cipher");
            return PJ_ETOOMANY;
        }
        pj_strcat2(dst, ":+");
        pj_strcat(dst, src);
    }
    return PJ_SUCCESS;
}

/* Generate cipher list with user preference order in OpenSSL format */
static pj_status_t tls_priorities_set(pj_ssl_sock_t *ssock)
{
    char buf[1024];
    pj_str_t cipher_list;
    pj_str_t compression = pj_str("COMP-NULL");
    pj_str_t server = pj_str(":%SERVER_PRECEDENCE");
    int i, j, status;
    const char *priority;
    const char *err;

    pj_strset(&cipher_list, buf, 0);

    /* default choice */
    if (ssock->param.ciphers_num == 0) {
        switch (ssock->param.proto) {
        case PJ_SSL_SOCK_PROTO_DEFAULT:
        case PJ_SSL_SOCK_PROTO_TLS1:
            priority = "SECURE256:-VERS-SSL3.0";
            break;
        case PJ_SSL_SOCK_PROTO_SSL3:
            priority = "SECURE256";
            break;
        case PJ_SSL_SOCK_PROTO_SSL23:
            priority = "NORMAL";
            break;
        default:
            return PJ_ENOTSUP;
        }
    } else
        priority = "NONE";

    pj_strcat2(&cipher_list, priority);
    for (i = 0; i < ssock->param.ciphers_num; i++) {
        for (j = 0; ; j++) {
            pj_ssl_cipher c;
            const char *suite;
            unsigned char id[2];
            gnutls_protocol_t proto;
            gnutls_kx_algorithm_t kx;
            gnutls_mac_algorithm_t mac;
            gnutls_cipher_algorithm_t algo;

            suite = gnutls_cipher_suite_info(j, (unsigned char *)id,
                                             &kx, &algo, &mac, &proto);
            if (suite == NULL)
                break;

            c = (pj_ssl_cipher) (pj_uint32_t) ((id[0] << 8) | id[1]);
            if (ssock->param.ciphers[i] == c) {
                char temp[256];
                pj_str_t cipher_entry;

                pj_strset(&cipher_entry, temp, 0);
                pj_strcat2(&cipher_entry, "VERS-");
                pj_strcat2(&cipher_entry, gnutls_protocol_get_name(proto));
                status = tls_str_append_once(&cipher_list, &cipher_entry);
                if (status != PJ_SUCCESS)
                    return status;

                pj_strset(&cipher_entry, temp, 0);
                pj_strcat2(&cipher_entry, gnutls_cipher_get_name(algo));
                tls_str_append_once(&cipher_list, &cipher_entry);
                if (status != PJ_SUCCESS)
                    return status;

                pj_strset(&cipher_entry, temp, 0);
                pj_strcat2(&cipher_entry, gnutls_mac_get_name(mac));
                tls_str_append_once(&cipher_list, &cipher_entry);
                if (status != PJ_SUCCESS)
                    return status;

                pj_strset(&cipher_entry, temp, 0);
                pj_strcat2(&cipher_entry, gnutls_kx_get_name(kx));
                tls_str_append_once(&cipher_list, &cipher_entry);
                if (status != PJ_SUCCESS)
                    return status;

                break;
            }
        }
    }

    /* disable compression, it's a TLS extension only after all */
    tls_str_append_once(&cipher_list, &compression);

    /* server should be the one deciding which cripto to use */
    if (ssock->is_server) {
       if (cipher_list.slen + server.slen + 1 > sizeof(buf)) {
            pj_assert(!"Insufficient temporary buffer for cipher");
            return PJ_ETOOMANY;
        } else
            pj_strcat(&cipher_list, &server);
    }

    /* end the string */
    cipher_list.ptr[cipher_list.slen] = '\0';

    /* set our priority string */
    status = gnutls_priority_set_direct(ssock->session,
                                        cipher_list.ptr, &err);
    if (status < 0) {
        fprintf(stderr, "tried string: %s\n", cipher_list.ptr);
        if (status == GNUTLS_E_INVALID_REQUEST)
            fprintf(stderr, "Syntax error at: %s\n", err);
        return status;
    }

    return PJ_SUCCESS;
}



/* Create and initialize new SSL context and instance */
static pj_status_t create_ssl(pj_ssl_sock_t *ssock)
{
    pj_ssl_cert_t *cert;
    int ret;
    pj_status_t status;

    pj_assert(ssock);

    cert = ssock->cert;

    /* OpenSSL library initialization counter */
    if (!ssock->tls_init_count) {
        ssock->tls_init_count++;
        init_openssl();
    } else
        return PJ_SUCCESS;

    gnutls_init(&ssock->session, ssock->is_server ? GNUTLS_SERVER : GNUTLS_CLIENT);

    /* Set SSL sock as application data of SSL instance */
    gnutls_transport_set_ptr(ssock->session, (gnutls_transport_ptr_t) (uintptr_t) ssock);
    /* Set our user-data into gnutls session */
    gnutls_session_set_ptr(ssock->session, (gnutls_transport_ptr_t) (uintptr_t) ssock);

    // Set the callback that allows GnuTLS to PUSH data TO the transport layer
    gnutls_transport_set_push_function(ssock->session, data_push);
    // Set the callback that allows GnuTls to PULL data FROM the tranport layer
    gnutls_transport_set_pull_function(ssock->session, data_pull);

    /* Determine SSL method to use */
    status = tls_priorities_set(ssock);
    if (status != PJ_SUCCESS) {
        fprintf(stderr, "Error setting priorities: %s\n", gnutls_strerror(status));
        return status;
    }

    /* Allocate credentials loading root cert, needed for handshaking */
    gnutls_certificate_allocate_credentials(&ssock->xcred);
    gnutls_certificate_set_verify_function(ssock->xcred, verify_callback);
    // TODO load more places
    gnutls_certificate_set_x509_trust_file(ssock->xcred,
                                           "/etc/ssl/certs/ca-certificates.crt",
                                           GNUTLS_X509_FMT_PEM);

    /* Apply credentials */
    if (cert) {
        /* Load CA list if one is specified. */
        if (cert->CA_file.slen) {
            status = gnutls_certificate_set_x509_trust_file(ssock->xcred,
                                                            cert->CA_file.ptr,
                                                            GNUTLS_X509_FMT_PEM);
            if (status < 0)
                status = gnutls_certificate_set_x509_trust_file(ssock->xcred,
                                                                cert->CA_file.ptr,
                                                                GNUTLS_X509_FMT_DER);
            if (status < 0) {
                fprintf(stderr, "Error loading CA list: %s\n", gnutls_strerror(status));
                return PJ_EINVAL;
            }
        }

        /* Load certificate if one is specified */
        if (cert->cert_file.slen) {
            ret = gnutls_certificate_set_x509_key_file2(ssock->xcred,
                                                        cert->cert_file.ptr,
                                                        cert->privkey_file.slen ? cert->privkey_file.ptr
                                                                                : NULL,
                                                        GNUTLS_X509_FMT_PEM,
                                                        (cert->privkey_file.slen &&
                                                        cert->privkey_pass.slen) ? cert->privkey_pass.ptr
                                                                                 : NULL,
                                                        0);
            if (ret != GNUTLS_E_SUCCESS)
                ret = gnutls_certificate_set_x509_key_file2(ssock->xcred,
                                                            cert->cert_file.ptr,
                                                            cert->privkey_file.slen ? cert->privkey_file.ptr
                                                                                    : NULL,
                                                            GNUTLS_X509_FMT_DER,
                                                            (cert->privkey_file.slen &&
                                                            cert->privkey_pass.slen) ? cert->privkey_pass.ptr
                                                                                     : NULL,
                                                            0);
            if (ret != GNUTLS_E_SUCCESS) {
                fprintf(stderr, "Could not import cert/key/pass - %s\n",
                        gnutls_strerror(ret));
                return PJ_EINVAL;
            }
        }
    }



#if 0
    #ifndef SSL_CTRL_SET_ECDH_AUTO
        #define SSL_CTRL_SET_ECDH_AUTO 94
    #endif

    /* SSL_CTX_set_ecdh_auto(ctx, on); requires OpenSSL 1.0.2 which wraps: */
    if (SSL_CTX_ctrl(ctx, SSL_CTRL_SET_ECDH_AUTO, 1, NULL)) {
        PJ_LOG(4,(ssock->pool->obj_name, "SSL ECDH initialized (automatic), "
                  "faster PFS ciphers enabled"));
    } else {
        /* enables AES-128 ciphers, to get AES-256 use NID_secp384r1 */
        ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        if (ecdh != NULL) {
            if (SSL_CTX_set_tmp_ecdh(ctx, ecdh)) {
                PJ_LOG(4,(ssock->pool->obj_name, "SSL ECDH initialized "
                          "(secp256r1), faster PFS cipher-suites enabled"));
            }
            EC_KEY_free(ecdh);
        }
    }
#endif

    /* SSL verification options */
    if (ssock->is_server && ssock->param.require_client_cert)
        gnutls_certificate_server_set_request(ssock->session, GNUTLS_CERT_REQUIRE);


    gnutls_credentials_set(ssock->session, GNUTLS_CRD_CERTIFICATE, ssock->xcred);


#if 0
    /* Setup SSL BIOs */
    ssock->ossl_rbio = BIO_new(BIO_s_mem());
    ssock->ossl_wbio = BIO_new(BIO_s_mem());
    (void)BIO_set_close(ssock->ossl_rbio, BIO_CLOSE);
    (void)BIO_set_close(ssock->ossl_wbio, BIO_CLOSE);
    SSL_set_bio(ssock->ossl_ssl, ssock->ossl_rbio, ssock->ossl_wbio);
#endif

    return PJ_SUCCESS;
}


/* Destroy SSL context and instance */
static void destroy_ssl(pj_ssl_sock_t *ssock)
{
    /* Destroy SSL context */
    if (ssock->xcred) {
        gnutls_certificate_free_credentials(ssock->xcred);
        ssock->xcred = NULL;
    }

    /* Destroy SSL instance */
    if (ssock->session) {
        gnutls_bye(ssock->session, GNUTLS_SHUT_RDWR);
        gnutls_deinit(ssock->session);
        ssock->session = NULL;
    }

    /* Potentially shutdown OpenSSL library if this is the last
     * context exists. */
    if (ssock->tls_init_count) {
        ssock->tls_init_count--;
        shutdown_openssl();
    }
}


/* Reset SSL socket state */
static void reset_ssl_sock_state(pj_ssl_sock_t *ssock)
{
    ssock->ssl_state = SSL_STATE_NULL;

    destroy_ssl(ssock);

    if (ssock->asock) {
        pj_activesock_close(ssock->asock);
        ssock->asock = NULL;
        ssock->sock = PJ_INVALID_SOCKET;
    }
    if (ssock->sock != PJ_INVALID_SOCKET) {
        pj_sock_close(ssock->sock);
        ssock->sock = PJ_INVALID_SOCKET;
    }

    /* Upon error, OpenSSL may leave any error description in the thread
     * error queue, which sometime may cause next call to SSL API returning
     * false error alarm, e.g: in Linux, SSL_CTX_use_certificate_chain_file()
     * returning false error after a handshake error (in different SSL_CTX!).
     * For now, just clear thread error queue here.
     */
    //ERR_clear_error();
}


/* Get Common Name field string from a general name string */
static void get_cn_from_gen_name(const pj_str_t *gen_name, pj_str_t *cn)
{
    pj_str_t CN_sign = {"CN=", 3};
    char *p, *q;

    pj_bzero(cn, sizeof(cn));

    p = pj_strstr(gen_name, &CN_sign);
    if (!p)
        return;

    p += 3; /* shift pointer to value part */
    pj_strset(cn, p, gen_name->slen - (p - gen_name->ptr));
    q = pj_strchr(cn, ',');
    if (q)
        cn->slen = q - p;
}


/* Get certificate info from OpenSSL X509, in case the certificate info
 * hal already populated, this function will check if the contents need
 * to be updated by inspecting the issuer and the serial number.
 */
static void get_cert_info(pj_pool_t *pool, pj_ssl_cert_info *ci, gnutls_x509_crt_t cert)
{
    pj_bool_t update_needed;
    char buf[512] = { 0 };
    size_t bufsize = sizeof(buf);
    pj_uint8_t serial_no[64] = { 0 }; /* should be >= sizeof(ci->serial_no) */
    size_t serialsize = sizeof(serial_no);

    pj_assert(pool && ci && cert);

    /* Get issuer */
    gnutls_x509_crt_get_issuer_dn(cert, buf, &bufsize);

    /* Get serial no */
    gnutls_x509_crt_get_serial(cert, serial_no, &serialsize);

    /* Check if the contents need to be updated. */
    update_needed = pj_strcmp2(&ci->issuer.info, buf) ||
                    pj_memcmp(ci->serial_no, serial_no, serialsize);
    if (!update_needed)
        return;

    /* Update cert info */

    pj_bzero(ci, sizeof(pj_ssl_cert_info));

    /* Version */
    ci->version = gnutls_x509_crt_get_version(cert);

    /* Issuer */
    pj_strdup2(pool, &ci->issuer.info, buf);
    get_cn_from_gen_name(&ci->issuer.info, &ci->issuer.cn);

    /* Serial number */
    pj_memcpy(ci->serial_no, serial_no, sizeof(ci->serial_no));

    /* Subject */
    bufsize = sizeof(buf);
    gnutls_x509_crt_get_dn(cert, buf, &bufsize);
    pj_strdup2(pool, &ci->subject.info, buf);
    get_cn_from_gen_name(&ci->subject.info, &ci->subject.cn);

    /* Validity */
    ci->validity.end.sec = gnutls_x509_crt_get_expiration_time(cert);
    ci->validity.start.sec = gnutls_x509_crt_get_activation_time(cert);
    ci->validity.gmt = 0;

    /* Subject Alternative Name extension */
    size_t len = sizeof(buf);
    int i, ret, seq = 0;

    pj_ssl_cert_name_type type;
    if (ci->version >= 3) {
        char out[256] = { 0 };
        /* Get the number of all alternate names so that we can allocate
         * the correct number of bytes in subj_alt_name */
        while (gnutls_x509_crt_get_subject_alt_name(cert, seq, out, &len, NULL) != GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE)
            seq++;

        ci->subj_alt_name.entry = pj_pool_calloc(pool, seq,
                                                 sizeof(*ci->subj_alt_name.entry));
        for (i = 0; i < seq; i++) {
            len = sizeof(out) - 1;
            ret = gnutls_x509_crt_get_subject_alt_name(cert, i, out, &len, NULL);
            switch (ret) {
            case GNUTLS_SAN_IPADDRESS:
                type = PJ_SSL_CERT_NAME_IP;
                pj_inet_ntop2(len == sizeof(pj_in6_addr) ? pj_AF_INET6() : pj_AF_INET(),
                              out, buf, sizeof(buf));
                break;
            case GNUTLS_SAN_URI:
                type = PJ_SSL_CERT_NAME_URI;
                break;
            case GNUTLS_SAN_RFC822NAME:
                type = PJ_SSL_CERT_NAME_RFC822;
                break;
            case GNUTLS_SAN_DNSNAME:
                type = PJ_SSL_CERT_NAME_DNS;
                break;
            default:
                type = PJ_SSL_CERT_NAME_UNKNOWN;
                break;
            }

            if (len && type != PJ_SSL_CERT_NAME_UNKNOWN) {
                ci->subj_alt_name.entry[ci->subj_alt_name.cnt].type = type;
                pj_strdup2(pool, &ci->subj_alt_name.entry[ci->subj_alt_name.cnt].name,
                           type == PJ_SSL_CERT_NAME_IP ? buf : out);
                ci->subj_alt_name.cnt++;
            }
        }

    /* Check against the commonName if no DNS alt. names were found,
     * as per RFC3280. ????? */
    }
}


/* Update local & remote certificates info. This function should be
 * called after handshake or renegotiation successfully completed.
 */
static void update_certs_info(pj_ssl_sock_t *ssock)
{
    gnutls_x509_crt_t cert = NULL;
    const gnutls_datum_t *us;
    const gnutls_datum_t *certs;
    unsigned int certslen = 0;
    int err;

    pj_assert(ssock->ssl_state == SSL_STATE_ESTABLISHED);

    us = gnutls_certificate_get_ours(ssock->session);
    if (us != NULL) {
        err = gnutls_x509_crt_init(&cert);
        if (err != GNUTLS_E_SUCCESS) {
            fprintf(stderr, "Could not init certificate - %s\n", gnutls_strerror(err));
            goto out;
        }
        err = gnutls_x509_crt_import(cert, us, GNUTLS_X509_FMT_DER);
        if (err != GNUTLS_E_SUCCESS) {
            fprintf(stderr, "Could not read our certificate - %s\n", gnutls_strerror(err));
            goto out;
        }
        get_cert_info(ssock->pool, &ssock->local_cert_info, cert);
        gnutls_x509_crt_deinit(cert);
        cert = NULL;
    } else {
        /* Active local certificate */
        pj_bzero(&ssock->local_cert_info, sizeof(pj_ssl_cert_info));
    }

    /* Active remote certificate */
    certs = gnutls_certificate_get_peers(ssock->session, &certslen);
    if (certs == NULL || certslen == 0) {
        fprintf(stderr, "Could not obtain peer certificate\n");
        goto out;
    }
    err = gnutls_x509_crt_init(&cert);
    if (err != GNUTLS_E_SUCCESS) {
        fprintf(stderr, "Could not init certificate - %s", gnutls_strerror(err));
        goto out;
    }

    /* The peer certificate is the first certificate in the list. */
    err = gnutls_x509_crt_import(cert, certs, GNUTLS_X509_FMT_PEM);
    if (err != GNUTLS_E_SUCCESS)
        err = gnutls_x509_crt_import(cert, certs, GNUTLS_X509_FMT_DER);
    if (err != GNUTLS_E_SUCCESS) {
        fprintf(stderr, "Could not read peer certificate - %s", gnutls_strerror(err));
        goto out;
    }

    get_cert_info(ssock->pool, &ssock->remote_cert_info, cert);

out:
    if (cert)
        gnutls_x509_crt_deinit(cert);
    else
        pj_bzero(&ssock->remote_cert_info, sizeof(pj_ssl_cert_info));

    return;
}


/* When handshake completed:
 * - notify application
 * - if handshake failed, reset SSL state
 * - return PJ_FALSE when SSL socket instance is destroyed by application.
 */
static pj_bool_t on_handshake_complete(pj_ssl_sock_t *ssock,
                                       pj_status_t status)
{
    /* Cancel handshake timer */
    if (ssock->timer.id == TIMER_HANDSHAKE_TIMEOUT) {
        pj_timer_heap_cancel(ssock->param.timer_heap, &ssock->timer);
        ssock->timer.id = TIMER_NONE;
    }

    /* Update certificates info on successful handshake */
    if (status == PJ_SUCCESS)
        update_certs_info(ssock);

    /* Accepting */
    if (ssock->is_server) {
        if (status != PJ_SUCCESS) {
            /* Handshake failed in accepting, destroy our self silently. */

            char errmsg[PJ_ERR_MSG_SIZE];
            char buf[PJ_INET6_ADDRSTRLEN + 10];

            pj_strerror(status, errmsg, sizeof(errmsg));
            PJ_LOG(3,(ssock->pool->obj_name, "Handshake failed in accepting "
                      "%s: %s",
                      pj_sockaddr_print(&ssock->rem_addr, buf, sizeof(buf), 3),
                      errmsg));

            /* Workaround for ticket #985 */
#if (defined(PJ_WIN32) && PJ_WIN32!=0) || (defined(PJ_WIN64) && PJ_WIN64!=0)
            if (ssock->param.timer_heap) {
                pj_time_val interval = {0, DELAYED_CLOSE_TIMEOUT};

                reset_ssl_sock_state(ssock);

                ssock->timer.id = TIMER_CLOSE;
                pj_time_val_normalize(&interval);
                if (pj_timer_heap_schedule(ssock->param.timer_heap,
                                           &ssock->timer, &interval) != 0)
                {
                    ssock->timer.id = TIMER_NONE;
                    pj_ssl_sock_close(ssock);
                }
            } else
#endif  /* PJ_WIN32 */
            {
                pj_ssl_sock_close(ssock);
            }
            return PJ_FALSE;
        }
        /* Notify application the newly accepted SSL socket */
        if (ssock->param.cb.on_accept_complete) {
            pj_bool_t ret;
            ret = (*ssock->param.cb.on_accept_complete)
                      (ssock->parent, ssock, (pj_sockaddr_t*)&ssock->rem_addr,
                       pj_sockaddr_get_len((pj_sockaddr_t*)&ssock->rem_addr));
            if (ret == PJ_FALSE)
                return PJ_FALSE;
        }
    }

    /* Connecting */
    else {
        /* On failure, reset SSL socket state first, as app may try to
         * reconnect in the callback.
         */
        if (status != PJ_SUCCESS) {
            /* Server disconnected us, possibly due to SSL nego failure */
            if (status == PJ_EEOF) {
                unsigned long err;
                err = ERR_get_error();
                if (err != SSL_ERROR_NONE)
                    status = STATUS_FROM_SSL_ERR(ssock, err);
            }
            reset_ssl_sock_state(ssock);
        }
        if (ssock->param.cb.on_connect_complete) {
            pj_bool_t ret;
            ret = (*ssock->param.cb.on_connect_complete)(ssock, status);
            if (ret == PJ_FALSE)
                return PJ_FALSE;
        }
    }

    return PJ_TRUE;
}

#if 0
static write_data_t* alloc_send_data(pj_ssl_sock_t *ssock, pj_size_t len)
{
    send_buf_t *send_buf = &ssock->send_buf;
    pj_size_t avail_len, skipped_len = 0;
    char *reg1, *reg2;
    pj_size_t reg1_len, reg2_len;
    write_data_t *p;

    /* Check buffer availability */
    avail_len = send_buf->max_len - send_buf->len;
    if (avail_len < len)
        return NULL;

    /* If buffer empty, reset start pointer and return it */
    if (send_buf->len == 0) {
        send_buf->start = send_buf->buf;
        send_buf->len   = len;
        p = (write_data_t*)send_buf->start;
        goto init_send_data;
    }

    /* Free space may be wrapped/splitted into two regions, so let's
     * analyze them if any region can hold the write data.
     */
    reg1 = send_buf->start + send_buf->len;
    if (reg1 >= send_buf->buf + send_buf->max_len)
        reg1 -= send_buf->max_len;
    reg1_len = send_buf->max_len - send_buf->len;
    if (reg1 + reg1_len > send_buf->buf + send_buf->max_len) {
        reg1_len = send_buf->buf + send_buf->max_len - reg1;
        reg2 = send_buf->buf;
        reg2_len = send_buf->start - send_buf->buf;
    } else {
        reg2 = NULL;
        reg2_len = 0;
    }

    /* More buffer availability check, note that the write data must be in
     * a contigue buffer.
     */
    avail_len = PJ_MAX(reg1_len, reg2_len);
    if (avail_len < len)
        return NULL;

    /* Get the data slot */
    if (reg1_len >= len) {
        p = (write_data_t*)reg1;
    } else {
        p = (write_data_t*)reg2;
        skipped_len = reg1_len;
    }

    /* Update buffer length */
    send_buf->len += len + skipped_len;

init_send_data:
    /* Init the new send data */
    pj_bzero(p, sizeof(*p));
    pj_list_init(p);
    pj_list_push_back(&ssock->send_pending, p);

    return p;
}
#endif

static void free_send_data(pj_ssl_sock_t *ssock, write_data_t *wdata)
{
    send_buf_t *buf = &ssock->send_buf;
    write_data_t *spl = &ssock->send_pending;

    pj_assert(!pj_list_empty(&ssock->send_pending));

    /* Free slot from the buffer */
    if (spl->next == wdata && spl->prev == wdata) {
        /* This is the only data, reset the buffer */
        buf->start = buf->buf;
        buf->len = 0;
    } else if (spl->next == wdata) {
        /* This is the first data, shift start pointer of the buffer and
         * adjust the buffer length.
         */
        buf->start = (char*)wdata->next;
        if (wdata->next > wdata) {
            buf->len -= ((char*)wdata->next - buf->start);
        } else {
            /* Overlapped */
            pj_size_t right_len, left_len;
            right_len = buf->buf + buf->max_len - (char*)wdata;
            left_len  = (char*)wdata->next - buf->buf;
            buf->len -= (right_len + left_len);
        }
    } else if (spl->prev == wdata) {
        /* This is the last data, just adjust the buffer length */
        if (wdata->prev < wdata) {
            pj_size_t jump_len;
            jump_len = (char*)wdata -
                       ((char*)wdata->prev + wdata->prev->record_len);
            buf->len -= (wdata->record_len + jump_len);
        } else {
            /* Overlapped */
            pj_size_t right_len, left_len;
            right_len = buf->buf + buf->max_len -
                        ((char*)wdata->prev + wdata->prev->record_len);
            left_len  = (char*)wdata + wdata->record_len - buf->buf;
            buf->len -= (right_len + left_len);
        }
    }
    /* For data in the middle buffer, just do nothing on the buffer. The slot
     * will be freed later when freeing the first/last data.
     */

    /* Remove the data from send pending list */
    pj_list_erase(wdata);
}

#if 0
/* Just for testing send buffer alloc/free */
#include <pj/rand.h>
pj_status_t pj_ssl_sock_ossl_test_send_buf(pj_pool_t *pool)
{
    enum { MAX_CHUNK_NUM = 20 };
    unsigned chunk_size, chunk_cnt, i;
    write_data_t *wdata[MAX_CHUNK_NUM] = {0};
    pj_time_val now;
    pj_ssl_sock_t *ssock = NULL;
    pj_ssl_sock_param param;
    pj_status_t status;

    pj_gettimeofday(&now);
    pj_srand((unsigned)now.sec);

    pj_ssl_sock_param_default(&param);
    status = pj_ssl_sock_create(pool, &param, &ssock);
    if (status != PJ_SUCCESS) {
        return status;
    }

    if (ssock->send_buf.max_len == 0) {
        ssock->send_buf.buf = (char*)
                              pj_pool_alloc(ssock->pool,
                                            ssock->param.send_buffer_size);
        ssock->send_buf.max_len = ssock->param.send_buffer_size;
        ssock->send_buf.start = ssock->send_buf.buf;
        ssock->send_buf.len = 0;
    }

    chunk_size = ssock->param.send_buffer_size / MAX_CHUNK_NUM / 2;
    chunk_cnt = 0;
    for (i = 0; i < MAX_CHUNK_NUM; i++) {
        wdata[i] = alloc_send_data(ssock, pj_rand() % chunk_size + 321);
        if (wdata[i])
            chunk_cnt++;
        else
            break;
    }

    while (chunk_cnt) {
        i = pj_rand() % MAX_CHUNK_NUM;
        if (wdata[i]) {
            free_send_data(ssock, wdata[i]);
            wdata[i] = NULL;
            chunk_cnt--;
        }
    }

    if (ssock->send_buf.len != 0)
        status = PJ_EBUG;

    pj_ssl_sock_close(ssock);
    return status;
}
#endif


#if 0
/* Flush write BIO to network socket. Note that any access to write BIO
 * MUST be serialized, so mutex protection must cover any call to OpenSSL
 * API (that possibly generate data for write BIO) along with the call to
 * this function (flushing all data in write BIO generated by above
 * OpenSSL API call).
 */
static pj_status_t flush_write_bio(pj_ssl_sock_t *ssock,
                                   pj_ioqueue_op_key_t *send_key,
                                   pj_size_t orig_len,
                                   unsigned flags, void *data)
{
    pj_ssize_t len;
    write_data_t *wdata;
    pj_size_t needed_len;
    pj_status_t status;

return PJ_SUCCESS;
#if 0
    pj_lock_acquire(ssock->write_mutex);

    /* Check if there is data in write BIO, flush it if any */
    if (!BIO_pending(ssock->ossl_wbio)) {
        pj_lock_release(ssock->write_mutex);
        return PJ_SUCCESS;
    }

    /* Get data and its length */
    len = BIO_get_mem_data(ssock->ossl_wbio, &data);
    if (len == 0) {
        pj_lock_release(ssock->write_mutex);
        return PJ_SUCCESS;
    }
#endif

    /* Calculate buffer size needed, and align it to 8 */
    needed_len = orig_len + sizeof(write_data_t);
    needed_len = ((needed_len + 7) >> 3) << 3;

    /* Allocate buffer for send data */
    wdata = alloc_send_data(ssock, needed_len);
    if (wdata == NULL) {
        pj_lock_release(ssock->write_mutex);
        return PJ_ENOMEM;
    }

    /* Copy the data and set its properties into the send data */
    pj_ioqueue_op_key_init(&wdata->key, sizeof(pj_ioqueue_op_key_t));
    wdata->key.user_data = wdata;
    wdata->app_key = send_key;
    wdata->record_len = needed_len;
    wdata->data_len = orig_len;
    wdata->plain_data_len = orig_len;
    wdata->flags = flags;
    pj_memcpy(&wdata->data, data, orig_len);

    /* Reset write BIO */
    //(void)BIO_reset(ssock->ossl_wbio);

    /* Ticket #1573: Don't hold mutex while calling PJLIB socket send(). */
    //pj_lock_release(ssock->write_mutex);

    /* Send it */
    if (ssock->param.sock_type == pj_SOCK_STREAM()) {
        status = pj_activesock_send(ssock->asock, &wdata->key,
                                    wdata->data.content, &orig_len,
                                    flags);
    } else {
        status = pj_activesock_sendto(ssock->asock, &wdata->key,
                                      wdata->data.content, &orig_len,
                                      flags,
                                      (pj_sockaddr_t*)&ssock->rem_addr,
                                      ssock->addr_len);
    }

    if (status != PJ_EPENDING) {
        /* When the sending is not pending, remove the wdata from send
         * pending list.
         */
        pj_lock_acquire(ssock->write_mutex);
        free_send_data(ssock, wdata);
        pj_lock_release(ssock->write_mutex);
    }

    return status;
}
#endif

static void on_timer(pj_timer_heap_t *th, struct pj_timer_entry *te)
{
    pj_ssl_sock_t *ssock = (pj_ssl_sock_t*)te->user_data;
    int timer_id = te->id;

    te->id = TIMER_NONE;

    PJ_UNUSED_ARG(th);

    switch (timer_id) {
    case TIMER_HANDSHAKE_TIMEOUT:
        PJ_LOG(1,(ssock->pool->obj_name, "SSL timeout after %d.%ds",
                  ssock->param.timeout.sec, ssock->param.timeout.msec));

        on_handshake_complete(ssock, PJ_ETIMEDOUT);
        break;
    case TIMER_CLOSE:
        pj_ssl_sock_close(ssock);
        break;
    default:
        pj_assert(!"Unknown timer");
        break;
    }
}


/* Asynchronouse handshake */
static pj_status_t do_handshake(pj_ssl_sock_t *ssock)
{
    int err;

    /* Perform SSL handshake */
    //pj_lock_acquire(ssock->write_mutex);
    //do {
        err = gnutls_handshake(ssock->session);
        //fprintf(stderr, "error during handshake. %s\n", gnutls_strerror(err));
    //} while (err != 0 && !gnutls_error_is_fatal(err));

    if (err == GNUTLS_E_SUCCESS) {
        ssock->ssl_state = SSL_STATE_ESTABLISHED;
        return PJ_SUCCESS;
    } else if (!gnutls_error_is_fatal(err)) {
        fprintf(stderr, "error during handshake. %s\n", gnutls_strerror(err));
        return PJ_EPENDING;
    } else {
        fprintf(stderr, "FATAL error during handshake. %s\n", gnutls_strerror(err));
        return PJ_ENOTFOUND;
    }
    //pj_lock_release(ssock->write_mutex);
}


/*
 *******************************************************************
 * Active socket callbacks.
 *******************************************************************
 */

static pj_bool_t asock_on_data_read (pj_activesock_t *asock,
                                     void *data,
                                     pj_size_t size,
                                     pj_status_t status,
                                     pj_size_t *remainder)
{
    pj_ssl_sock_t *ssock = (pj_ssl_sock_t*)
                           pj_activesock_get_user_data(asock);
#if 0
    pj_size_t nwritten;

    /* Socket error or closed */
    if (data && size > 0) {
        /* Consume the whole data */
        //nwritten = BIO_write(ssock->ossl_rbio, data, (int)size);
        if (nwritten < size) {
            status = GET_SSL_STATUS(ssock);
            goto on_error;
        }
    }
#endif

    /* Check if SSL handshake hasn't finished yet */
    if (ssock->ssl_state == SSL_STATE_HANDSHAKING) {
        pj_bool_t ret = PJ_TRUE;

        ssock->read_buf = data;
        ssock->read_buflen = size;
        if (status == PJ_SUCCESS)
            status = do_handshake(ssock);
        ssock->read_buf = NULL;

        /* Not pending is either success or failed */
        if (status != PJ_EPENDING)
            ret = on_handshake_complete(ssock, status);

        return ret;
    }

    /* See if there is any decrypted data for the application */
    if (data && size > 0 && ssock->read_started) {
        int decoded_size;
        void *decoded_data = (void *)pj_pool_calloc(ssock->pool, size, 1);

        /* Save the encrypted data and let data_pull deal with it */
        ssock->read_buf = data;
        ssock->read_buflen = size;
        decoded_size = gnutls_record_recv(ssock->session, decoded_data, size);
        ssock->read_buf = NULL;

        if (decoded_size > 0 || status != PJ_SUCCESS) {
            if (ssock->param.cb.on_data_read) {
                pj_bool_t ret;
                pj_size_t remainder_ = 0;

                // PJ_EEOF because we always read all data received
                ret = (*ssock->param.cb.on_data_read)(ssock,
                                                      decoded_data,
                                                      decoded_size,
                                                      PJ_EEOF,
                                                      &remainder_);
                if (!ret) {
                    /* We've been destroyed */
                    return PJ_FALSE;
                }
            }

            /* Active socket signalled connection closed/error, this has
             * been signalled to the application along with any remaining
             * buffer. So, let's just reset SSL socket now.  */
            if (status != PJ_SUCCESS) {
                reset_ssl_sock_state(ssock);
                return PJ_FALSE;
            }

            return PJ_TRUE;
        } else {
            /* SSL might just return SSL_ERROR_WANT_READ in
             * re-negotiation.  */
            if (decoded_size != GNUTLS_E_SUCCESS &&
                decoded_size != GNUTLS_E_AGAIN) {
                /* Reset SSL socket state, then return PJ_FALSE */
                reset_ssl_sock_state(ssock);
                goto on_error;
            }

            /* Let's try renegotiating */
            status = do_handshake(ssock);
            if (status == PJ_SUCCESS) {
                /* Update certificates */
                update_certs_info(ssock);
                /* Flush any data left in our buffers */
                status = flush_delayed_send(ssock);

                /* If flushing is ongoing, treat it as success */
                if (status == PJ_EBUSY)
                    status = PJ_SUCCESS;

                if (status != PJ_SUCCESS && status != PJ_EPENDING) {
                    PJ_PERROR(1, (ssock->pool->obj_name, status,
                                  "Failed to flush delayed send"));
                    goto on_error;
                }
            } else if (status != PJ_EPENDING) {
                PJ_PERROR(1, (ssock->pool->obj_name,
                              status, "Renegotiation failed"));
                goto on_error;
            }

            return PJ_FALSE;
        }

    }

    return PJ_TRUE;

on_error:
    if (ssock->ssl_state == SSL_STATE_HANDSHAKING)
        return on_handshake_complete(ssock, status);

    if (ssock->read_started && ssock->param.cb.on_data_read) {
        pj_bool_t ret;
        ret = (*ssock->param.cb.on_data_read)(ssock, NULL, 0, status,
                                              remainder);
        if (!ret) {
            /* We've been destroyed */
            return PJ_FALSE;
        }
    }

    reset_ssl_sock_state(ssock);
    return PJ_FALSE;
}


static pj_bool_t asock_on_data_sent (pj_activesock_t *asock,
                                     pj_ioqueue_op_key_t *send_key,
                                     pj_ssize_t sent)
{
    pj_ssl_sock_t *ssock = (pj_ssl_sock_t*)
                           pj_activesock_get_user_data(asock);

    PJ_UNUSED_ARG(send_key);
    PJ_UNUSED_ARG(sent);

    if (ssock->ssl_state == SSL_STATE_HANDSHAKING) {
        /* Initial handshaking */
        pj_status_t status;

        status = do_handshake(ssock);
        /* Not pending is either success or failed */
        if (status != PJ_EPENDING)
            return on_handshake_complete(ssock, status);

    } else if (send_key != &ssock->handshake_op_key) {
        /* Some data has been sent, notify application */
        write_data_t *wdata = (write_data_t*)send_key->user_data;
        if (ssock->param.cb.on_data_sent) {
            pj_bool_t ret;
            pj_ssize_t sent_len;

            sent_len = (sent > 0)? wdata->plain_data_len : sent;
            ret = (*ssock->param.cb.on_data_sent)(ssock, wdata->app_key,
                                                  sent_len);
            if (!ret) {
                /* We've been destroyed */
                return PJ_FALSE;
            }
        }

        /* Update write buffer state */
        pj_lock_acquire(ssock->write_mutex);
        free_send_data(ssock, wdata);
        pj_lock_release(ssock->write_mutex);

    } else {
        /* SSL re-negotiation is on-progress, just do nothing */
    }

    return PJ_TRUE;
}


static pj_bool_t asock_on_accept_complete (pj_activesock_t *asock,
                                           pj_sock_t newsock,
                                           const pj_sockaddr_t *src_addr,
                                           int src_addr_len)
{
    pj_ssl_sock_t *ssock_parent = (pj_ssl_sock_t*)
                                  pj_activesock_get_user_data(asock);
    pj_ssl_sock_t *ssock;
    pj_activesock_cb asock_cb;
    pj_activesock_cfg asock_cfg;
    unsigned i;
    pj_status_t status;

    PJ_UNUSED_ARG(src_addr_len);

    /* Create new SSL socket instance */
    status = pj_ssl_sock_create(ssock_parent->pool, &ssock_parent->param,
                                &ssock);
    if (status != PJ_SUCCESS)
        goto on_return;

    /* Update new SSL socket attributes */
    ssock->sock = newsock;
    ssock->parent = ssock_parent;
    ssock->is_server = PJ_TRUE;
    if (ssock_parent->cert) {
        status = pj_ssl_sock_set_certificate(ssock, ssock->pool,
                                             ssock_parent->cert);
        if (status != PJ_SUCCESS)
            goto on_return;
    }

    /* Apply QoS, if specified */
    status = pj_sock_apply_qos2(ssock->sock, ssock->param.qos_type,
                                &ssock->param.qos_params, 1,
                                ssock->pool->obj_name, NULL);
    if (status != PJ_SUCCESS && !ssock->param.qos_ignore_error)
        goto on_return;

    /* Update local address */
    ssock->addr_len = src_addr_len;
    status = pj_sock_getsockname(ssock->sock, &ssock->local_addr,
                                 &ssock->addr_len);
    if (status != PJ_SUCCESS) {
        /* This fails on few envs, e.g: win IOCP, just tolerate this and
         * use parent local address instead.
         */
        pj_sockaddr_cp(&ssock->local_addr, &ssock_parent->local_addr);
    }

    /* Set remote address */
    pj_sockaddr_cp(&ssock->rem_addr, src_addr);

    /* Create SSL context */
    status = create_ssl(ssock);
    if (status != PJ_SUCCESS)
        goto on_return;

    /* Prepare read buffer */
    ssock->asock_rbuf = (void**)pj_pool_calloc(ssock->pool,
                                               ssock->param.async_cnt,
                                               sizeof(void*));
    for (i = 0; i<ssock->param.async_cnt; ++i) {
        ssock->asock_rbuf[i] = (void*) pj_pool_alloc(
                                            ssock->pool,
                                            ssock->param.read_buffer_size +
                                            sizeof(read_data_t*));
    }

    /* Create active socket */
    pj_activesock_cfg_default(&asock_cfg);
    asock_cfg.async_cnt = ssock->param.async_cnt;
    asock_cfg.concurrency = ssock->param.concurrency;
    asock_cfg.whole_data = PJ_TRUE;

    pj_bzero(&asock_cb, sizeof(asock_cb));
    asock_cb.on_data_read = asock_on_data_read;
    asock_cb.on_data_sent = asock_on_data_sent;

    status = pj_activesock_create(ssock->pool,
                                  ssock->sock,
                                  ssock->param.sock_type,
                                  &asock_cfg,
                                  ssock->param.ioqueue,
                                  &asock_cb,
                                  ssock,
                                  &ssock->asock);

    if (status != PJ_SUCCESS)
        goto on_return;

    /* Start read */
    status = pj_activesock_start_read2(ssock->asock, ssock->pool,
                                       (unsigned)ssock->param.read_buffer_size,
                                       ssock->asock_rbuf,
                                       PJ_IOQUEUE_ALWAYS_ASYNC);
    if (status != PJ_SUCCESS)
        goto on_return;

    /* Prepare write/send state */
    pj_assert(ssock->send_buf.max_len == 0);
    ssock->send_buf.buf = (char*)
                          pj_pool_alloc(ssock->pool,
                                        ssock->param.send_buffer_size);
    ssock->send_buf.max_len = ssock->param.send_buffer_size;
    ssock->send_buf.start = ssock->send_buf.buf;
    ssock->send_buf.len = 0;

    /* Start handshake timer */
    if (ssock->param.timer_heap && (ssock->param.timeout.sec != 0 ||
        ssock->param.timeout.msec != 0))
    {
        pj_assert(ssock->timer.id == TIMER_NONE);
        ssock->timer.id = TIMER_HANDSHAKE_TIMEOUT;
        status = pj_timer_heap_schedule(ssock->param.timer_heap,
                                        &ssock->timer,
                                        &ssock->param.timeout);
        if (status != PJ_SUCCESS)
            ssock->timer.id = TIMER_NONE;
    }

    /* Start SSL handshake */
    ssock->ssl_state = SSL_STATE_HANDSHAKING;
    //SSL_set_accept_state(ssock->ossl_ssl);
    status = do_handshake(ssock);

on_return:
    if (ssock && status != PJ_EPENDING)
        on_handshake_complete(ssock, status);

    /* Must return PJ_TRUE whatever happened, as active socket must
     * continue listening.
     */
    return PJ_TRUE;
}


static pj_bool_t asock_on_connect_complete (pj_activesock_t *asock,
                                            pj_status_t status)
{
    pj_ssl_sock_t *ssock = (pj_ssl_sock_t*)
                           pj_activesock_get_user_data(asock);
    unsigned i;

    if (status != PJ_SUCCESS)
        goto on_return;

    /* Update local address */
    ssock->addr_len = sizeof(pj_sockaddr);
    status = pj_sock_getsockname(ssock->sock, &ssock->local_addr,
                                 &ssock->addr_len);
    if (status != PJ_SUCCESS)
        goto on_return;

    /* Create SSL context */
    status = create_ssl(ssock);
    if (status != PJ_SUCCESS)
        goto on_return;

    /* Prepare read buffer */
    ssock->asock_rbuf = (void**)pj_pool_calloc(ssock->pool,
                                               ssock->param.async_cnt,
                                               sizeof(void*));
    for (i = 0; i<ssock->param.async_cnt; ++i) {
        ssock->asock_rbuf[i] = (void*) pj_pool_alloc(
                                            ssock->pool,
                                            ssock->param.read_buffer_size +
                                            sizeof(read_data_t*));
    }

    /* Start read */
    status = pj_activesock_start_read2(ssock->asock, ssock->pool,
                                       (unsigned)ssock->param.read_buffer_size,
                                       ssock->asock_rbuf,
                                       PJ_IOQUEUE_ALWAYS_ASYNC);
    if (status != PJ_SUCCESS)
        goto on_return;

    /* Prepare write/send state */
    pj_assert(ssock->send_buf.max_len == 0);
    ssock->send_buf.buf = (char*)
                             pj_pool_alloc(ssock->pool,
                                           ssock->param.send_buffer_size);
    ssock->send_buf.max_len = ssock->param.send_buffer_size;
    ssock->send_buf.start = ssock->send_buf.buf;
    ssock->send_buf.len = 0;

    /* Set server name to connect */
    if (ssock->param.server_name.slen) {
        /* Server name is null terminated already */
        if (gnutls_server_name_set(ssock->session, GNUTLS_NAME_DNS,
                                   ssock->param.server_name.ptr,
                                   ssock->param.server_name.slen) != GNUTLS_E_SUCCESS) {
            char err_str[PJ_ERR_MSG_SIZE];

            ERR_error_string_n(ERR_get_error(), err_str, sizeof(err_str));
            PJ_LOG(3,(ssock->pool->obj_name, "SSL_set_tlsext_host_name() "
                "failed: %s", err_str));
        }
    }

    /* Start SSL handshake */
    ssock->ssl_state = SSL_STATE_HANDSHAKING;

    status = do_handshake(ssock);
    if (status != PJ_EPENDING)
        goto on_return;

    return PJ_TRUE;

on_return:
    return on_handshake_complete(ssock, status);
}



/*
 *******************************************************************
 * API
 *******************************************************************
 */

/* Load credentials from files. */
PJ_DEF(pj_status_t) pj_ssl_cert_load_from_files (pj_pool_t *pool,
                                                 const pj_str_t *CA_file,
                                                 const pj_str_t *cert_file,
                                                 const pj_str_t *privkey_file,
                                                 const pj_str_t *privkey_pass,
                                                 pj_ssl_cert_t **p_cert)
{
    pj_ssl_cert_t *cert;

    PJ_ASSERT_RETURN(pool && CA_file && cert_file && privkey_file, PJ_EINVAL);

    cert = PJ_POOL_ZALLOC_T(pool, pj_ssl_cert_t);
    pj_strdup_with_null(pool, &cert->CA_file, CA_file);
    pj_strdup_with_null(pool, &cert->cert_file, cert_file);
    pj_strdup_with_null(pool, &cert->privkey_file, privkey_file);
    pj_strdup_with_null(pool, &cert->privkey_pass, privkey_pass);

    *p_cert = cert;

    return PJ_SUCCESS;
}


/* Set SSL socket credentials. */
PJ_DECL(pj_status_t) pj_ssl_sock_set_certificate(
                                            pj_ssl_sock_t *ssock,
                                            pj_pool_t *pool,
                                            const pj_ssl_cert_t *cert)
{
    pj_ssl_cert_t *cert_;

    PJ_ASSERT_RETURN(ssock && pool && cert, PJ_EINVAL);

    cert_ = PJ_POOL_ZALLOC_T(pool, pj_ssl_cert_t);
    pj_memcpy(cert_, cert, sizeof(cert));
    pj_strdup_with_null(pool, &cert_->CA_file, &cert->CA_file);
    pj_strdup_with_null(pool, &cert_->cert_file, &cert->cert_file);
    pj_strdup_with_null(pool, &cert_->privkey_file, &cert->privkey_file);
    pj_strdup_with_null(pool, &cert_->privkey_pass, &cert->privkey_pass);

    ssock->cert = cert_;

    return PJ_SUCCESS;
}


/* Get available ciphers. */
PJ_DEF(pj_status_t) pj_ssl_cipher_get_availables(pj_ssl_cipher ciphers[],
                                                 unsigned *cipher_num)
{
    unsigned i;

    PJ_ASSERT_RETURN(ciphers && cipher_num, PJ_EINVAL);

    if (openssl_cipher_num == 0) {
        init_openssl();
        shutdown_openssl();
    }

    if (openssl_cipher_num == 0) {
        *cipher_num = 0;
        return PJ_ENOTFOUND;
    }

    *cipher_num = PJ_MIN(*cipher_num, openssl_cipher_num);

    for (i = 0; i < *cipher_num; ++i)
        ciphers[i] = openssl_ciphers[i].id;

    return PJ_SUCCESS;
}


/* Get cipher name string */
PJ_DEF(const char*) pj_ssl_cipher_name(pj_ssl_cipher cipher)
{
    unsigned i;

    if (openssl_cipher_num == 0) {
        init_openssl();
        shutdown_openssl();
    }

    for (i = 0; i < openssl_cipher_num; ++i) {
        if (cipher == openssl_ciphers[i].id)
            return openssl_ciphers[i].name;
    }

    return NULL;
}

/* Get cipher identifier */
PJ_DEF(pj_ssl_cipher) pj_ssl_cipher_id(const char *cipher_name)
{
    unsigned i;

    if (openssl_cipher_num == 0) {
        init_openssl();
        shutdown_openssl();
    }

    for (i = 0; i < openssl_cipher_num; ++i) {
        if (!pj_ansi_stricmp(openssl_ciphers[i].name, cipher_name))
            return openssl_ciphers[i].id;
    }

    return PJ_TLS_UNKNOWN_CIPHER;
}

/* Check if the specified cipher is supported by SSL/TLS backend. */
PJ_DEF(pj_bool_t) pj_ssl_cipher_is_supported(pj_ssl_cipher cipher)
{
    unsigned i;

    if (openssl_cipher_num == 0) {
        init_openssl();
        shutdown_openssl();
    }

    for (i = 0; i < openssl_cipher_num; ++i) {
        if (cipher == openssl_ciphers[i].id)
            return PJ_TRUE;
    }

    return PJ_FALSE;
}


/*
 * Create SSL socket instance.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_create (pj_pool_t *pool,
                                        const pj_ssl_sock_param *param,
                                        pj_ssl_sock_t **p_ssock)
{
    pj_ssl_sock_t *ssock;
    pj_status_t status;

    PJ_ASSERT_RETURN(pool && param && p_ssock, PJ_EINVAL);
    PJ_ASSERT_RETURN(param->sock_type == pj_SOCK_STREAM(), PJ_ENOTSUP);

    pool = pj_pool_create(pool->factory, "ssl%p", 512, 512, NULL);

    /* Create secure socket */
    ssock = PJ_POOL_ZALLOC_T(pool, pj_ssl_sock_t);
    ssock->pool = pool;
    ssock->sock = PJ_INVALID_SOCKET;
    ssock->ssl_state = SSL_STATE_NULL;
    pj_list_init(&ssock->write_pending);
    pj_list_init(&ssock->write_pending_empty);
    pj_list_init(&ssock->send_pending);
    pj_timer_entry_init(&ssock->timer, 0, ssock, &on_timer);
    pj_ioqueue_op_key_init(&ssock->handshake_op_key,
                           sizeof(pj_ioqueue_op_key_t));

    /* Create secure socket mutex */
    status = pj_lock_create_recursive_mutex(pool, pool->obj_name,
                                            &ssock->write_mutex);
    if (status != PJ_SUCCESS)
        return status;

    /* Init secure socket param */
    ssock->param = *param;
    ssock->param.read_buffer_size = ((ssock->param.read_buffer_size + 7) >> 3) << 3;
    if (param->ciphers_num > 0) {
        unsigned i;
        ssock->param.ciphers = (pj_ssl_cipher*)
                               pj_pool_calloc(pool, param->ciphers_num,
                                              sizeof(pj_ssl_cipher));
        for (i = 0; i < param->ciphers_num; ++i)
            ssock->param.ciphers[i] = param->ciphers[i];
    }

    /* Server name must be null-terminated */
    pj_strdup_with_null(pool, &ssock->param.server_name,
                        &param->server_name);

    /* Finally */
    *p_ssock = ssock;

    return PJ_SUCCESS;
}


/*
 * Close the secure socket. This will unregister the socket from the
 * ioqueue and ultimately close the socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_close(pj_ssl_sock_t *ssock)
{
    pj_pool_t *pool;

    PJ_ASSERT_RETURN(ssock, PJ_EINVAL);

    if (!ssock->pool)
        return PJ_SUCCESS;

    if (ssock->timer.id != TIMER_NONE) {
        pj_timer_heap_cancel(ssock->param.timer_heap, &ssock->timer);
        ssock->timer.id = TIMER_NONE;
    }

    reset_ssl_sock_state(ssock);
    pj_lock_destroy(ssock->write_mutex);

    pool = ssock->pool;
    ssock->pool = NULL;
    if (pool)
        pj_pool_release(pool);

    return PJ_SUCCESS;
}


/*
 * Associate arbitrary data with the secure socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_set_user_data(pj_ssl_sock_t *ssock,
                                              void *user_data)
{
    PJ_ASSERT_RETURN(ssock, PJ_EINVAL);

    ssock->param.user_data = user_data;
    return PJ_SUCCESS;
}


/*
 * Retrieve the user data previously associated with this secure
 * socket.
 */
PJ_DEF(void*) pj_ssl_sock_get_user_data(pj_ssl_sock_t *ssock)
{
    PJ_ASSERT_RETURN(ssock, NULL);

    return ssock->param.user_data;
}


/*
 * Retrieve the local address and port used by specified SSL socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_get_info (pj_ssl_sock_t *ssock,
                                          pj_ssl_sock_info *info)
{
    pj_bzero(info, sizeof(*info));

    /* Established flag */
    info->established = (ssock->ssl_state == SSL_STATE_ESTABLISHED);

    /* Protocol */
    info->proto = ssock->param.proto;

    /* Local address */
    pj_sockaddr_cp(&info->local_addr, &ssock->local_addr);

    if (info->established) {
        gnutls_cipher_algorithm_t cipher;

        /* Current cipher */
        cipher = gnutls_cipher_get(ssock->session);
        info->cipher = (cipher & 0x00FFFFFF);

        /* Remote address */
        pj_sockaddr_cp(&info->remote_addr, &ssock->rem_addr);

        /* Certificates info */
        info->local_cert_info = &ssock->local_cert_info;
        info->remote_cert_info = &ssock->remote_cert_info;

        /* Verification status */
        info->verify_status = ssock->verify_status;
    }

    /* Last known OpenSSL error code */
    info->last_native_err = ssock->last_err;

    return PJ_SUCCESS;
}


/*
 * Starts read operation on this secure socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_start_read (pj_ssl_sock_t *ssock,
                                            pj_pool_t *pool,
                                            unsigned buff_size,
                                            pj_uint32_t flags)
{
    void **readbuf;
    unsigned i;

    PJ_ASSERT_RETURN(ssock && pool && buff_size, PJ_EINVAL);
    PJ_ASSERT_RETURN(ssock->ssl_state==SSL_STATE_ESTABLISHED, PJ_EINVALIDOP);

    readbuf = (void**) pj_pool_calloc(pool, ssock->param.async_cnt,
                                      sizeof(void*));

    for (i=0; i<ssock->param.async_cnt; ++i) {
        readbuf[i] = pj_pool_alloc(pool, buff_size);
    }

    return pj_ssl_sock_start_read2(ssock, pool, buff_size,
                                   readbuf, flags);
}


/*
 * Same as #pj_ssl_sock_start_read(), except that the application
 * supplies the buffers for the read operation so that the acive socket
 * does not have to allocate the buffers.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_start_read2 (pj_ssl_sock_t *ssock,
                                             pj_pool_t *pool,
                                             unsigned buff_size,
                                             void *readbuf[],
                                             pj_uint32_t flags)
{
    unsigned i;

    PJ_ASSERT_RETURN(ssock && pool && buff_size && readbuf, PJ_EINVAL);
    PJ_ASSERT_RETURN(ssock->ssl_state==SSL_STATE_ESTABLISHED, PJ_EINVALIDOP);

    /* Create SSL socket read buffer */
    ssock->ssock_rbuf = (read_data_t*)pj_pool_calloc(pool,
                                               ssock->param.async_cnt,
                                               sizeof(read_data_t));

    /* Store SSL socket read buffer pointer in the activesock read buffer */
    for (i = 0; i < ssock->param.async_cnt; ++i) {
        read_data_t **p_ssock_rbuf =
                        OFFSET_OF_READ_DATA_PTR(ssock, ssock->asock_rbuf[i]);

        ssock->ssock_rbuf[i].data = readbuf[i];
        ssock->ssock_rbuf[i].len = 0;

        *p_ssock_rbuf = &ssock->ssock_rbuf[i];
    }

    ssock->read_size = buff_size;
    ssock->read_started = PJ_TRUE;
    ssock->read_flags = flags;

    return PJ_SUCCESS;
}


/*
 * Same as pj_ssl_sock_start_read(), except that this function is used
 * only for datagram sockets, and it will trigger \a on_data_recvfrom()
 * callback instead.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_start_recvfrom (pj_ssl_sock_t *ssock,
                                                pj_pool_t *pool,
                                                unsigned buff_size,
                                                pj_uint32_t flags)
{
    PJ_UNUSED_ARG(ssock);
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(buff_size);
    PJ_UNUSED_ARG(flags);

    return PJ_ENOTSUP;
}


/*
 * Same as #pj_ssl_sock_start_recvfrom() except that the recvfrom()
 * operation takes the buffer from the argument rather than creating
 * new ones.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_start_recvfrom2 (pj_ssl_sock_t *ssock,
                                                 pj_pool_t *pool,
                                                 unsigned buff_size,
                                                 void *readbuf[],
                                                 pj_uint32_t flags)
{
    PJ_UNUSED_ARG(ssock);
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(buff_size);
    PJ_UNUSED_ARG(readbuf);
    PJ_UNUSED_ARG(flags);

    return PJ_ENOTSUP;
}

/* Write plain data to SSL and flush write BIO. */
static pj_status_t ssl_write(pj_ssl_sock_t *ssock,
                             pj_ioqueue_op_key_t *send_key,
                             const void *data,
                             pj_ssize_t size,
                             unsigned flags)
{
    pj_status_t status;
    int nwritten;

    /* Write the plain data to SSL, after SSL encrypts it, write BIO will
     * contain the secured data to be sent via socket. Note that re-
     * negotitation may be on progress, so sending data should be delayed
     * until re-negotiation is completed.
     */
    //pj_lock_acquire(ssock->write_mutex);
    nwritten = gnutls_record_send(ssock->session, data, size);
    //SSL_write(ssock->ossl_ssl, data, (int)size);
    //pj_lock_release(ssock->write_mutex);

    if (nwritten == size) {
        /* All data written, flush write BIO to network socket */
        //status = flush_write_bio(ssock, send_key, size, flags, data);
        status = PJ_SUCCESS;
    } else if (nwritten <= 0) {
        /* SSL failed to process the data, it may just that re-negotiation
         * is on progress.
         */
        //int err;
        //err = SSL_get_error(ssock->ossl_ssl, nwritten);
        //if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_NONE) {
            /* Re-negotiation is on progress, flush re-negotiation data */
          //  status = flush_write_bio(ssock, &ssock->handshake_op_key, 0, 0, NULL);
          //  if (status == PJ_SUCCESS || status == PJ_EPENDING)
                /* Just return PJ_EBUSY when re-negotiation is on progress */
                status = PJ_EBUSY;

        //} else {
            /* Some problem occured */
          //  status = STATUS_FROM_SSL_ERR(ssock, err);
        //}
    } else {
        /* nwritten < *size, shouldn't happen, unless write BIO cannot hold
         * the whole secured data, perhaps because of insufficient memory.
         */
        status = PJ_ENOMEM;
    }

    return status;
}

/* Flush delayed data sending in the write pending list. */
static pj_status_t flush_delayed_send(pj_ssl_sock_t *ssock)
{
    /* Check for another ongoing flush */
    if (ssock->flushing_write_pend)
        return PJ_EBUSY;

    pj_lock_acquire(ssock->write_mutex);

    /* Again, check for another ongoing flush */
    if (ssock->flushing_write_pend) {
        pj_lock_release(ssock->write_mutex);
        return PJ_EBUSY;
    }

    /* Set ongoing flush flag */
    ssock->flushing_write_pend = PJ_TRUE;

    while (!pj_list_empty(&ssock->write_pending)) {
        write_data_t *wp;
        pj_status_t status;

        wp = ssock->write_pending.next;

        /* Ticket #1573: Don't hold mutex while calling socket send. */
        pj_lock_release(ssock->write_mutex);

        status = ssl_write(ssock, &wp->key, wp->data.ptr,
                           wp->plain_data_len, wp->flags);
        if (status != PJ_SUCCESS) {
            /* Reset ongoing flush flag first. */
            ssock->flushing_write_pend = PJ_FALSE;
            return status;
        }

        pj_lock_acquire(ssock->write_mutex);
        pj_list_erase(wp);
        pj_list_push_back(&ssock->write_pending_empty, wp);
    }

    /* Reset ongoing flush flag */
    ssock->flushing_write_pend = PJ_FALSE;

    pj_lock_release(ssock->write_mutex);

    return PJ_SUCCESS;
}

/* Sending is delayed, push back the sending data into pending list. */
static pj_status_t delay_send (pj_ssl_sock_t *ssock,
                               pj_ioqueue_op_key_t *send_key,
                               const void *data,
                               pj_ssize_t size,
                               unsigned flags)
{
    write_data_t *wp;

    pj_lock_acquire(ssock->write_mutex);

    /* Init write pending instance */
    if (!pj_list_empty(&ssock->write_pending_empty)) {
        wp = ssock->write_pending_empty.next;
        pj_list_erase(wp);
    } else {
        wp = PJ_POOL_ZALLOC_T(ssock->pool, write_data_t);
    }

    wp->app_key = send_key;
    wp->plain_data_len = size;
    wp->data.ptr = data;
    wp->flags = flags;

    pj_list_push_back(&ssock->write_pending, wp);

    pj_lock_release(ssock->write_mutex);

    /* Must return PJ_EPENDING */
    return PJ_EPENDING;
}

/**
 * Send data using the socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_send (pj_ssl_sock_t *ssock,
                                      pj_ioqueue_op_key_t *send_key,
                                      const void *data,
                                      pj_ssize_t *size,
                                      unsigned flags)
{
    pj_status_t status;

    PJ_ASSERT_RETURN(ssock && data && size && (*size>0), PJ_EINVAL);
    PJ_ASSERT_RETURN(ssock->ssl_state==SSL_STATE_ESTABLISHED, PJ_EINVALIDOP);

    // Ticket #1573: Don't hold mutex while calling PJLIB socket send().
    //pj_lock_acquire(ssock->write_mutex);

    /* Flush delayed send first. Sending data might be delayed when
     * re-negotiation is on-progress.
     */
    status = flush_delayed_send(ssock);
    if (status == PJ_EBUSY) {
        /* Re-negotiation or flushing is on progress, delay sending */
        status = delay_send(ssock, send_key, data, *size, flags);
        goto on_return;
    } else if (status != PJ_SUCCESS) {
        goto on_return;
    }

    /* Write data to SSL */
    status = ssl_write(ssock, send_key, data, *size, flags);
    if (status == PJ_EBUSY) {
        /* Re-negotiation is on progress, delay sending */
        status = delay_send(ssock, send_key, data, *size, flags);
    }

on_return:
    //pj_lock_release(ssock->write_mutex);
    return status;
}


/**
 * Send datagram using the socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_sendto (pj_ssl_sock_t *ssock,
                                        pj_ioqueue_op_key_t *send_key,
                                        const void *data,
                                        pj_ssize_t *size,
                                        unsigned flags,
                                        const pj_sockaddr_t *addr,
                                        int addr_len)
{
    PJ_UNUSED_ARG(ssock);
    PJ_UNUSED_ARG(send_key);
    PJ_UNUSED_ARG(data);
    PJ_UNUSED_ARG(size);
    PJ_UNUSED_ARG(flags);
    PJ_UNUSED_ARG(addr);
    PJ_UNUSED_ARG(addr_len);

    return PJ_ENOTSUP;
}


/**
 * Starts asynchronous socket accept() operations on this secure socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_start_accept (pj_ssl_sock_t *ssock,
                                              pj_pool_t *pool,
                                              const pj_sockaddr_t *localaddr,
                                              int addr_len)
{
    pj_activesock_cb asock_cb;
    pj_activesock_cfg asock_cfg;
    pj_status_t status;

    PJ_ASSERT_RETURN(ssock && pool && localaddr && addr_len, PJ_EINVAL);

    /* Create socket */
    status = pj_sock_socket(ssock->param.sock_af, ssock->param.sock_type, 0,
                            &ssock->sock);
    if (status != PJ_SUCCESS)
        goto on_error;

    /* Apply SO_REUSEADDR */
    if (ssock->param.reuse_addr) {
        int enabled = 1;
        status = pj_sock_setsockopt(ssock->sock, pj_SOL_SOCKET(),
                                    pj_SO_REUSEADDR(),
                                    &enabled, sizeof(enabled));
        if (status != PJ_SUCCESS) {
            PJ_PERROR(4,(ssock->pool->obj_name, status,
                         "Warning: error applying SO_REUSEADDR"));
        }
    }

    /* Apply QoS, if specified */
    status = pj_sock_apply_qos2(ssock->sock, ssock->param.qos_type,
                                &ssock->param.qos_params, 2,
                                ssock->pool->obj_name, NULL);
    if (status != PJ_SUCCESS && !ssock->param.qos_ignore_error)
        goto on_error;

    /* Bind socket */
    status = pj_sock_bind(ssock->sock, localaddr, addr_len);
    if (status != PJ_SUCCESS)
        goto on_error;

    /* Start listening to the address */
    status = pj_sock_listen(ssock->sock, PJ_SOMAXCONN);
    if (status != PJ_SUCCESS)
        goto on_error;

    /* Create active socket */
    pj_activesock_cfg_default(&asock_cfg);
    asock_cfg.async_cnt = ssock->param.async_cnt;
    asock_cfg.concurrency = ssock->param.concurrency;
    asock_cfg.whole_data = PJ_TRUE;

    pj_bzero(&asock_cb, sizeof(asock_cb));
    asock_cb.on_accept_complete = asock_on_accept_complete;

    status = pj_activesock_create(pool,
                                  ssock->sock,
                                  ssock->param.sock_type,
                                  &asock_cfg,
                                  ssock->param.ioqueue,
                                  &asock_cb,
                                  ssock,
                                  &ssock->asock);

    if (status != PJ_SUCCESS)
        goto on_error;

    /* Start accepting */
    status = pj_activesock_start_accept(ssock->asock, pool);
    if (status != PJ_SUCCESS)
        goto on_error;

    /* Update local address */
    ssock->addr_len = addr_len;
    status = pj_sock_getsockname(ssock->sock, &ssock->local_addr,
                                 &ssock->addr_len);
    if (status != PJ_SUCCESS)
        pj_sockaddr_cp(&ssock->local_addr, localaddr);

    ssock->is_server = PJ_TRUE;

    return PJ_SUCCESS;

on_error:
    reset_ssl_sock_state(ssock);
    return status;
}


/**
 * Starts asynchronous socket connect() operation.
 */
PJ_DECL(pj_status_t) pj_ssl_sock_start_connect(pj_ssl_sock_t *ssock,
                                               pj_pool_t *pool,
                                               const pj_sockaddr_t *localaddr,
                                               const pj_sockaddr_t *remaddr,
                                               int addr_len)
{
    pj_activesock_cb asock_cb;
    pj_activesock_cfg asock_cfg;
    pj_status_t status;

    PJ_ASSERT_RETURN(ssock && pool && localaddr && remaddr && addr_len,
                     PJ_EINVAL);

    /* Create socket */
    status = pj_sock_socket(ssock->param.sock_af, ssock->param.sock_type, 0,
                            &ssock->sock);
    if (status != PJ_SUCCESS)
        goto on_error;

    /* Apply QoS, if specified */
    status = pj_sock_apply_qos2(ssock->sock, ssock->param.qos_type,
                                &ssock->param.qos_params, 2,
                                ssock->pool->obj_name, NULL);
    if (status != PJ_SUCCESS && !ssock->param.qos_ignore_error)
        goto on_error;

    /* Bind socket */
    status = pj_sock_bind(ssock->sock, localaddr, addr_len);
    if (status != PJ_SUCCESS)
        goto on_error;

    /* Create active socket */
    pj_activesock_cfg_default(&asock_cfg);
    asock_cfg.async_cnt = ssock->param.async_cnt;
    asock_cfg.concurrency = ssock->param.concurrency;
    asock_cfg.whole_data = PJ_TRUE;

    pj_bzero(&asock_cb, sizeof(asock_cb));
    asock_cb.on_connect_complete = asock_on_connect_complete;
    asock_cb.on_data_read = asock_on_data_read;
    asock_cb.on_data_sent = asock_on_data_sent;

    status = pj_activesock_create(pool,
                                  ssock->sock,
                                  ssock->param.sock_type,
                                  &asock_cfg,
                                  ssock->param.ioqueue,
                                  &asock_cb,
                                  ssock,
                                  &ssock->asock);

    if (status != PJ_SUCCESS)
        goto on_error;

    /* Save remote address */
    pj_sockaddr_cp(&ssock->rem_addr, remaddr);

    /* Start timer */
    if (ssock->param.timer_heap && (ssock->param.timeout.sec != 0 ||
        ssock->param.timeout.msec != 0))
    {
        pj_assert(ssock->timer.id == TIMER_NONE);
        ssock->timer.id = TIMER_HANDSHAKE_TIMEOUT;
        status = pj_timer_heap_schedule(ssock->param.timer_heap,
                                        &ssock->timer,
                                        &ssock->param.timeout);
        if (status != PJ_SUCCESS)
            ssock->timer.id = TIMER_NONE;
    }

    status = pj_activesock_start_connect(ssock->asock, pool, remaddr,
                                         addr_len);

    if (status == PJ_SUCCESS)
        asock_on_connect_complete(ssock->asock, PJ_SUCCESS);
    else if (status != PJ_EPENDING)
        goto on_error;

    /* Update local address */
    ssock->addr_len = addr_len;
    status = pj_sock_getsockname(ssock->sock, &ssock->local_addr,
                                 &ssock->addr_len);
    /* Note that we may not get an IP address here. This can
     * happen for example on Windows, where getsockname()
     * would return 0.0.0.0 if socket has just started the
     * async connect. In this case, just leave the local
     * address with 0.0.0.0 for now; it will be updated
     * once the socket is established.
     */

    /* Update SSL state */
    ssock->is_server = PJ_FALSE;

    return PJ_EPENDING;

on_error:
    reset_ssl_sock_state(ssock);
    return status;
}


PJ_DEF(pj_status_t) pj_ssl_sock_renegotiate(pj_ssl_sock_t *ssock)
{
    int ret;
    pj_status_t status;

    PJ_ASSERT_RETURN(ssock->ssl_state == SSL_STATE_ESTABLISHED, PJ_EINVALIDOP);

    if (SSL_renegotiate_pending(ssock->ossl_ssl))
        return PJ_EPENDING;

    ret = SSL_renegotiate(ssock->ossl_ssl);
    if (ret <= 0) {
        status = GET_SSL_STATUS(ssock);
    } else {
        status = do_handshake(ssock);
    }

    return status;
}

#endif  /* PJ_HAS_SSL_SOCK */
