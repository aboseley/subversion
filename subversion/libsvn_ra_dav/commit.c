/*
 * commit.c :  routines for committing changes to the server
 *
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */



#include <http_request.h>

#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_uuid.h>

#include "svn_error.h"
#include "svn_delta.h"
#include "svn_ra.h"
#include "svn_wc.h"
#include "svn_path.h"

#include "ra_dav.h"


/*
** resource_t: identify the relevant pieces of a resource on the server
**
** URL refers to the public/viewable/original resource.
** VSN_URL refers to the version resource that we stored locally
** WR_URL refers to a working resource for this resource
**
** Note that VSN_URL is NULL if this resource has just been added, and
** WR_URL can be NULL if the resource has not (yet) been checked out.
*/
typedef struct
{
  const char *url;
  const char *vsn_url;
  const char *wr_url;
} resource_t;

typedef struct
{
  svn_ra_session_t *ras;
  const char *activity_url;
  apr_hash_t *resources;        /* LOCAL PATH (const char *) -> RESOURCE_T */

  /* This is how we pass back the new revision number to our callers. */
  svn_revnum_t *new_revision;

} commit_ctx_t;

typedef struct
{
  commit_ctx_t *cc;
  resource_t res;
} dir_baton_t;

/* ### combine this with dir_baton_t ? */
typedef struct
{
  commit_ctx_t *cc;
  resource_t res;
} file_baton_t;


static svn_error_t *
create_activity (commit_ctx_t *cc)
{
  /* ### damn this is annoying to have to create a string */
  svn_string_t * propname = svn_string_create(SVN_RA_DAV__LP_ACTIVITY_URL,
                                              cc->ras->pool);

  /* ### what the hell to use for the path? and where to get it */
  svn_string_t * path = svn_string_create(".", cc->ras->pool);

  svn_string_t * activity_url;
  apr_uuid_t uuid;
  char uuid_buf[APR_UUID_FORMATTED_LENGTH];
  svn_string_t uuid_str = { uuid_buf, sizeof(uuid_buf), 0, NULL };
  int rv;
  http_req *req;
  http_status hstat;

  /* get the URL where we should create activities */
  SVN_ERR( svn_wc_prop_get(&activity_url, propname, path, cc->ras->pool) );

  /* the URL for our activity will be ACTIVITY_URL/UUID */
  apr_get_uuid(&uuid);
  apr_format_uuid(uuid_buf, &uuid);

  svn_path_add_component(activity_url, &uuid_str, svn_path_url_style);

  cc->activity_url = activity_url->data;

  /* create/prep the MKACTIVITY request */
  req = http_request_create(cc->ras->sess, "MKACTIVITY", cc->activity_url);
  if (req == NULL)
    {
      return svn_error_create(SVN_ERR_RA_CREATING_REQUEST, 0, NULL,
                              cc->ras->pool,
                              "Could not create the MKACTIVITY request");
    }

  /* run the request and get the resulting status code. */
  rv = http_request_dispatch(req, &hstat);
  if (hstat.code != 201)
    {
      /* ### need to be more sophisticated with reporting the failure */
      return svn_error_create(SVN_ERR_RA_MKACTIVITY_FAILED, 0, NULL,
                              cc->ras->pool,
                              "The MKACTIVITY request failed.");
    }

  http_request_destroy(req);

  return NULL;
}

#if 0  /* With -Wall, we keep getting a warning that this is defined
          but not used.  It aids Emacs reflexes to have no warnings at
          all, so #if this out until it's actually used.  -kff*/  
static svn_error_t *
checkout_resource (commit_ctx_t *cc, const char *src_url, const char **wr_url)
{
  /* ### examine cc->workrsrc -- we may already have a WR */
  return NULL;
}
#endif /* 0 */

static svn_error_t *
commit_replace_root (void *edit_baton, void **root_baton)
{
  commit_ctx_t *cc = edit_baton;
  dir_baton_t *root = apr_pcalloc(cc->ras->pool, sizeof(*root));

  root->cc = cc;
  root->res.url = cc->ras->root.path;

  /* ### fetch vsn_url from props */

  *root_baton = root;

  return NULL;
}

static svn_error_t *
commit_delete (svn_string_t *name, void *parent_baton)
{
  dir_baton_t *parent = parent_baton;

  /* ### CHECKOUT parent collection, then DELETE */
  printf("[delete] CHECKOUT: %s\n[delete] DELETE: %s/%s\n",
         parent->res.url, parent->res.url, name->data);

  return NULL;
}

static svn_error_t *
commit_add_dir (svn_string_t *name,
                void *parent_baton,
                svn_string_t *ancestor_path,
                svn_revnum_t ancestor_revision,
                void **child_baton)
{
  dir_baton_t *parent = parent_baton;
  dir_baton_t *child = apr_pcalloc(parent->cc->ras->pool, sizeof(*child));

  child->cc = parent->cc;
  child->res.url = apr_pstrcat(child->cc->ras->pool, parent->res.url,
                               "/", name->data, NULL);

  /* ### CHECKOUT parent, then: if ancestor: COPY; if no ancestor: MKCOL */

  printf("[add_dir] CHECKOUT: %s\n[add_dir] MKCOL: %s\n",
         parent->res.url, child->res.url);

  *child_baton = child;
  return NULL;
}

