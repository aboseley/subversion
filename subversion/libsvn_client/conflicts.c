/*
 * conflicts.c:  conflict resolver implementation
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

/* ==================================================================== */



/*** Includes. ***/

#include "svn_types.h"
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_props.h"
#include "svn_hash.h"
#include "svn_sorts.h"
#include "client.h"
#include "private/svn_sorts_private.h"
#include "private/svn_token.h"
#include "private/svn_wc_private.h"

#include "svn_private_config.h"

#define ARRAY_LEN(ary) ((sizeof (ary)) / (sizeof ((ary)[0])))


/*** Dealing with conflicts. ***/

/* Describe a tree conflict. */
typedef svn_error_t *(*tree_conflict_get_description_func_t)(
  const char **description,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool);

/* Get more information about a tree conflict.
 * This function may contact the repository. */
typedef svn_error_t *(*tree_conflict_get_details_func_t)(
  svn_client_conflict_t *conflict,
  apr_pool_t *scratch_pool);

struct svn_client_conflict_t
{
  const char *local_abspath;
  svn_client_ctx_t *ctx;
  apr_hash_t *prop_conflicts;

  /* Indicate which options were chosen to resolve a text or tree conflict
   * on the conflited node. */
  svn_client_conflict_option_id_t resolution_text;
  svn_client_conflict_option_id_t resolution_tree;

  /* A mapping from const char* property name to pointers to
   * svn_client_conflict_option_t for all properties which had their
   * conflicts resolved. Indicates which options were chosen to resolve
   * the property conflicts. */
  apr_hash_t *resolved_props;

  /* Ask a tree conflict to describe itself. */
  tree_conflict_get_description_func_t tree_conflict_get_description_func;

  /* Ask a tree conflict to find out more information about itself
   * by contacting the repository. */
  tree_conflict_get_details_func_t tree_conflict_get_details_func;

  /* Any additional information found can be stored here and may be used
   * when describing a tree conflict. */
  void *tree_conflict_details;

  /* The pool this conflict was allocated from. */
  apr_pool_t *pool;

  /* Conflict data provided by libsvn_wc. */
  const svn_wc_conflict_description2_t *legacy_text_conflict;
  const char *legacy_prop_conflict_propname;
  const svn_wc_conflict_description2_t *legacy_tree_conflict;
};

/* Resolves conflict to OPTION and sets CONFLICT->RESOLUTION accordingly.
 *
 * May raise an error in case the conflict could not be resolved. A common
 * case would be a tree conflict the resolution of which depends on other
 * tree conflicts to be resolved first. */
typedef svn_error_t *(*conflict_option_resolve_func_t)(
  svn_client_conflict_option_t *option,
  svn_client_conflict_t *conflict,
  apr_pool_t *scratch_pool);

struct svn_client_conflict_option_t
{
  svn_client_conflict_option_id_t id;
  const char *description;

  svn_client_conflict_t *conflict;
  conflict_option_resolve_func_t do_resolve_func;

  /* Data which is specific to particular conflicts and options. */
  union {
    struct {
      /* Indicates the property to resolve in case of a property conflict.
       * If set to "", all properties are resolved to this option. */
      const char *propname;

      /* A merged property value, if supplied by the API user, else NULL. */
      const svn_string_t *merged_propval;
    } prop;
  } type_data;

};

/*
 * Return a legacy conflict choice corresponding to OPTION_ID.
 * Return svn_wc_conflict_choose_undefined if no corresponding
 * legacy conflict choice exists.
 */
static svn_wc_conflict_choice_t
conflict_option_id_to_wc_conflict_choice(
  svn_client_conflict_option_id_t option_id)
{

  switch (option_id)
    {
      case svn_client_conflict_option_undefined:
        return svn_wc_conflict_choose_undefined;

      case svn_client_conflict_option_postpone:
        return svn_wc_conflict_choose_postpone;

      case svn_client_conflict_option_base_text:
        return svn_wc_conflict_choose_base;

      case svn_client_conflict_option_incoming_text:
        return svn_wc_conflict_choose_theirs_full;

      case svn_client_conflict_option_working_text:
        return svn_wc_conflict_choose_mine_full;

      case svn_client_conflict_option_incoming_text_where_conflicted:
        return svn_wc_conflict_choose_theirs_conflict;

      case svn_client_conflict_option_working_text_where_conflicted:
        return svn_wc_conflict_choose_mine_conflict;

      case svn_client_conflict_option_merged_text:
        return svn_wc_conflict_choose_merged;

      case svn_client_conflict_option_unspecified:
        return svn_wc_conflict_choose_unspecified;

      default:
        break;
    }

  return svn_wc_conflict_choose_undefined;
}

static void
add_legacy_desc_to_conflict(const svn_wc_conflict_description2_t *desc,
                            svn_client_conflict_t *conflict,
                            apr_pool_t *result_pool)
{
  switch (desc->kind)
    {
      case svn_wc_conflict_kind_text:
        conflict->legacy_text_conflict = desc;
        break;

      case svn_wc_conflict_kind_property:
        if (conflict->prop_conflicts == NULL)
          conflict->prop_conflicts = apr_hash_make(result_pool);
        svn_hash_sets(conflict->prop_conflicts, desc->property_name, desc);
        conflict->legacy_prop_conflict_propname = desc->property_name;
        break;

      case svn_wc_conflict_kind_tree:
        conflict->legacy_tree_conflict = desc;
        break;

      default:
        SVN_ERR_ASSERT_NO_RETURN(FALSE); /* unknown kind of conflict */
    }
}

/* ### forward declarations */
static svn_error_t *
conflict_tree_get_description_generic(const char **description,
                                      svn_client_conflict_t *conflict,
                                      apr_pool_t *result_pool,
                                      apr_pool_t *scratch_pool);
static svn_error_t *
conflict_tree_get_description_incoming_delete(const char **description,
                                              svn_client_conflict_t *conflict,
                                              apr_pool_t *result_pool,
                                              apr_pool_t *scratch_pool);
static svn_error_t *
conflict_tree_get_details_incoming_delete(svn_client_conflict_t *conflict,
                                          apr_pool_t *scratch_pool);

/* Set up type-specific data for a new conflict object. */
static svn_error_t *
conflict_type_specific_setup(svn_client_conflict_t *conflict,
                             apr_pool_t *scratch_pool)
{
  svn_boolean_t tree_conflicted;
  svn_wc_operation_t operation;
  svn_wc_conflict_reason_t local_change;
  svn_wc_conflict_action_t incoming_change;

  /* For now, we only deal with tree conflicts here. */
  SVN_ERR(svn_client_conflict_get_conflicted(NULL, NULL, &tree_conflicted,
                                             conflict, scratch_pool,
                                             scratch_pool));
  if (!tree_conflicted)
    return SVN_NO_ERROR;

  /* Set a default description function. */
  conflict->tree_conflict_get_description_func =
    conflict_tree_get_description_generic;

  operation = svn_client_conflict_get_operation(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);

  /* Set type-specific description and details functions if available. */
  if (incoming_change == svn_wc_conflict_action_delete &&
      (operation == svn_wc_operation_update ||
       operation == svn_wc_operation_switch) /* ### TODO: merge */)
    {
      conflict->tree_conflict_get_description_func =
        conflict_tree_get_description_incoming_delete;
      conflict->tree_conflict_get_details_func =
        conflict_tree_get_details_incoming_delete;
    }

  return SVN_NO_ERROR;
}

/* Set up a conflict object. If legacy conflict descriptor DESC is not NULL,
 * set up the conflict object for backwards compatibility. */
