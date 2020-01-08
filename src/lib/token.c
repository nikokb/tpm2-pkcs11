/* SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "attrs.h"
#include "checks.h"
#include "db.h"
#include "list.h"
#include "pkcs11.h"
#include "session.h"
#include "session_table.h"
#include "slot.h"
#include "tpm.h"
#include "token.h"
#include "utils.h"

CK_RV token_min_init(token *t) {
    /*
     * Initialize the per-token session table
     */
    CK_RV rv = session_table_new(&t->s_table);
    if (rv != CKR_OK) {
        LOGE("Could not initialize session table");
        return rv;
    }

    /*
     * Initialize the per-token tpm context
     */
    rv = tpm_ctx_new(t->config.tcti, &t->tctx);
    if (rv != CKR_OK) {
        LOGE("Could not initialize tpm ctx: 0x%lx", rv);
        return rv;
    }

    rv = mutex_create(&t->mutex);
    if (rv != CKR_OK) {
        LOGE("Could not initialize mutex: 0x%lx", rv);
    }

    return rv;
}

void token_free_list(token *t, size_t len) {

    size_t i;
    for (i=0; i < len; i++) {
        token_free(&t[i]);
    }
    free(t);
}

void token_free(token *t) {

    /*
     * for each session remove them
     */
    session_table_free_ctx_all(t);
    session_table_free(t->s_table);

    twist_free(t->pobject.objauth);

    sealobject_free(&t->sealobject);

    if (t->tobjects) {
        list *cur = &t->tobjects->l;
        while(cur) {
            tobject *tobj = list_entry(cur, tobject, l);
            cur = cur->next;
            tobject_free(tobj);
        }
    }

    tpm_ctx_free(t->tctx);

    mutex_destroy(t->mutex);

    free(t->config.tcti);
}

CK_RV token_get_info (token *t, CK_TOKEN_INFO *info) {
    check_pointer(t);
    check_pointer(info);

    int rval;
    time_t rawtime;
    struct tm tminfo;

    memset(info, 0, sizeof(*info));

    rval = tpm_get_token_info(t->tctx, info);
    if (rval != CKR_OK) {
        return CKR_GENERAL_ERROR;
    }

    // Support Flags
    info->flags = CKF_RNG
        | CKF_LOGIN_REQUIRED;

    if (t->config.is_initialized) {
        info->flags |= CKF_TOKEN_INITIALIZED;
        info->flags |= CKF_USER_PIN_INITIALIZED;
    }

    // Identification
    str_padded_copy(info->label, t->label, sizeof(info->label));
    str_padded_copy(info->serialNumber, (unsigned char*) TPM2_TOKEN_SERIAL_NUMBER, sizeof(info->serialNumber));


    // Memory: TODO not sure what memory values should go here, the platform?
    info->ulFreePrivateMemory = ~0;
    info->ulFreePublicMemory = ~0;
    info->ulTotalPrivateMemory = ~0;
    info->ulTotalPublicMemory = ~0;

    // Maximums and Minimums
    info->ulMaxPinLen = 128;
    info->ulMinPinLen = 0;
    info->ulMaxSessionCount = MAX_NUM_OF_SESSIONS;
    info->ulMaxRwSessionCount = MAX_NUM_OF_SESSIONS;

    // Session
    session_table_get_cnt(t->s_table, &info->ulSessionCount, &info->ulRwSessionCount, NULL);

    // Time
    time (&rawtime);
    gmtime_r(&rawtime, &tminfo);
    strftime ((char *)info->utcTime, sizeof(info->utcTime), "%Y%m%d%H%M%S", &tminfo);
    // The last two bytes must be '0', not NULL/'\0' terminated.
    info->utcTime[14] = '0';
    info->utcTime[15] = '0';

    return CKR_OK;
}

