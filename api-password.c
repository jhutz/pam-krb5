/*
 * Implements the PAM password group API (pam_sm_chauthtok).
 *
 * Copyright 2011
 *     The Board of Trustees of the Leland Stanford Junior University
 * Copyright 2005, 2006, 2007, 2008, 2009 Russ Allbery <rra@stanford.edu>
 * Copyright 2005 Andres Salomon <dilinger@debian.org>
 * Copyright 1999, 2000 Frank Cusack <fcusack@fcusack.com>
 *
 * See LICENSE for licensing terms.
 */

/* Get declarations for the password functions. */
#define PAM_SM_PASSWORD

#include <config.h>
#include <portable/pam.h>
#include <portable/system.h>

#include <errno.h>

#include <internal.h>
#include <pam-util/args.h>
#include <pam-util/logging.h>


/*
 * The main PAM interface for password changing.
 */
int
pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    struct context *ctx = NULL;
    struct pam_args *args;
    int pamret = PAM_SUCCESS;
    int status;
    PAM_CONST char *user;
    char *pass = NULL;
    bool set_context = false;

    args = pamk5_init(pamh, flags, argc, argv);
    if (args == NULL) {
        pamret = PAM_AUTHTOK_ERR;
        goto done;
    }
    pamret = pamk5_context_fetch(args);
    ENTRY(args, flags);

    /* We only support password changes. */
    if (!(flags & PAM_UPDATE_AUTHTOK) && !(flags & PAM_PRELIM_CHECK)) {
        putil_err(args, "invalid pam_chauthtok flags %d", flags);
        pamret = PAM_AUTHTOK_ERR;
        goto done;
    }

    /*
     * Check whether we should ignore this user.
     *
     * If we do ignore this user, and we're not in the preliminary check
     * phase, still prompt the user for the new password, but suppress our
     * banner.  This is a little strange, but it allows another module to be
     * stacked behind pam-krb5 with use_authtok and have it still work for
     * ignored users.
     *
     * We ignore the return status when prompting for the new password in this
     * case.  The worst thing that can happen is to fail to get the password,
     * in which case the other module will fail (or might even not care).
     */
    if (args->config->ignore_root || args->config->minimum_uid > 0) {
        status = pam_get_user(pamh, &user, NULL);
        if (status == PAM_SUCCESS && pamk5_should_ignore(args, user)) {
            if (flags & PAM_UPDATE_AUTHTOK) {
                if (args->config->banner != NULL) {
                    free(args->config->banner);
                    args->config->banner = NULL;
                }
                pamk5_password_prompt(args, NULL);
            }
            pamret = PAM_IGNORE;
            goto done;
        }
    }

    /*
     * If we weren't able to find an existing context to use, we're going
     * into this fresh and need to create a new context.
     */
    if (args->config->ctx == NULL) {
        pamret = pamk5_context_new(args);
        if (pamret != PAM_SUCCESS) {
            putil_debug_pam(args, pamret, "creating context failed");
            pamret = PAM_AUTHTOK_ERR;
            goto done;
        }
        pamret = pam_set_data(pamh, "pam_krb5", args->config->ctx,
                              pamk5_context_destroy);
        if (pamret != PAM_SUCCESS) {
            putil_err_pam(args, pamret, "cannot set context data");
            pamret = PAM_AUTHTOK_ERR;
            goto done;
        }
        set_context = true;
    }
    ctx = args->config->ctx;

    /*
     * Tell the user what's going on if we're handling an expiration, but not
     * if we were configured to use the same password as an earlier module in
     * the stack.  The correct behavior here is not clear (what if the
     * Kerberos password expired but the other one didn't?), but warning
     * unconditionally leads to a strange message in the middle of doing the
     * password change.
     */
    if (ctx->expired && ctx->creds == NULL)
        if (!args->config->force_first_pass && !args->config->use_first_pass)
            pamk5_conv(args, "Password expired.  You must change it now.",
                       PAM_TEXT_INFO, NULL);

    /*
     * Do the password change.  This may only get tickets if we're doing the
     * preliminary check phase.
     */
    pamret = pamk5_password_change(args, !(flags & PAM_UPDATE_AUTHTOK));
    if (!(flags & PAM_UPDATE_AUTHTOK))
        goto done;

    /*
     * If we were handling a password change for an expired password, now
     * try to get a ticket cache with the new password.
     */
    if (pamret == PAM_SUCCESS && ctx->expired) {
        krb5_creds *creds = NULL;

        putil_debug(args, "obtaining credentials with new password");
        args->config->force_first_pass = 1;
        pamret = pamk5_password_auth(args, NULL, &creds);
        if (pamret != PAM_SUCCESS)
            goto done;
        pamret = pamk5_cache_init_random(args, creds);
    }

done:
    if (pamret != PAM_SUCCESS) {
        if (pamret == PAM_SERVICE_ERR || pamret == PAM_AUTH_ERR)
            pamret = PAM_AUTHTOK_ERR;
        if (pamret == PAM_AUTHINFO_UNAVAIL)
            pamret = PAM_AUTHTOK_ERR;
    }
    EXIT(args, pamret);
    if (pass != NULL) {
        memset(pass, 0, strlen(pass));
        free(pass);
    }

    /*
     * Don't free our Kerberos context if we set a context, since the context
     * will take care of that.
     */
    if (set_context)
        args->ctx = NULL;

    pamk5_free(args);
    return pamret;
}