static svn_error_t *
conflict_get_internal(svn_client_conflict_t **conflict,
                      const char *local_abspath,
                      const svn_wc_conflict_description2_t *desc,
                      svn_client_ctx_t *ctx,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  const apr_array_header_t *descs;
  int i;

  *conflict = apr_pcalloc(result_pool, sizeof(**conflict));

  if (desc)
    {
      /* Add a single legacy conflict descriptor. */
      (*conflict)->local_abspath = desc->local_abspath;
      (*conflict)->resolution_text = svn_client_conflict_option_unspecified;
      (*conflict)->resolution_tree = svn_client_conflict_option_unspecified;
      (*conflict)->resolved_props = apr_hash_make(result_pool);
      add_legacy_desc_to_conflict(desc, *conflict, result_pool);

      return SVN_NO_ERROR;
    }

  (*conflict)->local_abspath = apr_pstrdup(result_pool, local_abspath);
  (*conflict)->resolution_text = svn_client_conflict_option_unspecified;
  (*conflict)->resolution_tree = svn_client_conflict_option_unspecified;
  (*conflict)->resolved_props = apr_hash_make(result_pool);
  (*conflict)->ctx = ctx;
  (*conflict)->pool = result_pool;

  /* Add all legacy conflict descriptors we can find. Eventually, this code
   * path should stop relying on svn_wc_conflict_description2_t entirely. */
  SVN_ERR(svn_wc__read_conflict_descriptions2_t(&descs, ctx->wc_ctx,
                                                local_abspath,
                                                result_pool, scratch_pool));
  for (i = 0; i < descs->nelts; i++)
    {
      desc = APR_ARRAY_IDX(descs, i, const svn_wc_conflict_description2_t *);
      add_legacy_desc_to_conflict(desc, *conflict, result_pool);
    }

  SVN_ERR(conflict_type_specific_setup(*conflict, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_get(svn_client_conflict_t **conflict,
                        const char *local_abspath,
                        svn_client_ctx_t *ctx,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool)
{
  return svn_error_trace(conflict_get_internal(conflict, local_abspath, NULL,
                                               ctx, result_pool, scratch_pool));
}

/* A map for svn_wc_conflict_action_t values to strings */
static const svn_token_map_t map_conflict_action[] =
{
  { "edit",             svn_wc_conflict_action_edit },
  { "delete",           svn_wc_conflict_action_delete },
  { "add",              svn_wc_conflict_action_add },
  { "replace",          svn_wc_conflict_action_replace },
  { NULL,               0 }
};

/* A map for svn_wc_conflict_reason_t values to strings */
static const svn_token_map_t map_conflict_reason[] =
{
  { "edit",             svn_wc_conflict_reason_edited },
  { "delete",           svn_wc_conflict_reason_deleted },
  { "missing",          svn_wc_conflict_reason_missing },
  { "obstruction",      svn_wc_conflict_reason_obstructed },
  { "add",              svn_wc_conflict_reason_added },
  { "replace",          svn_wc_conflict_reason_replaced },
  { "unversioned",      svn_wc_conflict_reason_unversioned },
  { "moved-away",       svn_wc_conflict_reason_moved_away },
  { "moved-here",       svn_wc_conflict_reason_moved_here },
  { NULL,               0 }
};

/* Return a localised string representation of the local part of a conflict;
   NULL for non-localised odd cases. */
static const char *
local_reason_str(svn_node_kind_t kind, svn_wc_conflict_reason_t reason,
                 svn_wc_operation_t operation)
{
  switch (kind)
    {
      case svn_node_file:
      case svn_node_symlink:
        switch (reason)
          {
          case svn_wc_conflict_reason_edited:
            return _("local file edit");
          case svn_wc_conflict_reason_obstructed:
            return _("local file obstruction");
          case svn_wc_conflict_reason_deleted:
            return _("local file delete");
          case svn_wc_conflict_reason_missing:
            if (operation == svn_wc_operation_merge)
              return _("local file missing or deleted or moved away");
            else
              return _("local file missing");
          case svn_wc_conflict_reason_unversioned:
            return _("local file unversioned");
          case svn_wc_conflict_reason_added:
            return _("local file add");
          case svn_wc_conflict_reason_replaced:
            return _("local file replace");
          case svn_wc_conflict_reason_moved_away:
            return _("local file moved away");
          case svn_wc_conflict_reason_moved_here:
            return _("local file moved here");
          }
        break;
      case svn_node_dir:
        switch (reason)
          {
          case svn_wc_conflict_reason_edited:
            return _("local dir edit");
          case svn_wc_conflict_reason_obstructed:
            return _("local dir obstruction");
          case svn_wc_conflict_reason_deleted:
            return _("local dir delete");
          case svn_wc_conflict_reason_missing:
            if (operation == svn_wc_operation_merge)
              return _("local dir missing or deleted or moved away");
            else
              return _("local dir missing");
          case svn_wc_conflict_reason_unversioned:
            return _("local dir unversioned");
          case svn_wc_conflict_reason_added:
            return _("local dir add");
          case svn_wc_conflict_reason_replaced:
            return _("local dir replace");
          case svn_wc_conflict_reason_moved_away:
            return _("local dir moved away");
          case svn_wc_conflict_reason_moved_here:
            return _("local dir moved here");
          }
        break;
      case svn_node_none:
      case svn_node_unknown:
        switch (reason)
          {
          case svn_wc_conflict_reason_edited:
            return _("local edit");
          case svn_wc_conflict_reason_obstructed:
            return _("local obstruction");
          case svn_wc_conflict_reason_deleted:
            return _("local delete");
          case svn_wc_conflict_reason_missing:
            if (operation == svn_wc_operation_merge)
              return _("local missing or deleted or moved away");
            else
              return _("local missing");
          case svn_wc_conflict_reason_unversioned:
            return _("local unversioned");
          case svn_wc_conflict_reason_added:
            return _("local add");
          case svn_wc_conflict_reason_replaced:
            return _("local replace");
          case svn_wc_conflict_reason_moved_away:
            return _("local moved away");
          case svn_wc_conflict_reason_moved_here:
            return _("local moved here");
          }
        break;
    }
  return NULL;
}

/* Return a localised string representation of the incoming part of a
   conflict; NULL for non-localised odd cases. */
static const char *
incoming_action_str(svn_node_kind_t kind, svn_wc_conflict_action_t action)
{
  switch (kind)
    {
      case svn_node_file:
      case svn_node_symlink:
        switch (action)
          {
            case svn_wc_conflict_action_edit:
              return _("incoming file edit");
            case svn_wc_conflict_action_add:
              return _("incoming file add");
            case svn_wc_conflict_action_delete:
              return _("incoming file delete or move");
            case svn_wc_conflict_action_replace:
              return _("incoming replace with file");
          }
        break;
      case svn_node_dir:
        switch (action)
          {
            case svn_wc_conflict_action_edit:
              return _("incoming dir edit");
            case svn_wc_conflict_action_add:
              return _("incoming dir add");
            case svn_wc_conflict_action_delete:
              return _("incoming dir delete or move");
            case svn_wc_conflict_action_replace:
              return _("incoming replace with dir");
          }
        break;
      case svn_node_none:
      case svn_node_unknown:
        switch (action)
          {
            case svn_wc_conflict_action_edit:
              return _("incoming edit");
            case svn_wc_conflict_action_add:
              return _("incoming add");
            case svn_wc_conflict_action_delete:
              return _("incoming delete or move");
            case svn_wc_conflict_action_replace:
              return _("incoming replace");
          }
        break;
    }
  return NULL;
}

/* Return a localised string representation of the operation part of a
   conflict. */
static const char *
operation_str(svn_wc_operation_t operation)
{
  switch (operation)
    {
    case svn_wc_operation_update: return _("upon update");
    case svn_wc_operation_switch: return _("upon switch");
    case svn_wc_operation_merge:  return _("upon merge");
    case svn_wc_operation_none:   return _("upon none");
    }
  SVN_ERR_MALFUNCTION_NO_RETURN();
  return NULL;
}

svn_error_t *
svn_client_conflict_prop_get_description(const char **description,
                                         svn_client_conflict_t *conflict,
                                         apr_pool_t *result_pool,
                                         apr_pool_t *scratch_pool)
{
  const char *reason_str, *action_str;

  /* We provide separately translatable strings for the values that we
   * know about, and a fall-back in case any other values occur. */
  switch (svn_client_conflict_get_local_change(conflict))
    {
      case svn_wc_conflict_reason_edited:
        reason_str = _("local edit");
        break;
      case svn_wc_conflict_reason_added:
        reason_str = _("local add");
        break;
      case svn_wc_conflict_reason_deleted:
        reason_str = _("local delete");
        break;
      case svn_wc_conflict_reason_obstructed:
        reason_str = _("local obstruction");
        break;
      default:
        reason_str = apr_psprintf(
                       scratch_pool, _("local %s"),
                       svn_token__to_word(
                         map_conflict_reason,
                         svn_client_conflict_get_local_change(conflict)));
        break;
    }
  switch (svn_client_conflict_get_incoming_change(conflict))
    {
      case svn_wc_conflict_action_edit:
        action_str = _("incoming edit");
        break;
      case svn_wc_conflict_action_add:
        action_str = _("incoming add");
        break;
      case svn_wc_conflict_action_delete:
        action_str = _("incoming delete");
        break;
      default:
        action_str = apr_psprintf(
                       scratch_pool, _("incoming %s"),
                       svn_token__to_word(
                         map_conflict_action,
                         svn_client_conflict_get_incoming_change(conflict)));
        break;
    }
  SVN_ERR_ASSERT(reason_str && action_str);

  *description = apr_psprintf(result_pool, _("%s, %s %s"),
                              reason_str, action_str,
                              operation_str(
                                svn_client_conflict_get_operation(conflict)));

  return SVN_NO_ERROR;
}

/* Implements tree_conflict_get_description_func_t. */
static svn_error_t *
conflict_tree_get_description_generic(const char **description,
                                      svn_client_conflict_t *conflict,
                                      apr_pool_t *result_pool,
                                      apr_pool_t *scratch_pool)
{
  const char *action, *reason, *operation;
  svn_node_kind_t incoming_kind;
  svn_wc_conflict_action_t conflict_action;
  svn_wc_conflict_reason_t conflict_reason;
  svn_wc_operation_t conflict_operation;
  svn_node_kind_t conflict_node_kind;

  conflict_action = svn_client_conflict_get_incoming_change(conflict);
  conflict_reason = svn_client_conflict_get_local_change(conflict);
  conflict_operation = svn_client_conflict_get_operation(conflict);
  conflict_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);

  /* Determine the node kind of the incoming change. */
  incoming_kind = svn_node_unknown;
  if (conflict_action == svn_wc_conflict_action_edit ||
      conflict_action == svn_wc_conflict_action_delete)
    {
      /* Change is acting on 'src_left' version of the node. */
      SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
                NULL, NULL, &incoming_kind, conflict, scratch_pool,
                scratch_pool));
    }
  else if (conflict_action == svn_wc_conflict_action_add ||
           conflict_action == svn_wc_conflict_action_replace)
    {
      /* Change is acting on 'src_right' version of the node.
       *
       * ### For 'replace', the node kind is ambiguous. However, src_left
       * ### is NULL for replace, so we must use src_right. */
      SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
                NULL, NULL, &incoming_kind, conflict, scratch_pool,
                scratch_pool));
    }

  reason = local_reason_str(conflict_node_kind, conflict_reason,
                            conflict_operation);
  action = incoming_action_str(incoming_kind, conflict_action);
  operation = operation_str(conflict_operation);
  SVN_ERR_ASSERT(operation);

  if (action && reason)
    {
      *description = apr_psprintf(result_pool, _("%s, %s %s"),
                                  reason, action, operation);
    }
  else
    {
      /* A catch-all message for very rare or nominally impossible cases.
         It will not be pretty, but is closer to an internal error than
         an ordinary user-facing string. */
      *description = apr_psprintf(result_pool,
                                  _("local: %s %s incoming: %s %s %s"),
                                  svn_node_kind_to_word(conflict_node_kind),
                                  svn_token__to_word(map_conflict_reason,
                                                     conflict_reason),
                                  svn_node_kind_to_word(incoming_kind),
                                  svn_token__to_word(map_conflict_action,
                                                     conflict_action),
                                  operation);
    }
  return SVN_NO_ERROR;
}