CK_RV get_or_create_primary(token *t) {

    twist blob = NULL;

    /* if there is no primary object ... */
    if (t->pid) {
        return CKR_OK;
    }

    /* is there one in the db to use ? */
    CK_RV rv = db_get_first_pid(&t->pid);
    if (rv != CKR_OK) {
        return rv;
    }

    /* if so use it */
    if (t->pid) {
        /* tokens in the DB store already have an associated primary object */
        return db_init_pobject(t->pid, &t->pobject, t->tctx);
    }

    /* is their a PC client spec key ? */
    rv = tpm_get_existing_primary(t->tctx, &t->pobject.handle, &blob);
    if (rv != CKR_OK) {
        return rv;
    }

    /* nothing, create one */
    if (!t->pobject.handle) {
        rv = tpm_create_primary(t->tctx, &t->pobject.handle, &blob);
        if (rv != CKR_OK) {
            return rv;
        }
    }

    assert(t->pobject.handle);

    rv = db_add_primary(blob, &t->pid);
    assert(t->pid);
    twist_free(blob);
    return rv;
}

CK_RV token_init(token *t, CK_BYTE_PTR pin, CK_ULONG pin_len, CK_BYTE_PTR label) {
    check_pointer(pin);
    check_pointer(label);

    CK_RV rv = CKR_GENERAL_ERROR;

    twist newauth = NULL;
    twist newsalthex = NULL;

    twist sopin = twistbin_new(pin, pin_len);
    if (!sopin) {
        LOGE("oom");
        return CKR_HOST_MEMORY;
    }

    twist hexwrappingkey = utils_get_rand_hex_str(32);
    if (!sopin) {
        LOGE("oom");
        rv = CKR_HOST_MEMORY;
        goto out;
    }

    /*
     * find or create a primary object and get the serialized blob
     * for it.
     */
    rv = get_or_create_primary(t);
    if (rv != CKR_OK) {
        LOGE("Could not find nor create a primary object");
        goto error;
    }

    rv = utils_setup_new_object_auth(sopin, &newauth, &newsalthex);
    if (rv != CKR_OK) {
        goto error;
    }

    /* we have a primary object, create the seal object underneath it */
    rv = tpm2_create_seal_obj(t->tctx, t->pobject.objauth, t->pobject.handle,
            newauth, NULL, hexwrappingkey, &t->sealobject.sopub,
            &t->sealobject.sopriv, &t->sealobject.handle);
    if (rv != CKR_OK) {
        LOGE("Could not create SO seal object");
        goto error;
    }

    t->sealobject.soauthsalt = newsalthex;
    newsalthex = NULL;

    memcpy(t->label, label, sizeof(t->label));

    /* TODO get TCTI config from ENV var and use throughout this process */
    t->config.is_initialized = true;

    rv = db_add_token(t);
    if (rv != CKR_OK) {
        LOGE("Could not add token to db");
        goto error;
    }

    assert(t->id);

    rv = slot_add_uninit_token();
    if (rv != CKR_OK) {
        LOGW("Could not add unitialized token");
    }

    rv =  CKR_OK;
out:
    twist_free(sopin);
    twist_free(newauth);
    twist_free(newsalthex);
    twist_free(hexwrappingkey);

    return rv;

error:
    token_free(t);
    token_min_init(t);
    t->config.is_initialized = false;
    goto out;
}

bool token_is_any_user_logged_in(token *tok) {

    return tok->login_state != token_no_one_logged_in;
}

bool token_is_user_logged_in(token *tok) {

    return tok->login_state != token_user_logged_in;
}

void token_lock(token *t) {
    mutex_lock_fatal(t->mutex);
}

void token_unlock(token *t) {
    mutex_unlock_fatal(t->mutex);
}

static void change_token_mem_data(token *tok, bool is_so, uint32_t new_seal_handle,
        twist newsalthex, twist newprivblob, twist newpubblob) {

    tok->sealobject.handle = new_seal_handle;
    twist *authsalt;
    twist *priv;
    twist *pub;

    if (is_so) {
        authsalt = &tok->sealobject.soauthsalt;
        priv = &tok->sealobject.sopriv;
        pub = &tok->sealobject.sopub;
    } else {
        authsalt = &tok->sealobject.userauthsalt;
        priv = &tok->sealobject.userpriv;
        pub = &tok->sealobject.userpub;
    }

    twist_free(*authsalt);
    twist_free(*priv);

    *authsalt = newsalthex;
    *priv = newprivblob;

    if (newpubblob) {
        twist_free(*pub);
        *pub = newpubblob;
    }
}