static svn_error_t *
commit_rep_dir (svn_string_t *name,
                void *parent_baton,
                svn_string_t *ancestor_path,
                svn_revnum_t ancestor_revision,
                void **child_baton)
{
  dir_baton_t *parent = parent_baton;
  dir_baton_t *child = apr_pcalloc(parent->cc->ras->pool, sizeof(*child));

  child->cc = parent->cc;
  child->res.url = apr_pstrcat(child->cc->ras->pool, parent->res.url,
                               "/", name->data, NULL);

  /* ### if replacing with ancestor of something else, then CHECKOUT target
     ### and COPY ancestor over the target (Overwrite: update)
     ### replace w/o an ancestor is just a signal for change within the
     ### dir and we do nothing
  */

  printf("[rep_dir] CHECKOUT: %s\n[rep_dir] COPY: %s -> %s\n",
         parent->res.url, ancestor_path->data, child->res.url);

  *child_baton = child;
  return NULL;
}

static svn_error_t *
commit_change_dir_prop (void *dir_baton,
                        svn_string_t *name,
                        svn_string_t *value)
{
  dir_baton_t *dir = dir_baton;

  /* ### CHECKOUT, then PROPPATCH */

  printf("[change_dir_prop] CHECKOUT: %s\n"
         "[change_dir_prop] PROPPATCH: %s (%s=%s)\n",
         dir->res.url, dir->res.url, name->data, value->data);

  return NULL;
}

static svn_error_t *
commit_close_dir (void *dir_baton)
{
  /* ### nothing? */
  return NULL;
}

static svn_error_t *
commit_add_file (svn_string_t *name,
                 void *parent_baton,
                 svn_string_t *ancestor_path,
                 svn_revnum_t ancestor_revision,
                 void **file_baton)
{
  dir_baton_t *parent = parent_baton;
  file_baton_t *file = apr_pcalloc(parent->cc->ras->pool, sizeof(*file));

  file->cc = parent->cc;
  file->res.url = apr_pstrcat(file->cc->ras->pool, parent->res.url,
                              "/", name->data, NULL);
  /* ### store name, parent? */

  /* ### CHECKOUT parent (then PUT in apply_txdelta) */
  printf("[add_file] CHECKOUT: %s\n", file->res.url);

  /* ### begin the PUT here? */

  *file_baton = file;
  return NULL;
}

static svn_error_t *
commit_rep_file (svn_string_t *name,
                 void *parent_baton,
                 svn_string_t *ancestor_path,
                 svn_revnum_t ancestor_revision,
                 void **file_baton)
{
  dir_baton_t *parent = parent_baton;
  file_baton_t *file = apr_pcalloc(parent->cc->ras->pool, sizeof(*file));

  file->cc = parent->cc;
  file->res.url = apr_pstrcat(file->cc->ras->pool, parent->res.url,
                              "/", name->data, NULL);
  /* ### store more info? */

  /* ### CHECKOUT (then PUT in apply_txdelta) */
  /* ### if replacing with a specific ancestor, then COPY */
  printf("[rep_file] CHECKOUT: %s\n", file->res.url);

  /* ### begin the PUT/COPY here? */

  *file_baton = file;
  return NULL;
}

static svn_error_t * commit_send_txdelta(svn_txdelta_window_t *window,
                                         void *baton)
{
  return NULL;
}

static svn_error_t *
commit_apply_txdelta (void *file_baton, 
                      svn_txdelta_window_handler_t **handler,
                      void **handler_baton)
{
  file_baton_t *file = file_baton;

  /* ### PUT */
  printf("[apply_txdelta] PUT: %s\n", file->res.url);

  *handler = commit_send_txdelta;
  *handler_baton = NULL;

  return NULL;
}

static svn_error_t *
commit_change_file_prop (void *file_baton,
                         svn_string_t *name,
                         svn_string_t *value)
{
  file_baton_t *file = file_baton;

  /* ### CHECKOUT, then PROPPATCH */

  printf("[change_file_prop] CHECKOUT: %s\n"
         "[change_file_prop] PROPPATCH: %s (%s=%s)\n",
         file->res.url, file->res.url, name->data, value->data);

  return NULL;
}

static svn_error_t *
commit_close_file (void *file_baton)
{
  /* ### nothing? */
  return NULL;
}

static svn_error_t *
commit_close_edit (void *edit_baton)
{
  commit_ctx_t *cc = edit_baton;
  svn_revnum_t new_revision = SVN_INVALID_REVNUM;

  /* ### CHECKIN the activity */
  printf("[close_edit] CHECKIN: %s\n",
         cc->activity_url ? cc->activity_url : "(activity)");

  /* todo: set new_revision according to response from server. */

  /* Make sure the caller (most likely the working copy library, or
     maybe its caller) knows the new revision. */
  *cc->new_revision = new_revision;

  return NULL;
}

/*
** This structure is used during the commit process. An external caller
** uses these callbacks to describe all the changes in the working copy
** that must be committed to the server.
*/
static const svn_delta_edit_fns_t commit_editor = {
  commit_replace_root,
  commit_delete,
  commit_add_dir,
  commit_rep_dir,
  commit_change_dir_prop,
  commit_close_dir,
  commit_add_file,
  commit_rep_file,
  commit_apply_txdelta,
  commit_change_file_prop,
  commit_close_file,
  commit_close_edit
};

svn_error_t * svn_ra_dav__get_commit_editor(
  void *session_baton,
  const svn_delta_edit_fns_t **editor,
  void **edit_baton,
  svn_revnum_t *new_revision)
{
  svn_ra_session_t *ras = session_baton;
  commit_ctx_t *cc = apr_pcalloc(ras->pool, sizeof(*cc));
  svn_error_t *err;

  cc->ras = ras;
  cc->resources = apr_make_hash(ras->pool);

  /* ### disable for now */
  err = 0 && create_activity(cc);
  if (err)
    return err;

  /* Record where the caller wants the new revision number stored. */
  cc->new_revision = new_revision;

  *edit_baton = cc;

  *editor = &commit_editor;

  return NULL;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