/* Details for tree conflicts involving incoming deletions. */
struct conflict_tree_incoming_delete_details
{
  /* If not SVN_INVALID_REVNUM, the node was deleted in DELETED_REV. */
  svn_revnum_t deleted_rev;

  /* If not SVN_INVALID_REVNUM, the node was added in ADDED_REV. The incoming
   * delete is the result of a reverse application of this addition. */
  svn_revnum_t added_rev;

  /* The path which was deleted/added relative to the repository root. */
  const char *repos_relpath;

  /* Author who committed DELETED_REV/ADDED_REV. */
  const char *rev_author;
};

/* Implements tree_conflict_get_description_func_t. */
static svn_error_t *
conflict_tree_get_description_incoming_delete(const char **description,
                                              svn_client_conflict_t *conflict,
                                              apr_pool_t *result_pool,
                                              apr_pool_t *scratch_pool)
{
  const char *action, *reason;
  svn_node_kind_t victim_node_kind;
  svn_wc_conflict_action_t incoming_change;
  svn_wc_conflict_reason_t local_change;
  svn_wc_operation_t conflict_operation;
  const char *old_repos_relpath;
  svn_revnum_t old_rev;
  const char *new_repos_relpath;
  svn_revnum_t new_rev;
  struct conflict_tree_incoming_delete_details *details;

  if (conflict->tree_conflict_details == NULL)
    return svn_error_trace(conflict_tree_get_description_generic(description,
                                                                 conflict,
                                                                 result_pool,
                                                                 scratch_pool));

  incoming_change = svn_client_conflict_get_incoming_change(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  conflict_operation = svn_client_conflict_get_operation(conflict);
  victim_node_kind = svn_client_conflict_tree_get_victim_node_kind(conflict);
  reason = local_reason_str(victim_node_kind, local_change, conflict_operation);
  if (reason == NULL)
    return svn_error_trace(conflict_tree_get_description_generic(description,
                                                                 conflict,
                                                                 result_pool,
                                                                 scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
            &old_repos_relpath, &old_rev, NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &new_repos_relpath, &new_rev, NULL, conflict, scratch_pool,
            scratch_pool));

  details = conflict->tree_conflict_details;

  if (conflict_operation == svn_wc_operation_update)
    {
      if (details->deleted_rev != SVN_INVALID_REVNUM)
        {
          if (victim_node_kind == svn_node_dir)
            action = apr_psprintf(result_pool,
                                  _("dir updated to r%lu was deleted or moved "
                                    "by %s in r%lu"), new_rev,
                                  details->rev_author, details->deleted_rev);
          else if (victim_node_kind == svn_node_file ||
                   victim_node_kind == svn_node_symlink)
            action = apr_psprintf(result_pool,
                                  _("file updated to r%lu was deleted or moved "
                                    "by %s in r%lu"), new_rev,
                                  details->rev_author, details->deleted_rev);
          else
            action = apr_psprintf(result_pool,
                                  _("item updated to r%lu was deleted or moved "
                                    "by %s in r%lu"), new_rev,
                                  details->rev_author, details->deleted_rev);
        }
      else /* details->added_rev != SVN_INVALID_REVNUM */
        {
          /* This deletion is really the reverse change of an addition. */
          if (victim_node_kind == svn_node_dir)
            action = apr_psprintf(result_pool,
                                  _("dir updated to r%lu did not exist before "
                                    "it was added by %s in r%lu"), new_rev,
                                  details->rev_author, details->added_rev);
          else if (victim_node_kind == svn_node_file ||
                   victim_node_kind == svn_node_symlink)
            action = apr_psprintf(result_pool,
                                  _("file updated to r%lu did not exist before "
                                    "it was added by %s in r%lu"), new_rev,
                                  details->rev_author, details->added_rev);
          else
            action = apr_psprintf(result_pool,
                                  _("item updated to r%lu did not exist before "
                                    "it was added by %s in r%lu"), new_rev,
                                  details->rev_author, details->added_rev);
        }
    }
  else if (conflict_operation == svn_wc_operation_switch)
    {
      if (details->deleted_rev != SVN_INVALID_REVNUM)
        {
          if (victim_node_kind == svn_node_dir)
            action = apr_psprintf(result_pool,
                                  _("dir switched from %s@r%lu to %s@r%lu was "
                                    "deleted or moved by %s in r%lu"),
                                  old_repos_relpath, old_rev,
                                  new_repos_relpath, new_rev,
                                  details->rev_author, details->deleted_rev);
          else if (victim_node_kind == svn_node_file ||
                   victim_node_kind == svn_node_symlink)
            action = apr_psprintf(result_pool,
                                  _("file switched from %s@r%lu to %s@r%lu was "
                                    "deleted or moved by %s in r%lu"),
                                  old_repos_relpath, old_rev,
                                  new_repos_relpath, new_rev,
                                  details->rev_author, details->deleted_rev);
          else
            action = apr_psprintf(result_pool,
                                  _("item switched from %s@r%lu to %s@r%lu was "
                                    "deleted or moved by %s in r%lu"),
                                  old_repos_relpath, old_rev,
                                  new_repos_relpath, new_rev,
                                  details->rev_author, details->deleted_rev);
        }
      else /* details->added_rev != SVN_INVALID_REVNUM */
        {
          /* This deletion is really the reverse change of an addition. */
          if (victim_node_kind == svn_node_dir)
            action = apr_psprintf(result_pool,
                                  _("dir switched from %s@r%lu to %s@r%lu did "
                                    "not exist before it was added by %s in "
                                    "r%lu"),
                                  old_repos_relpath, old_rev,
                                  new_repos_relpath, new_rev,
                                  details->rev_author, details->added_rev);
          else if (victim_node_kind == svn_node_file ||
                   victim_node_kind == svn_node_symlink)
            action = apr_psprintf(result_pool,
                                  _("file switched from %s@r%lu to %s@r%lu did "
                                    "not exist before it was added by %s in "
                                    "r%lu"),
                                  old_repos_relpath, old_rev,
                                  new_repos_relpath, new_rev,
                                  details->rev_author, details->added_rev);
          else
            action = apr_psprintf(result_pool,
                                  _("item switched from %s@r%lu to %s@r%lu did "
                                    "not exist before it was added by %s in "
                                    "r%lu"),
                                  old_repos_relpath, old_rev,
                                  new_repos_relpath, new_rev,
                                  details->rev_author, details->added_rev);
          }
      }

  *description = apr_psprintf(result_pool, _("%s, %s"), reason, action);
  return SVN_NO_ERROR;
}

/* Baton for find_added_rev(). */
struct find_added_rev_baton
{
  struct conflict_tree_incoming_delete_details *details;
  apr_pool_t *pool;
};

/* Implements svn_location_segment_receiver_t.
 * Finds the revision in which a node was added by tracing 'start'
 * revisions in location segments reported for the node. */
static svn_error_t *
find_added_rev(svn_location_segment_t *segment,
               void *baton,
               apr_pool_t *scratch_pool)
{
  struct find_added_rev_baton *b = baton;

  if (segment->path) /* not interested in gaps */
    {
      b->details->added_rev = segment->range_start;
      b->details->repos_relpath = apr_pstrdup(b->pool, segment->path);
    }

  return SVN_NO_ERROR;
}

struct find_deleted_rev_baton
{
  const char *deleted_repos_relpath;
  const char *related_repos_relpath;
  svn_revnum_t related_repos_peg_rev;

  svn_revnum_t deleted_rev;

  const char *repos_root_url;
  const char *repos_uuid;
  svn_client_ctx_t *ctx;
};

/* Implements svn_log_entry_receiver_t.
 *
 * Find the revision in which a node, ancestrally related to the node
 * specified via find_deleted_rev_baton, was deleted, When the revision
 * was found, store it in BATON->DELETED_REV and abort the log operation
 * by raising SVN_ERR_CANCELLED.
 *
 * If no such revision can be found, leave BATON->DELETED_REV alone.
 *
 * This function answers the same question as svn_ra_get_deleted_rev() but
 * works in cases where we do not already know a revision in which the deleted
 * node once used to exist. */
static svn_error_t *
find_deleted_rev(void *baton,
                 svn_log_entry_t *log_entry,
                 apr_pool_t *scratch_pool)
{
  struct find_deleted_rev_baton *b = baton;
  apr_hash_index_t *hi;
  apr_pool_t *iterpool;

  /* No paths were changed in this revision.  Nothing to do. */
  if (! log_entry->changed_paths2)
    return SVN_NO_ERROR;

  iterpool = svn_pool_create(scratch_pool);
  for (hi = apr_hash_first(scratch_pool, log_entry->changed_paths2);
       hi != NULL;
       hi = apr_hash_next(hi))
    {
      void *val;
      const char *path;
      svn_log_changed_path_t *log_item;

      svn_pool_clear(iterpool);


      apr_hash_this(hi, (void *) &path, NULL, &val);
      log_item = val;

      /* ### Remove leading slash from paths in log entries. */
      if (path[0] == '/')
          path = svn_relpath_canonicalize(path, iterpool);

      if (svn_path_compare_paths(b->deleted_repos_relpath, path) == 0
          && (log_item->action == 'D' || log_item->action == 'R'))
        {
          svn_client__pathrev_t *yca_loc;
          svn_client__pathrev_t *loc1;
          svn_client__pathrev_t *loc2;

          /* We found a deleted node which occupies the correct path.
           * To be certain that this is the deleted node we're looking for,
           * we must establish whether it is ancestrally related to the
           * "related node" specified in our baton. */
          loc1 = svn_client__pathrev_create_with_relpath(
                   b->repos_root_url, b->repos_uuid, b->related_repos_peg_rev,
                   b->related_repos_relpath, iterpool);
          loc2 = svn_client__pathrev_create_with_relpath(
                   b->repos_root_url, b->repos_uuid, log_entry->revision - 1,
                   b->deleted_repos_relpath, iterpool);
          SVN_ERR(svn_client__get_youngest_common_ancestor(&yca_loc, loc1, loc2,
                                                           NULL, b->ctx,
                                                           iterpool,
                                                           iterpool));
          if (yca_loc != NULL)
            {
              /* Found the correct node, we are done. */
              b->deleted_rev = log_entry->revision;
              return svn_error_create(SVN_ERR_CANCELLED, NULL, NULL);
            }
        }
    }
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Implements tree_conflict_get_details_func_t.
 * Find the revision in which the victim was deleted in the repository. */
static svn_error_t *
conflict_tree_get_details_incoming_delete(svn_client_conflict_t *conflict,
                                          apr_pool_t *scratch_pool)
{
  svn_revnum_t deleted_rev;
  svn_string_t *author_revprop;
  const char *old_repos_relpath;
  const char *new_repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_revnum_t old_rev;
  svn_revnum_t new_rev;
  const char *url;
  const char *corrected_url;
  svn_ra_session_t *ra_session;
  struct conflict_tree_incoming_delete_details *details;
  svn_wc_operation_t operation;

  SVN_ERR(svn_client_conflict_get_incoming_old_repos_location(
            &old_repos_relpath, &old_rev, NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_incoming_new_repos_location(
            &new_repos_relpath, &new_rev, NULL, conflict, scratch_pool,
            scratch_pool));
  SVN_ERR(svn_client_conflict_get_repos_info(&repos_root_url, &repos_uuid,
                                             conflict,
                                             scratch_pool, scratch_pool));
  operation = svn_client_conflict_get_operation(conflict);

  if (operation == svn_wc_operation_update)
    {
      if (old_rev < new_rev)
        {
          /* The update operation went forward in history. */
          url = svn_path_url_add_component2(repos_root_url, new_repos_relpath,
                                            scratch_pool);
          SVN_ERR(svn_client__open_ra_session_internal(&ra_session,
                                                       &corrected_url,
                                                       url, NULL, NULL,
                                                       FALSE,
                                                       FALSE,
                                                       conflict->ctx,
                                                       scratch_pool,
                                                       scratch_pool));
          SVN_ERR(svn_ra_get_deleted_rev(ra_session, "", old_rev, new_rev,
                                         &deleted_rev, scratch_pool));
          SVN_ERR(svn_ra_rev_prop(ra_session, deleted_rev,
                                  SVN_PROP_REVISION_AUTHOR,
                                  &author_revprop, scratch_pool));
          details = apr_pcalloc(conflict->pool, sizeof(*details));
          details->deleted_rev = deleted_rev;
          details->added_rev = SVN_INVALID_REVNUM;
          details->repos_relpath = apr_pstrdup(conflict->pool,
                                               new_repos_relpath);
          details->rev_author = apr_pstrdup(conflict->pool,
                                            author_revprop->data);
        }
      else /* new_rev < old_rev */
        {
          struct find_added_rev_baton b;

          /* The update operation went backwards in history. */
          url = svn_path_url_add_component2(repos_root_url, old_repos_relpath,
                                            scratch_pool);
          SVN_ERR(svn_client__open_ra_session_internal(&ra_session,
                                                       &corrected_url,
                                                       url, NULL, NULL,
                                                       FALSE,
                                                       FALSE,
                                                       conflict->ctx,
                                                       scratch_pool,
                                                       scratch_pool));

          details = apr_pcalloc(conflict->pool, sizeof(*details));
          b.details = details;
          b.pool = scratch_pool;
          /* Figure out when this node was added. */
          SVN_ERR(svn_ra_get_location_segments(ra_session, "", old_rev,
                                               old_rev, new_rev,
                                               find_added_rev, &b,
                                               scratch_pool));
          SVN_ERR(svn_ra_rev_prop(ra_session, details->added_rev,
                                  SVN_PROP_REVISION_AUTHOR,
                                  &author_revprop, scratch_pool));
          details->deleted_rev = SVN_INVALID_REVNUM;
          details->repos_relpath = apr_pstrdup(conflict->pool,
                                               new_repos_relpath);
          details->rev_author = apr_pstrdup(conflict->pool,
                                            author_revprop->data);
        }
    }
  else if (operation == svn_wc_operation_switch)
    {
      if (old_rev < new_rev)
        {
          apr_array_header_t *paths;
          struct find_deleted_rev_baton b;
          svn_error_t *err;

          /* The switch operation went forward in history.
           *
           * The deletion of the node happened on the branch we switched to.
           * Scan new_repos_relpath's parent's log to find the revision which
           * deleted the node. */
          url = svn_path_url_add_component2(
                  repos_root_url,
                  svn_relpath_dirname(new_repos_relpath, scratch_pool),
                  scratch_pool);
          SVN_ERR(svn_client__open_ra_session_internal(&ra_session,
                                                       &corrected_url,
                                                       url, NULL, NULL,
                                                       FALSE,
                                                       FALSE,
                                                       conflict->ctx,
                                                       scratch_pool,
                                                       scratch_pool));

          paths = apr_array_make(scratch_pool, 1, sizeof(const char *));
          APR_ARRAY_PUSH(paths, const char *) = "";

          b.deleted_repos_relpath = new_repos_relpath;
          b.related_repos_relpath = old_repos_relpath;
          b.related_repos_peg_rev = old_rev;
          b.deleted_rev = SVN_INVALID_REVNUM;
          b.repos_root_url = repos_root_url;
          b.repos_uuid = repos_uuid;
          b.ctx = conflict->ctx;

          err = svn_ra_get_log2(ra_session, paths, new_rev, 0,
                                 0, /* no limit */
                                 TRUE, /* need the changed paths list */
                                 FALSE, /* need to traverse copies */
                                 FALSE, /* no need for merged revisions */
                                 /* no need for revprops: */
                                 apr_array_make(scratch_pool, 0,
                                                sizeof(const char *)),
                                 find_deleted_rev, &b,
                                 scratch_pool);
          if (err)
            {
              if (err->apr_err == SVN_ERR_CANCELLED &&
                  b.deleted_rev != SVN_INVALID_REVNUM)
                {
                  /* Log operation was aborted because we found a YCA. */
                  svn_error_clear(err);
                }
              else
                return svn_error_trace(err);
            }

          if (b.deleted_rev == SVN_INVALID_REVNUM)
            {
              /* We could not determine the revision in which the node was
               * deleted. We cannot provide the required details so the best
               * we can do is fall back to the default description. */
              return SVN_NO_ERROR;
            }

          SVN_ERR(svn_ra_rev_prop(ra_session, b.deleted_rev,
                                  SVN_PROP_REVISION_AUTHOR,
                                  &author_revprop, scratch_pool));
          details = apr_pcalloc(conflict->pool, sizeof(*details));
          details->deleted_rev = b.deleted_rev;
          details->added_rev = SVN_INVALID_REVNUM;
          details->repos_relpath = apr_pstrdup(conflict->pool,
                                               new_repos_relpath);
          details->rev_author = apr_pstrdup(conflict->pool,
                                            author_revprop->data);
        }
      else /* new_rev < old_rev */
        {
          struct find_added_rev_baton b;

          /* The switch operation went backwards in history. */
          url = svn_path_url_add_component2(repos_root_url, old_repos_relpath,
                                            scratch_pool);
          SVN_ERR(svn_client__open_ra_session_internal(&ra_session,
                                                       &corrected_url,
                                                       url, NULL, NULL,
                                                       FALSE,
                                                       FALSE,
                                                       conflict->ctx,
                                                       scratch_pool,
                                                       scratch_pool));

          details = apr_pcalloc(conflict->pool, sizeof(*details));
          b.details = details;
          b.pool = scratch_pool;
          /* Figure out when the node we switched away from was added. */
          SVN_ERR(svn_ra_get_location_segments(ra_session, "", old_rev,
                                               old_rev, new_rev,
                                               find_added_rev, &b,
                                               scratch_pool));
          SVN_ERR(svn_ra_rev_prop(ra_session, details->added_rev,
                                  SVN_PROP_REVISION_AUTHOR,
                                  &author_revprop, scratch_pool));
          details->deleted_rev = SVN_INVALID_REVNUM;
          details->repos_relpath = apr_pstrdup(conflict->pool,
                                               new_repos_relpath);
          details->rev_author = apr_pstrdup(conflict->pool,
                                            author_revprop->data);
        }
    }

  conflict->tree_conflict_details = details;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_tree_get_description(const char **description,
                                         svn_client_conflict_t *conflict,
                                         apr_pool_t *result_pool,
                                         apr_pool_t *scratch_pool)
{
  return svn_error_trace(conflict->tree_conflict_get_description_func(
                           description, conflict, result_pool, scratch_pool));
}

void
svn_client_conflict_option_set_merged_propval(
  svn_client_conflict_option_t *option,
  const svn_string_t *merged_propval)
{
  option->type_data.prop.merged_propval = merged_propval;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_postpone(svn_client_conflict_option_t *option,
                      svn_client_conflict_t *conflict,
                      apr_pool_t *scratch_pool)
{
  return SVN_NO_ERROR; /* Nothing to do. */
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_text_conflict(svn_client_conflict_option_t *option,
                      svn_client_conflict_t *conflict,
                      apr_pool_t *scratch_pool)
{
  svn_client_conflict_option_id_t option_id;
  const char *local_abspath;
  const char *lock_abspath;
  svn_wc_conflict_choice_t conflict_choice;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  option_id = svn_client_conflict_option_get_id(option);
  conflict_choice = conflict_option_id_to_wc_conflict_choice(option_id);
  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  err = svn_wc__conflict_text_mark_resolved(conflict->ctx->wc_ctx,
                                            local_abspath,
                                            conflict_choice,
                                            conflict->ctx->cancel_func,
                                            conflict->ctx->cancel_baton,
                                            conflict->ctx->notify_func2,
                                            conflict->ctx->notify_baton2,
                                            scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  svn_io_sleep_for_timestamps(local_abspath, scratch_pool);
  SVN_ERR(err);

  conflict->resolution_text = option_id;

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_prop_conflict(svn_client_conflict_option_t *option,
                      svn_client_conflict_t *conflict,
                      apr_pool_t *scratch_pool)
{
  svn_client_conflict_option_id_t option_id;
  svn_wc_conflict_choice_t conflict_choice;
  const char *local_abspath;
  const char *lock_abspath;
  const char *propname = option->type_data.prop.propname;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  option_id = svn_client_conflict_option_get_id(option);
  conflict_choice = conflict_option_id_to_wc_conflict_choice(option_id);
  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  err = svn_wc__conflict_prop_mark_resolved(ctx->wc_ctx, local_abspath,
                                            propname, conflict_choice,
                                            conflict->ctx->notify_func2,
                                            conflict->ctx->notify_baton2,
                                            scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  svn_io_sleep_for_timestamps(local_abspath, scratch_pool);
  SVN_ERR(err);

  if (propname[0] == '\0')
    {
      apr_hash_index_t *hi;

      /* All properties have been resolved to the same option. */
      for (hi = apr_hash_first(scratch_pool, conflict->prop_conflicts);
           hi;
           hi = apr_hash_next(hi))
        {
          const char *this_propname = apr_hash_this_key(hi);

          svn_hash_sets(conflict->resolved_props,
                        apr_pstrdup(apr_hash_pool_get(conflict->resolved_props),
                                    this_propname),
                        option);
          svn_hash_sets(conflict->prop_conflicts, this_propname, NULL);
        }

      conflict->legacy_prop_conflict_propname = NULL;
    }
  else
    {
      svn_hash_sets(conflict->resolved_props,
                    apr_pstrdup(apr_hash_pool_get(conflict->resolved_props),
                                propname),
                   option);
      svn_hash_sets(conflict->prop_conflicts, propname, NULL);

      conflict->legacy_prop_conflict_propname =
          apr_hash_this_key(apr_hash_first(scratch_pool,
                                           conflict->prop_conflicts));
    }

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_accept_current_wc_state(svn_client_conflict_option_t *option,
                                svn_client_conflict_t *conflict,
                                apr_pool_t *scratch_pool)
{
  svn_client_conflict_option_id_t option_id;
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  option_id = svn_client_conflict_option_get_id(option);
  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  if (option_id != svn_client_conflict_option_accept_current_wc_state)
    return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                             _("Tree conflict on '%s' can only be resolved "
                               "to the current working copy state"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));

  /* Resolve to current working copy state. */
  err = svn_wc__del_tree_conflict(ctx->wc_ctx, local_abspath, scratch_pool);

  /* svn_wc__del_tree_conflict doesn't handle notification for us */
  if (ctx->notify_func2)
    ctx->notify_func2(ctx->notify_baton2,
                      svn_wc_create_notify(local_abspath,
                                           svn_wc_notify_resolved,
                                           scratch_pool),
                      scratch_pool);

  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  SVN_ERR(err);

  conflict->resolution_tree = option_id;

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_update_break_moved_away(svn_client_conflict_option_t *option,
                                svn_client_conflict_t *conflict,
                                apr_pool_t *scratch_pool)
{
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  err = svn_wc__conflict_tree_update_break_moved_away(ctx->wc_ctx,
                                                      local_abspath,
                                                      ctx->cancel_func,
                                                      ctx->cancel_baton,
                                                      ctx->notify_func2,
                                                      ctx->notify_baton2,
                                                      scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  SVN_ERR(err);

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_update_raise_moved_away(svn_client_conflict_option_t *option,
                                svn_client_conflict_t *conflict,
                                apr_pool_t *scratch_pool)
{
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  err = svn_wc__conflict_tree_update_raise_moved_away(ctx->wc_ctx,
                                                      local_abspath,
                                                      ctx->cancel_func,
                                                      ctx->cancel_baton,
                                                      ctx->notify_func2,
                                                      ctx->notify_baton2,
                                                      scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  SVN_ERR(err);

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}

/* Implements conflict_option_resolve_func_t. */
static svn_error_t *
resolve_update_moved_away_node(svn_client_conflict_option_t *option,
                               svn_client_conflict_t *conflict,
                               apr_pool_t *scratch_pool)
{
  const char *local_abspath;
  const char *lock_abspath;
  svn_client_ctx_t *ctx = conflict->ctx;
  svn_error_t *err;

  local_abspath = svn_client_conflict_get_local_abspath(conflict);

  SVN_ERR(svn_wc__acquire_write_lock_for_resolve(&lock_abspath, ctx->wc_ctx,
                                                 local_abspath,
                                                 scratch_pool, scratch_pool));
  err = svn_wc__conflict_tree_update_moved_away_node(ctx->wc_ctx,
                                                     local_abspath,
                                                     ctx->cancel_func,
                                                     ctx->cancel_baton,
                                                     ctx->notify_func2,
                                                     ctx->notify_baton2,
                                                     scratch_pool);
  err = svn_error_compose_create(err, svn_wc__release_write_lock(ctx->wc_ctx,
                                                                 lock_abspath,
                                                                 scratch_pool));
  svn_io_sleep_for_timestamps(local_abspath, scratch_pool);
  SVN_ERR(err);

  conflict->resolution_tree = svn_client_conflict_option_get_id(option);

  return SVN_NO_ERROR;
}

/* Resolver options for a text conflict */
static const svn_client_conflict_option_t text_conflict_options[] =
{
  {
    svn_client_conflict_option_postpone,
    N_("skip this conflict and leave it unresolved"),
    NULL,
    resolve_postpone
  },

  {
    svn_client_conflict_option_base_text,
    N_("discard local and incoming changes for this file"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_incoming_text,
    N_("accept incoming version of entire file"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_working_text,
    N_("reject all incoming changes for this file"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_incoming_text_where_conflicted,
    N_("accept changes only where they conflict"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_working_text_where_conflicted,
    N_("reject changes which conflict and accept the rest"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_merged_text,
    N_("accept the file as it appears in the working copy"),
    NULL,
    resolve_text_conflict
  },

};

/* Resolver options for a binary file conflict */
static const svn_client_conflict_option_t binary_conflict_options[] =
{
  {
    svn_client_conflict_option_postpone,
    N_("skip this conflict and leave it unresolved"),
    NULL,
    resolve_postpone,
  },

  {
    svn_client_conflict_option_incoming_text,
    N_("accept incoming version of binary file"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_working_text,
    N_("accept working copy version of binary file"),
    NULL,
    resolve_text_conflict
  },

  {
    svn_client_conflict_option_merged_text,
    N_("accept the file as it appears in the working copy"),
    NULL,
    resolve_text_conflict
  },

};

/* Resolver options for a property conflict */
static const svn_client_conflict_option_t prop_conflict_options[] =
{
  {
    svn_client_conflict_option_postpone,
    N_("skip this conflict and leave it unresolved"),
    NULL,
    resolve_postpone
  },

  {
    svn_client_conflict_option_base_text,
    N_("discard local and incoming changes for this property"),
    NULL,
    resolve_prop_conflict
  },

  {
    svn_client_conflict_option_incoming_text,
    N_("accept incoming version of entire property value"),
    NULL,
    resolve_prop_conflict
  },

  {
    svn_client_conflict_option_working_text,
    N_("accept working copy version of entire property value"),
    NULL,
    resolve_prop_conflict
  },

  {
    svn_client_conflict_option_incoming_text_where_conflicted,
    N_("accept changes only where they conflict"),
    NULL,
    resolve_prop_conflict
  },

  {
    svn_client_conflict_option_working_text_where_conflicted,
    N_("reject changes which conflict and accept the rest"),
    NULL,
    resolve_prop_conflict
  },

  {
    svn_client_conflict_option_merged_text,
    N_("accept merged version of property value"),
    NULL,
    resolve_prop_conflict
  },

};

static svn_error_t *
assert_text_conflict(svn_client_conflict_t *conflict, apr_pool_t *scratch_pool)
{
  svn_boolean_t text_conflicted;

  SVN_ERR(svn_client_conflict_get_conflicted(&text_conflicted, NULL, NULL,
                                             conflict, scratch_pool,
                                             scratch_pool));

  SVN_ERR_ASSERT(text_conflicted); /* ### return proper error? */

  return SVN_NO_ERROR;
}

static svn_error_t *
assert_prop_conflict(svn_client_conflict_t *conflict, apr_pool_t *scratch_pool)
{
  apr_array_header_t *props_conflicted;

  SVN_ERR(svn_client_conflict_get_conflicted(NULL, &props_conflicted, NULL,
                                             conflict, scratch_pool,
                                             scratch_pool));

  /* ### return proper error? */
  SVN_ERR_ASSERT(props_conflicted && props_conflicted->nelts > 0);

  return SVN_NO_ERROR;
}

static svn_error_t *
assert_tree_conflict(svn_client_conflict_t *conflict, apr_pool_t *scratch_pool)
{
  svn_boolean_t tree_conflicted;

  SVN_ERR(svn_client_conflict_get_conflicted(NULL, NULL, &tree_conflicted,
                                             conflict, scratch_pool,
                                             scratch_pool));

  SVN_ERR_ASSERT(tree_conflicted); /* ### return proper error? */

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_text_get_resolution_options(apr_array_header_t **options,
                                                svn_client_conflict_t *conflict,
                                                apr_pool_t *result_pool,
                                                apr_pool_t *scratch_pool)
{
  const char *mime_type;
  int i;

  SVN_ERR(assert_text_conflict(conflict, scratch_pool));

  *options = apr_array_make(result_pool, ARRAY_LEN(text_conflict_options),
                            sizeof(svn_client_conflict_option_t *));

  mime_type = svn_client_conflict_text_get_mime_type(conflict);
  if (mime_type && svn_mime_type_is_binary(mime_type))
    {
      for (i = 0; i < ARRAY_LEN(binary_conflict_options); i++)
        {
          svn_client_conflict_option_t *option;

          /* We must make a copy to make the memory for option->type_data
           * writable and to localize the description. */
          option = apr_pcalloc(result_pool, sizeof(*option));
          *option = binary_conflict_options[i];
          option->description = _(option->description);
          APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) =
            option;
        }
    }
  else
    {
      for (i = 0; i < ARRAY_LEN(text_conflict_options); i++)
        {
          svn_client_conflict_option_t *option;

          /* We must make a copy to make the memory for option->type_data
           * writable and to localize the description. */
          option = apr_pcalloc(result_pool, sizeof(*option));
          *option = text_conflict_options[i];
          option->description = _(option->description);
          APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) =
            option;
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_prop_get_resolution_options(apr_array_header_t **options,
                                                svn_client_conflict_t *conflict,
                                                apr_pool_t *result_pool,
                                                apr_pool_t *scratch_pool)
{
  int i;

  SVN_ERR(assert_prop_conflict(conflict, scratch_pool));

  *options = apr_array_make(result_pool, ARRAY_LEN(prop_conflict_options),
                            sizeof(svn_client_conflict_option_t *));
  for (i = 0; i < ARRAY_LEN(prop_conflict_options); i++)
    {
      svn_client_conflict_option_t *option;

      /* We must make a copy to make the memory for option->type_data
       * writable and to localize the description. */
      option = apr_pcalloc(result_pool, sizeof(*option));
      *option = prop_conflict_options[i];
      option->description = _(option->description);
      APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) = option;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_tree_get_resolution_options(apr_array_header_t **options,
                                                svn_client_conflict_t *conflict,
                                                apr_pool_t *result_pool,
                                                apr_pool_t *scratch_pool)
{
  svn_client_conflict_option_t *option;
  svn_wc_operation_t operation;
  svn_wc_conflict_reason_t local_change;
  svn_wc_conflict_action_t incoming_change;

  operation = svn_client_conflict_get_operation(conflict);
  local_change = svn_client_conflict_get_local_change(conflict);
  incoming_change = svn_client_conflict_get_incoming_change(conflict);

  SVN_ERR(assert_tree_conflict(conflict, scratch_pool));

  *options = apr_array_make(result_pool, 2,
                            sizeof(svn_client_conflict_option_t *));

  /* Add postpone option. */
  option = apr_pcalloc(result_pool, sizeof(*option));
  option->id = svn_client_conflict_option_postpone;
  option->description = _("skip this conflict and leave it unresolved");
  option->conflict = conflict;
  option->do_resolve_func = resolve_postpone;
  APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) = option;

  /* Add an option which marks the conflict resolved. */
  option = apr_pcalloc(result_pool, sizeof(*option));
  option->id = svn_client_conflict_option_accept_current_wc_state;
  option->description = _("accept current working copy state");
  option->conflict = conflict;
  if ((operation == svn_wc_operation_update ||
       operation == svn_wc_operation_switch) &&
      (local_change == svn_wc_conflict_reason_moved_away ||
       local_change == svn_wc_conflict_reason_deleted ||
       local_change == svn_wc_conflict_reason_replaced) &&
      incoming_change == svn_wc_conflict_action_edit)
    {
      /* We must break moves if the user accepts the current working copy
       * state instead of updating a moved-away node or updating children
       * moved outside of deleted or replaced directory nodes.
       * Else such moves would be left in an invalid state. */
      option->do_resolve_func = resolve_update_break_moved_away;
    }
  else
    option->do_resolve_func = resolve_accept_current_wc_state;

  APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) = option;

  /* Add options which offer automated resolution: */
  if (svn_client_conflict_get_operation(conflict) == svn_wc_operation_update ||
      svn_client_conflict_get_operation(conflict) == svn_wc_operation_switch)
    {
      svn_wc_conflict_reason_t reason;

      reason = svn_client_conflict_get_local_change(conflict);
      if (reason == svn_wc_conflict_reason_moved_away &&
          incoming_change == svn_wc_conflict_action_edit)
        {
          option = apr_pcalloc(result_pool, sizeof(*option));
          option->id =
            svn_client_conflict_option_update_move_destination;
          option->description =
            _("apply incoming changes to move destination");
          option->conflict = conflict;
          option->do_resolve_func = resolve_update_moved_away_node;
          APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) =
            option;
        }
      else if (reason == svn_wc_conflict_reason_deleted ||
               reason == svn_wc_conflict_reason_replaced)
        {
          if (svn_client_conflict_get_incoming_change(conflict) ==
              svn_wc_conflict_action_edit &&
              svn_client_conflict_tree_get_victim_node_kind(conflict) ==
              svn_node_dir)
            {
              option = apr_pcalloc(result_pool, sizeof(*option));
              option->id =
                svn_client_conflict_option_update_any_moved_away_children;
              option->description =
                _("prepare for updating moved-away children, if any");
              option->conflict = conflict;
              option->do_resolve_func = resolve_update_raise_moved_away;
              APR_ARRAY_PUSH((*options), const svn_client_conflict_option_t *) =
                option;
            }
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_tree_get_details(svn_client_conflict_t *conflict,
                                     apr_pool_t *scratch_pool)
{
  SVN_ERR(assert_tree_conflict(conflict, scratch_pool));

  if (conflict->tree_conflict_get_details_func)
    SVN_ERR(conflict->tree_conflict_get_details_func(conflict, scratch_pool));

  return SVN_NO_ERROR;
}

svn_client_conflict_option_id_t
svn_client_conflict_option_get_id(svn_client_conflict_option_t *option)
{
  return option->id;
}

svn_error_t *
svn_client_conflict_option_describe(const char **description,
                                    svn_client_conflict_option_t *option,
                                    apr_pool_t *result_pool,
                                    apr_pool_t *scratch_pool)
{
  *description = apr_pstrdup(result_pool, option->description);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_text_resolve(svn_client_conflict_t *conflict,
                                 svn_client_conflict_option_t *option,
                                 apr_pool_t *scratch_pool)
{
  SVN_ERR(assert_text_conflict(conflict, scratch_pool));
  SVN_ERR(option->do_resolve_func(option, conflict, scratch_pool));

  return SVN_NO_ERROR;
}

svn_client_conflict_option_t *
svn_client_conflict_option_find_by_id(apr_array_header_t *options,
                                      svn_client_conflict_option_id_t option_id)
{
  int i;

  for (i = 0; i < options->nelts; i++)
    {
      svn_client_conflict_option_t *this_option;
      svn_client_conflict_option_id_t this_option_id;
      
      this_option = APR_ARRAY_IDX(options, i, svn_client_conflict_option_t *);
      this_option_id = svn_client_conflict_option_get_id(this_option);

      if (this_option_id == option_id)
        return this_option;
    }

  return NULL;
}

svn_error_t *
svn_client_conflict_text_resolve_by_id(
  svn_client_conflict_t *conflict,
  svn_client_conflict_option_id_t option_id,
  apr_pool_t *scratch_pool)
{
  apr_array_header_t *resolution_options;
  svn_client_conflict_option_t *option;

  SVN_ERR(svn_client_conflict_text_get_resolution_options(
            &resolution_options, conflict,
            scratch_pool, scratch_pool));
  option = svn_client_conflict_option_find_by_id(resolution_options,
                                                 option_id);
  if (option == NULL)
    return svn_error_createf(SVN_ERR_CLIENT_CONFLICT_OPTION_NOT_APPLICABLE,
                             NULL,
                             _("Inapplicable conflict resolution option "
                               "ID '%d' given for conflicted path '%s'"),
                             option_id,
                             svn_dirent_local_style(conflict->local_abspath,
                                                    scratch_pool));
  SVN_ERR(svn_client_conflict_text_resolve(conflict, option, scratch_pool));

  return SVN_NO_ERROR;
}

svn_client_conflict_option_id_t
svn_client_conflict_text_get_resolution(svn_client_conflict_t *conflict)
{
  return conflict->resolution_text;
}

svn_error_t *
svn_client_conflict_prop_resolve(svn_client_conflict_t *conflict,
                                 const char *propname,
                                 svn_client_conflict_option_t *option,
                                 apr_pool_t *scratch_pool)
{
  SVN_ERR(assert_prop_conflict(conflict, scratch_pool));
  option->type_data.prop.propname = propname;
  SVN_ERR(option->do_resolve_func(option, conflict, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_prop_resolve_by_id(
  svn_client_conflict_t *conflict,
  const char *propname,
  svn_client_conflict_option_id_t option_id,
  apr_pool_t *scratch_pool)
{
  apr_array_header_t *resolution_options;
  svn_client_conflict_option_t *option;

  SVN_ERR(svn_client_conflict_prop_get_resolution_options(
            &resolution_options, conflict,
            scratch_pool, scratch_pool));
  option = svn_client_conflict_option_find_by_id(resolution_options,
                                                 option_id);
  if (option == NULL)
    return svn_error_createf(SVN_ERR_CLIENT_CONFLICT_OPTION_NOT_APPLICABLE,
                             NULL,
                             _("Inapplicable conflict resolution option "
                               "ID '%d' given for conflicted path '%s'"),
                             option_id,
                             svn_dirent_local_style(conflict->local_abspath,
                                                    scratch_pool));
  SVN_ERR(svn_client_conflict_prop_resolve(conflict, propname, option,
                                           scratch_pool));

  return SVN_NO_ERROR;
}

svn_client_conflict_option_id_t
svn_client_conflict_prop_get_resolution(svn_client_conflict_t *conflict,
                                        const char *propname)
{
  svn_client_conflict_option_t *option;

  option = svn_hash_gets(conflict->resolved_props, propname);
  if (option == NULL)
    return svn_client_conflict_option_unspecified;

  return svn_client_conflict_option_get_id(option);
}

svn_error_t *
svn_client_conflict_tree_resolve(svn_client_conflict_t *conflict,
                                 svn_client_conflict_option_t *option,
                                 apr_pool_t *scratch_pool)
{
  SVN_ERR(assert_tree_conflict(conflict, scratch_pool));
  SVN_ERR(option->do_resolve_func(option, conflict, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_tree_resolve_by_id(
  svn_client_conflict_t *conflict,
  svn_client_conflict_option_id_t option_id,
  apr_pool_t *scratch_pool)
{
  apr_array_header_t *resolution_options;
  svn_client_conflict_option_t *option;

  /* Backwards compatibility hack: Upper layers may still try to resolve
   * these two tree conflicts as 'mine-conflict' as Subversion 1.9 did.
   * Fix up if necessary... */
  if (option_id == svn_client_conflict_option_working_text_where_conflicted)
    {
      svn_wc_operation_t operation;

      operation = svn_client_conflict_get_operation(conflict);
      if (operation == svn_wc_operation_update ||
          operation == svn_wc_operation_switch)
        {
          svn_wc_conflict_reason_t reason;

          reason = svn_client_conflict_get_local_change(conflict);
          if (reason == svn_wc_conflict_reason_moved_away)
            {
              /* Map 'mine-conflict' to 'update move destination'. */
              option_id = svn_client_conflict_option_update_move_destination;
            }
          else if (reason == svn_wc_conflict_reason_deleted ||
                   reason == svn_wc_conflict_reason_replaced)
            {
              svn_wc_conflict_action_t action;
              svn_node_kind_t node_kind;

              action = svn_client_conflict_get_incoming_change(conflict);
              node_kind =
                svn_client_conflict_tree_get_victim_node_kind(conflict);

              if (action == svn_wc_conflict_action_edit &&
                  node_kind == svn_node_dir)
                {
                  /* Map 'mine-conflict' to 'update any moved away children'. */
                  option_id =
                    svn_client_conflict_option_update_any_moved_away_children;
                }
            }
        }
    }
  else if (option_id == svn_client_conflict_option_merged_text)
    {
      /* Another backwards compatibility hack for 'choose merged'. */
      option_id = svn_client_conflict_option_accept_current_wc_state;
    }
  
  SVN_ERR(svn_client_conflict_tree_get_resolution_options(
            &resolution_options, conflict,
            scratch_pool, scratch_pool));
  option = svn_client_conflict_option_find_by_id(resolution_options,
                                                 option_id);
  if (option == NULL)
    return svn_error_createf(SVN_ERR_CLIENT_CONFLICT_OPTION_NOT_APPLICABLE,
                             NULL,
                             _("Inapplicable conflict resolution option "
                               "ID '%d' given for conflicted path '%s'"),
                             option_id,
                             svn_dirent_local_style(conflict->local_abspath,
                                                    scratch_pool));
  SVN_ERR(svn_client_conflict_tree_resolve(conflict, option, scratch_pool));

  return SVN_NO_ERROR;
}

svn_client_conflict_option_id_t
svn_client_conflict_tree_get_resolution(svn_client_conflict_t *conflict)
{
  return conflict->resolution_tree;
}

/* Return the legacy conflict descriptor which is wrapped by CONFLICT. */
static const svn_wc_conflict_description2_t *
get_conflict_desc2_t(svn_client_conflict_t *conflict)
{
  if (conflict->legacy_text_conflict)
    return conflict->legacy_text_conflict;

  if (conflict->legacy_tree_conflict)
    return conflict->legacy_tree_conflict;

  if (conflict->prop_conflicts && conflict->legacy_prop_conflict_propname)
    return svn_hash_gets(conflict->prop_conflicts,
                         conflict->legacy_prop_conflict_propname);

  return NULL;
}

svn_error_t *
svn_client_conflict_get_conflicted(svn_boolean_t *text_conflicted,
                                   apr_array_header_t **props_conflicted,
                                   svn_boolean_t *tree_conflicted,
                                   svn_client_conflict_t *conflict,
                                   apr_pool_t *result_pool,
                                   apr_pool_t *scratch_pool)
{
  if (text_conflicted)
    *text_conflicted = (conflict->legacy_text_conflict != NULL);

  if (props_conflicted)
    {
      if (conflict->prop_conflicts)
        SVN_ERR(svn_hash_keys(props_conflicted, conflict->prop_conflicts,
                              result_pool));
      else
        *props_conflicted = apr_array_make(result_pool, 0,
                                           sizeof(const char*));
    }

  if (tree_conflicted)
    *tree_conflicted = (conflict->legacy_tree_conflict != NULL);

  return SVN_NO_ERROR;
}

const char *
svn_client_conflict_get_local_abspath(svn_client_conflict_t *conflict)
{
  return conflict->local_abspath;
}

svn_wc_operation_t
svn_client_conflict_get_operation(svn_client_conflict_t *conflict)
{
  return get_conflict_desc2_t(conflict)->operation;
}

svn_wc_conflict_action_t
svn_client_conflict_get_incoming_change(svn_client_conflict_t *conflict)
{
  return get_conflict_desc2_t(conflict)->action;
}

svn_wc_conflict_reason_t
svn_client_conflict_get_local_change(svn_client_conflict_t *conflict)
{
  return get_conflict_desc2_t(conflict)->reason;
}

svn_error_t *
svn_client_conflict_get_repos_info(const char **repos_root_url,
                                   const char **repos_uuid,
                                   svn_client_conflict_t *conflict,
                                   apr_pool_t *result_pool,
                                   apr_pool_t *scratch_pool)
{
  if (repos_root_url)
    {
      if (get_conflict_desc2_t(conflict)->src_left_version)
        *repos_root_url =
          get_conflict_desc2_t(conflict)->src_left_version->repos_url;
      else if (get_conflict_desc2_t(conflict)->src_right_version)
        *repos_root_url =
          get_conflict_desc2_t(conflict)->src_right_version->repos_url;
      else
        *repos_root_url = NULL;
    }

  if (repos_uuid)
    {
      if (get_conflict_desc2_t(conflict)->src_left_version)
        *repos_uuid =
          get_conflict_desc2_t(conflict)->src_left_version->repos_uuid;
      else if (get_conflict_desc2_t(conflict)->src_right_version)
        *repos_uuid =
          get_conflict_desc2_t(conflict)->src_right_version->repos_uuid;
      else
        *repos_uuid = NULL;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_get_incoming_old_repos_location(
  const char **incoming_old_repos_relpath,
  svn_revnum_t *incoming_old_pegrev,
  svn_node_kind_t *incoming_old_node_kind,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  if (incoming_old_repos_relpath)
    {
      if (get_conflict_desc2_t(conflict)->src_left_version)
        *incoming_old_repos_relpath =
          get_conflict_desc2_t(conflict)->src_left_version->path_in_repos;
      else
        *incoming_old_repos_relpath = NULL;
    }

  if (incoming_old_pegrev)
    {
      if (get_conflict_desc2_t(conflict)->src_left_version)
        *incoming_old_pegrev =
          get_conflict_desc2_t(conflict)->src_left_version->peg_rev;
      else
        *incoming_old_pegrev = SVN_INVALID_REVNUM;
    }

  if (incoming_old_node_kind)
    {
      if (get_conflict_desc2_t(conflict)->src_left_version)
        *incoming_old_node_kind =
          get_conflict_desc2_t(conflict)->src_left_version->node_kind;
      else
        *incoming_old_node_kind = svn_node_none;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_conflict_get_incoming_new_repos_location(
  const char **incoming_new_repos_relpath,
  svn_revnum_t *incoming_new_pegrev,
  svn_node_kind_t *incoming_new_node_kind,
  svn_client_conflict_t *conflict,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  if (incoming_new_repos_relpath)
    {
      if (get_conflict_desc2_t(conflict)->src_right_version)
        *incoming_new_repos_relpath =
          get_conflict_desc2_t(conflict)->src_right_version->path_in_repos;
      else
        *incoming_new_repos_relpath = NULL;
    }

  if (incoming_new_pegrev)
    {
      if (get_conflict_desc2_t(conflict)->src_right_version)
        *incoming_new_pegrev =
          get_conflict_desc2_t(conflict)->src_right_version->peg_rev;
      else
        *incoming_new_pegrev = SVN_INVALID_REVNUM;
    }

  if (incoming_new_node_kind)
    {
      if (get_conflict_desc2_t(conflict)->src_right_version)
        *incoming_new_node_kind =
          get_conflict_desc2_t(conflict)->src_right_version->node_kind;
      else
        *incoming_new_node_kind = svn_node_none;
    }

  return SVN_NO_ERROR;
}

svn_node_kind_t
svn_client_conflict_tree_get_victim_node_kind(svn_client_conflict_t *conflict)
{
  SVN_ERR_ASSERT_NO_RETURN(assert_tree_conflict(conflict, conflict->pool)
                           == SVN_NO_ERROR);

  return get_conflict_desc2_t(conflict)->node_kind;
}

svn_error_t *
svn_client_conflict_prop_get_propvals(const svn_string_t **base_propval,
                                      const svn_string_t **working_propval,
                                      const svn_string_t **incoming_old_propval,
                                      const svn_string_t **incoming_new_propval,
                                      svn_client_conflict_t *conflict,
                                      const char *propname,
                                      apr_pool_t *result_pool)
{
  const svn_wc_conflict_description2_t *desc;

  SVN_ERR(assert_prop_conflict(conflict, conflict->pool));

  desc = svn_hash_gets(conflict->prop_conflicts, propname);
  if (desc == NULL)
    return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE, NULL,
                             _("Property '%s' is not in conflict."), propname);

  if (base_propval)
    *base_propval =
      svn_string_dup(desc->prop_value_base, result_pool);

  if (working_propval)
    *working_propval =
      svn_string_dup(desc->prop_value_working, result_pool);

  if (incoming_old_propval)
    *incoming_old_propval =
      svn_string_dup(desc->prop_value_incoming_old, result_pool);

  if (incoming_new_propval)
    *incoming_new_propval =
      svn_string_dup(desc->prop_value_incoming_new, result_pool);

  return SVN_NO_ERROR;
}

const char *
svn_client_conflict_prop_get_reject_abspath(svn_client_conflict_t *conflict)
{
  SVN_ERR_ASSERT_NO_RETURN(assert_prop_conflict(conflict, conflict->pool)
                           == SVN_NO_ERROR);

  /* svn_wc_conflict_description2_t stores this path in 'their_abspath' */
  return get_conflict_desc2_t(conflict)->their_abspath;
}

const char *
svn_client_conflict_text_get_mime_type(svn_client_conflict_t *conflict)
{
  SVN_ERR_ASSERT_NO_RETURN(assert_text_conflict(conflict, conflict->pool)
                           == SVN_NO_ERROR);

  return get_conflict_desc2_t(conflict)->mime_type;
}

svn_error_t *
svn_client_conflict_text_get_contents(const char **base_abspath,
                                      const char **working_abspath,
                                      const char **incoming_old_abspath,
                                      const char **incoming_new_abspath,
                                      svn_client_conflict_t *conflict,
                                      apr_pool_t *result_pool,
                                      apr_pool_t *scratch_pool)
{
  SVN_ERR(assert_text_conflict(conflict, scratch_pool));

  if (base_abspath)
    {
      if (svn_client_conflict_get_operation(conflict) ==
          svn_wc_operation_merge)
        *base_abspath = NULL; /* ### WC base contents not available yet */
      else /* update/switch */
        *base_abspath = get_conflict_desc2_t(conflict)->base_abspath;
    }

  if (working_abspath)
    *working_abspath = get_conflict_desc2_t(conflict)->my_abspath;

  if (incoming_old_abspath)
    *incoming_old_abspath = get_conflict_desc2_t(conflict)->base_abspath;

  if (incoming_new_abspath)
    *incoming_new_abspath = get_conflict_desc2_t(conflict)->their_abspath;

  return SVN_NO_ERROR;
}