CK_RV token_setpin(token *tok, CK_UTF8CHAR_PTR oldpin, CK_ULONG oldlen, CK_UTF8CHAR_PTR newpin, CK_ULONG newlen) {

    CK_RV rv = CKR_GENERAL_ERROR;

    /* new seal auth data */
    twist newsalthex = NULL;
    twist newauthhex = NULL;

    twist newprivblob = NULL;

    /* pin data */
    twist toldpin = NULL;
    twist tnewpin = NULL;

    bool is_so = (tok->login_state == token_so_logged_in);

    toldpin = twistbin_new(oldpin, oldlen);
    if (!toldpin) {
        rv = CKR_HOST_MEMORY;
        goto out;
    }

    tnewpin = twistbin_new(newpin, newlen);
    if (!tnewpin) {
        rv = CKR_HOST_MEMORY;
        goto out;
    }

    /*
     * Step 1 - Generate a new sealing auth value derived from pin and salt
     *
     * This will be used to update the sealobjects table, the columns:
     *  - (so|user)authsalt  --> newsalt
     */
    rv = utils_setup_new_object_auth(tnewpin, &newauthhex, &newsalthex);
    if (rv != CKR_OK) {
        goto out;
    }

    /*
     * Step 2 - Generate the current auth value from oldpin
     */
    twist oldsalt = is_so ? tok->sealobject.soauthsalt : tok->sealobject.userauthsalt;

    twist oldauth = utils_hash_pass(toldpin, oldsalt);
    if (!oldauth) {
        goto out;
    }

    /*
     * Step 3- Call tpm2_changeauth and get a new private object portion
     *
     * This private blob will update table sealobjects (user|so)priv
     */
    rv = tpm_changeauth(tok->tctx, tok->pobject.handle, tok->sealobject.handle,
            oldauth, newauthhex,
            &newprivblob);
    twist_free(oldauth);
    if (rv != CKR_OK) {
        goto out;
    }

    /*
     * Step 5 - load up a new seal object with the new private blob
     */
    twist pubblob = is_so ? tok->sealobject.sopub : tok->sealobject.userpub;

    /* load and update new seal object */
    uint32_t new_seal_handle = 0;
    bool res = tpm_loadobj(tok->tctx, tok->pobject.handle, tok->pobject.objauth,
                pubblob, newprivblob,
                &new_seal_handle);
    if (!res) {
        goto out;
    }

    /*
     * Step X - update the db data
     */
    rv = db_update_for_pinchange(
            tok,
            is_so,

            /* new seal object auth metadata */
            newsalthex,

            /* private and public blobs */
            newprivblob,
            NULL);
    if (rv != CKR_OK) {
        goto out;
    }

    /* TODO: consider calling unload on old seal object handle and WARN on failure */

    /*
     * step 6 - update in-memory metadata for seal object and primary object
     */
    change_token_mem_data(tok, is_so, new_seal_handle, newsalthex, newprivblob, NULL);

    rv = CKR_OK;

out:

    /* If the function failed, then these pointers ARE NOT CLAIMED and must be free'd */
    if (rv != CKR_OK) {
        twist_free(newsalthex);
        twist_free(newprivblob);
    }

    twist_free(newauthhex);

    twist_free(toldpin);
    twist_free(tnewpin);

    return rv;
}

CK_RV token_initpin(token *tok, CK_UTF8CHAR_PTR newpin, CK_ULONG newlen) {

    CK_RV rv = CKR_GENERAL_ERROR;

    twist tnewpin = NULL;

    twist newkeysalthex = NULL;

    twist newsalthex = NULL;
    twist newauthhex = NULL;

    twist newpubblob = NULL;
    twist newprivblob = NULL;

    twist sealdata = NULL;

    tnewpin = twistbin_new(newpin, newlen);
    if (!tnewpin) {
        rv = CKR_HOST_MEMORY;
        goto out;
    }

    /* generate a new auth */
    rv = utils_setup_new_object_auth(tnewpin, &newauthhex, &newsalthex);
    if (rv != CKR_OK) {
        goto out;
    }

    /* we store the seal data in hex form, but it's in binary form in memory, so convert it */
    sealdata = twist_hexlify(tok->wappingkey);
    if (!sealdata) {
        LOGE("oom");
        goto out;
    }

    /* create a new seal object and seal the data */
    uint32_t new_seal_handle = 0;

    rv = tpm2_create_seal_obj(tok->tctx,
            tok->pobject.objauth,
            tok->pobject.handle,
            newauthhex,
            tok->sealobject.userpub,
            sealdata,
            &newpubblob,
            &newprivblob,
            &new_seal_handle);
    if (rv != CKR_OK) {
        goto out;
    }

    /* update the db data */
    rv = db_update_for_pinchange(
            tok,
            false,
            /* new seal object auth metadata */
            newsalthex,

            /* private and public blobs */
            newprivblob,
            newpubblob);
    if (rv != CKR_OK) {
        goto out;
    }

     /* update in-memory metadata for seal object and primary object */
    change_token_mem_data(tok, false, new_seal_handle, newsalthex, newprivblob, newpubblob);

    rv = CKR_OK;

out:

    /* If the function failed, then these pointers ARE NOT CLAIMED and must be free'd */
    if (rv != CKR_OK) {
        twist_free(newkeysalthex);
        twist_free(newsalthex);
        twist_free(newprivblob);
        twist_free(newpubblob);
    }

    twist_free(sealdata);
    twist_free(newauthhex);

    twist_free(tnewpin);

    return rv;
}

CK_RV token_load_object(token *tok, CK_OBJECT_HANDLE key, tobject **loaded_tobj) {

    tpm_ctx *tpm = tok->tctx;

    if (!tok->tobjects) {
        return CKR_KEY_HANDLE_INVALID;
    }

    list *cur = &tok->tobjects->l;
    while(cur) {
        tobject *tobj = list_entry(cur, tobject, l);
        cur = cur->next;
        if (tobj->id != key) {
            continue;
        }

        CK_RV rv = tobject_user_increment(tobj);
        if (rv != CKR_OK) {
            return rv;
        }

        /* this might not be the best place for this check */
        CK_ATTRIBUTE_PTR a = attr_get_attribute_by_type(tobj->attrs, CKA_CLASS);
        if (!a) {
            LOGE("All objects expected to have CKA_CLASS, missing"
                    " for tobj id: %lu", tobj->id);
            return CKR_GENERAL_ERROR;
        }

        CK_OBJECT_CLASS v;
        rv = attr_CK_OBJECT_CLASS(a, &v);
        if (rv != CKR_OK) {
            return rv;
        }

        if (v != CKO_PRIVATE_KEY
                && v != CKO_PUBLIC_KEY
                && v != CKO_SECRET_KEY) {
            LOGE("Cannot use tobj id %lu in a crypto operation", tobj->id);
            return CKR_KEY_HANDLE_INVALID;
        }

        /*
         * The object may already be loaded by the TPM or may just be
         * a public key object not-resident in the TPM.
         */
        if (tobj->handle || !tobj->pub) {
            *loaded_tobj = tobj;
            return CKR_OK;
        }

        bool result = tpm_loadobj(
                tpm,
                tok->pobject.handle, tok->pobject.objauth,
                tobj->pub, tobj->priv,
                &tobj->handle);
        if (!result) {
            return CKR_GENERAL_ERROR;
        }

        rv = utils_ctx_unwrap_objauth(tok, tobj->objauth,
                &tobj->unsealed_auth);
        if (rv != CKR_OK) {
            LOGE("Error unwrapping tertiary object auth");
            return rv;
        }

        *loaded_tobj = tobj;
        return CKR_OK;
    }

    // Found no match on key id
    return CKR_KEY_HANDLE_INVALID;
}

CK_RV token_get_mechanism_list(token *t, CK_MECHANISM_TYPE_PTR mechanism_list, CK_ULONG_PTR count) {

    check_pointer(count);

    /* build a cache of mechanisms */
    static bool is_mech_list_initialized = false;
    static CK_MECHANISM_TYPE mech_list_cache[64];
    static CK_ULONG mech_cache_len = ARRAY_LEN(mech_list_cache);
    if (!is_mech_list_initialized) {
        CK_RV rv = tpm2_getmechanisms(t->tctx, mech_list_cache, &mech_cache_len);
        if (rv != CKR_OK) {
            return rv;
        }
        is_mech_list_initialized = true;
    }

    if (mechanism_list) {

        if (*count < mech_cache_len) {
            *count = mech_cache_len;
            return CKR_BUFFER_TOO_SMALL;
        }

        memcpy(mechanism_list, mech_list_cache,
                ARRAY_BYTES(mech_cache_len, mech_list_cache));
    }

    *count = mech_cache_len;

    return CKR_OK;

}
