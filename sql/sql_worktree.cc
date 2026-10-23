/* Copyright (c) 2026, MySQL worktrees contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "sql/sql_worktree.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include "m_string.h"
#include "my_dbug.h"
#include "mysqld_error.h"
#include "sql/auth/auth_acls.h"
#include "sql/auth/auth_common.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/dd.h"
#include "sql/dd/dd_schema.h"
#include "sql/dd/dictionary.h"
#include "sql/dd/dd_table.h"
#include "sql/dd/impl/sdi.h"
#include "sql/dd/types/event.h"
#include "sql/dd/types/foreign_key.h"
#include "sql/dd/types/routine.h"
#include "sql/dd/types/schema.h"
#include "sql/dd/types/table.h"
#include "sql/dd/types/view.h"
#include "sql/dd/impl/types/column_impl.h"
#include "sql/dd/impl/types/event_impl.h"
#include "sql/dd/impl/types/parameter_impl.h"
#include "sql/dd/impl/types/routine_impl.h"
#include "sql/dd/impl/types/view_impl.h"
#include "sql/dd/types/function.h"
#include "sql/dd/types/parameter.h"
#include "sql/dd/types/procedure.h"
#include "sql/dd/types/trigger.h"
#include "sql/dd/types/view_routine.h"
#include "sql/dd/types/view_table.h"
#include "scope_guard.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/derror.h"
#include "sql/sp_head.h"
#include "sql/sp_rcontext.h"
#include "sql/sql_error.h"
#include "sql/system_variables.h"
#include "sql/handler.h"
#include "sql/mdl.h"
#include "sql/mysqld.h"
#include "sql/error_handler.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_db.h"
#include "sql/sql_lex.h"
#include "sql/sql_table.h"
#include "sql/system_variables.h"
#include "sql/table.h"

namespace {

/** Reject a source schema the reflink core cannot clone yet. */
bool unsupported(const char *what) {
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), what);
  return true;
}

/** @return name as used in MDL keys (case-folded for l_c_t_n = 2) */
const char *mdl_name(THD *thd, const char *name) {
  if (lower_case_table_names != 2) return name;
  char *buf = thd->mem_strdup(name);
  my_casedn_str(&my_charset_utf8mb3_tolower_ci, buf);
  return buf;
}

using Table_names = std::vector<std::pair<std::string, std::string>>;

/** Lock tables (db, name) with the given MDL type for the statement. */
bool lock_tables(THD *thd, const Table_names &tables, enum_mdl_type type) {
  if (tables.empty()) return false;
  Table_ref *first = nullptr;
  Table_ref **next = &first;
  for (const auto &[db, name] : tables) {
    const char *mdl_db = mdl_name(thd, db.c_str());
    const char *tname = mdl_name(thd, name.c_str());
    auto *tl = new (thd->mem_root)
        Table_ref(mdl_db, strlen(mdl_db), tname, strlen(tname), tname, TL_IGNORE);
    if (tl == nullptr) return true;
    tl->mdl_request.set_type(type);
    *next = tl;
    next = &tl->next_global;
  }
  return lock_table_names(thd, first, nullptr,
                          thd->variables.lock_wait_timeout, 0);
}

Table_names in_schema(const char *db, const std::vector<std::string> &names) {
  Table_names tables;
  for (const auto &name : names) tables.emplace_back(db, name);
  return tables;
}

bool same_schema_name(const std::string &a, const char *b) {
  return lower_case_table_names == 0
             ? a == b
             : my_strcasecmp(system_charset_info, a.c_str(), b) == 0;
}

}  // namespace

Remap_result remap_schema(const std::string &in, const char *src,
                          const char *dst, ulonglong sql_mode,
                          const std::function<bool(const std::string &)> &exists) {
  enum Kind { IDENT, QIDENT, STRING, DOT, OTHER };
  struct Tok {
    size_t b, e;
    Kind kind;
    std::string value;  // unquoted
    char quote;
  };
  const bool ansi = sql_mode & MODE_ANSI_QUOTES;
  const bool no_bs = sql_mode & MODE_NO_BACKSLASH_ESCAPES;
  const auto ident_char = [](unsigned char c) {
    return isalnum(c) || c == '_' || c == '$' || c >= 0x80;
  };

  std::vector<Tok> toks;
  const size_t n = in.size();
  bool in_versioned = false;  // inside /*! ... */, which is executed SQL
  for (size_t i = 0; i < n;) {
    const char c = in[i];
    if (isspace(static_cast<unsigned char>(c))) {
      ++i;
    } else if (c == '#' ||
               (c == '-' && i + 2 <= n && in[i + 1] == '-' &&
                (i + 2 == n || isspace(static_cast<unsigned char>(in[i + 2])) ||
                 iscntrl(static_cast<unsigned char>(in[i + 2]))))) {
      while (i < n && in[i] != '\n') ++i;
    } else if (c == '/' && i + 1 < n && in[i + 1] == '*') {
      if (i + 2 < n && (in[i + 2] == '!' || in[i + 2] == '+')) {
        i += 3;
        while (i < n && isdigit(static_cast<unsigned char>(in[i]))) ++i;
        in_versioned = true;
      } else {
        const size_t end = in.find("*/", i + 2);
        i = end == std::string::npos ? n : end + 2;
      }
    } else if (in_versioned && c == '*' && i + 1 < n && in[i + 1] == '/') {
      in_versioned = false;
      i += 2;
    } else if (c == '`' || (ansi && c == '"')) {
      Tok t{i, 0, QIDENT, "", c};
      for (++i; i < n; ++i) {
        if (in[i] == c) {
          if (i + 1 < n && in[i + 1] == c) {
            t.value += c;
            ++i;
            continue;
          }
          break;
        }
        t.value += in[i];
      }
      t.e = ++i;
      toks.push_back(t);
    } else if (c == '\'' || c == '"') {
      Tok t{i, 0, STRING, "", c};
      for (++i; i < n; ++i) {
        if (in[i] == '\\' && !no_bs && i + 1 < n) {
          t.value += in[++i];
          continue;
        }
        if (in[i] == c) {
          if (i + 1 < n && in[i + 1] == c) {
            t.value += c;
            ++i;
            continue;
          }
          break;
        }
        t.value += in[i];
      }
      t.e = ++i;
      toks.push_back(t);
    } else if (ident_char(static_cast<unsigned char>(c))) {
      Tok t{i, 0, IDENT, "", 0};
      while (i < n && ident_char(static_cast<unsigned char>(in[i]))) t.value += in[i++];
      t.e = i;
      toks.push_back(t);
    } else {
      toks.push_back(Tok{i, i + 1, c == '.' ? DOT : OTHER, std::string(1, c), 0});
      ++i;
    }
  }

  Remap_result r;
  const std::string src_dot = std::string(src) + ".";
  std::vector<std::pair<size_t, size_t>> spans;  // tokens to replace
  for (size_t k = 0; k < toks.size(); ++k) {
    const Tok &t = toks[k];
    if (t.kind == STRING) {
      std::string lower = t.value, needle = src_dot;
      for (auto &ch : lower) ch = tolower(static_cast<unsigned char>(ch));
      for (auto &ch : needle) ch = tolower(static_cast<unsigned char>(ch));
      if (lower.find(needle) != std::string::npos ||
          lower.find("`" + needle.substr(0, needle.size() - 1) + "`.") !=
              std::string::npos)
        r.literal_with_src = true;
      else if (same_schema_name(t.value, src))
        r.literal_is_src = true;
      continue;
    }
    if ((t.kind != IDENT && t.kind != QIDENT) || !same_schema_name(t.value, src))
      continue;
    const bool qualifies = k + 2 < toks.size() && toks[k + 1].kind == DOT &&
                           (toks[k + 2].kind == IDENT || toks[k + 2].kind == QIDENT);
    const bool after_dot = k > 0 && toks[k - 1].kind == DOT;
    if (!qualifies || after_dot) {
      r.src_as_name = true;  // a table, column or alias that is also named src
      continue;
    }
    if (exists && !exists(toks[k + 2].value)) continue;
    spans.emplace_back(k, 0);
  }

  std::string quoted_dst;
  bool plain = *dst != 0 && !isdigit(static_cast<unsigned char>(*dst));
  for (const char *p = dst; *p; ++p) plain &= ident_char(static_cast<unsigned char>(*p));
  size_t pos = 0;
  for (auto &[k, unused] : spans) {
    const Tok &t = toks[k];
    r.text.append(in, pos, t.b - pos);
    const char q = t.kind == QIDENT ? t.quote : (plain ? 0 : '`');
    if (q == 0) {
      r.text += dst;
    } else {
      r.text += q;
      for (const char *p = dst; *p; ++p) {
        if (*p == q) r.text += q;
        r.text += *p;
      }
      r.text += q;
    }
    pos = t.e;
    ++r.remapped;
  }
  r.text.append(in, pos, std::string::npos);
  return r;
}

namespace {

/** Remap one stored body, reporting what clone-time remapping cannot cover. */
dd::String_type remap_body(THD *thd, const char *kind, const char *wt,
                           const dd::String_type &name, const dd::String_type &body,
                           ulonglong sql_mode, const char *src, bool *warned) {
  const Remap_result r = remap_schema(body.c_str(), src, wt, sql_mode);
  if (!*warned) {
    const std::string lower = [&] {
      std::string l(body.c_str());
      for (auto &ch : l) ch = tolower(static_cast<unsigned char>(ch));
      return l;
    }();
    const bool prepares = lower.find("prepare") != std::string::npos;
    if (r.literal_with_src && !prepares) {
      push_warning_printf(
          thd, Sql_condition::SL_WARNING, ER_UNKNOWN_ERROR,
          "CREATE WORKTREE: %s %s.%s has `%s.` inside a string literal that is "
          "not PREPAREd; it still refers to %s",
          kind, wt, name.c_str(), src, src);
      *warned = true;
    } else if (r.literal_is_src) {
      push_warning_printf(
          thd, Sql_condition::SL_WARNING, ER_UNKNOWN_ERROR,
          prepares
              ? "CREATE WORKTREE: %s %s.%s uses the schema name '%s' as data; "
                "it is remapped at run time only where it ends up in a "
                "PREPAREd statement (not in comparisons with DATABASE() or "
                "*_SCHEMA)"
              : "CREATE WORKTREE: %s %s.%s uses the schema name '%s' as data "
                "(e.g. compared with DATABASE() or *_SCHEMA); it is not "
                "remapped",
          kind, wt, name.c_str(), src);
      *warned = true;
    } else if (r.src_as_name) {
      push_warning_printf(
          thd, Sql_condition::SL_WARNING, ER_UNKNOWN_ERROR,
          "CREATE WORKTREE: %s %s.%s also uses `%s` as a table, column or alias "
          "name; check its remapped references",
          kind, wt, name.c_str(), src);
      *warned = true;
    }
  }
  return dd::String_type(r.text.c_str(), r.text.size());
}

/** Cloned objects keep their definer. Creating an object for someone else
needs SET_ANY_DEFINER (or SUPER), as CREATE ... DEFINER= does. */
bool check_definer(THD *thd, const dd::String_type &user,
                   const dd::String_type &host) {
  Security_context *sctx = thd->security_context();
  if (strcmp(sctx->priv_user().str, user.c_str()) == 0 &&
      my_strcasecmp(system_charset_info, sctx->priv_host().str, host.c_str()) == 0)
    return false;
  if (sctx->has_global_grant(STRING_WITH_LEN("SET_ANY_DEFINER")).first ||
      sctx->check_access(SUPER_ACL))
    return false;
  my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "SET_ANY_DEFINER");
  return true;
}

bool lock_objects(THD *thd, const std::vector<MDL_key> &keys) {
  MDL_request_list reqs;
  for (const auto &key : keys) {
    auto *r = new (thd->mem_root) MDL_request;
    if (r == nullptr) return true;
    MDL_REQUEST_INIT_BY_KEY(r, &key, MDL_EXCLUSIVE, MDL_TRANSACTION);
    reqs.push_front(r);
  }
  return thd->mdl_context.acquire_locks(&reqs, thd->variables.lock_wait_timeout);
}

}  // namespace

bool Sql_cmd_create_worktree::execute(THD *thd) {
  DBUG_TRACE;
  LEX *lex = thd->lex;
  const char *wt = lex->name.str;  // validated, CREATE_ACL checked by caller

  LEX_STRING src_name{const_cast<char *>(m_src.str), m_src.length};
  if (check_and_convert_db_name(&src_name, false) != Ident_name_check::OK)
    return true;
  const char *src = src_name.str;
  if (check_access(thd, SELECT_ACL, src, nullptr, nullptr, true, false))
    return true;
  if (my_strcasecmp(table_alias_charset, src, wt) == 0) {
    my_error(ER_DB_CREATE_EXISTS, MYF(0), wt);
    return true;
  }
  if (dd::get_dictionary()->is_dd_schema_name(src) ||
      my_strcasecmp(system_charset_info, src, "sys") == 0 ||
      my_strcasecmp(system_charset_info, src, "information_schema") == 0 ||
      my_strcasecmp(system_charset_info, src, "performance_schema") == 0) {
    return unsupported("CREATE WORKTREE from a system schema");
  }

  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  dd::cache::Dictionary_client &dc = *thd->dd_client();

  /* S lock on the source schema: no table can be created, dropped, renamed
  or altered in it until we commit (all DDL takes IX on the schema). */
  MDL_request schema_req;
  MDL_REQUEST_INIT(&schema_req, MDL_key::SCHEMA, mdl_name(thd, src), "",
                   MDL_SHARED, MDL_TRANSACTION);
  if (thd->mdl_context.acquire_lock(&schema_req,
                                    thd->variables.lock_wait_timeout) ||
      dd::mdl_lock_schema(thd, src, MDL_TRANSACTION))  // IX for DD lookups
    return true;

  const dd::Schema *src_schema = nullptr;
  if (dc.acquire(src, &src_schema)) return true;
  if (src_schema == nullptr) {
    my_error(ER_BAD_DB_ERROR, MYF(0), src);
    return true;
  }

  /* Views, routines and events are cloned with `src.` references remapped
(fork-design.md §10.6); the S lock above keeps them stable. */
  std::vector<const dd::View *> views;
  std::vector<const dd::Routine *> routines;
  std::vector<const dd::Event *> events;
  if (dc.fetch_schema_components(src_schema, &views) ||
      dc.fetch_schema_components(src_schema, &routines) ||
      dc.fetch_schema_components(src_schema, &events))
    return true;
  const auto need = [&](Access_bitmask acl) {
    return check_access(thd, acl, wt, nullptr, nullptr, true, false);
  };
  const auto need_src = [&](Access_bitmask acl) {
    return check_access(thd, acl, src, nullptr, nullptr, true, false);
  };
  if ((!events.empty() && need_src(EVENT_ACL)) ||
      (!views.empty() && (need(CREATE_VIEW_ACL) ||
                          check_access(thd, SHOW_VIEW_ACL, src, nullptr,
                                       nullptr, true, false))) ||
      (!routines.empty() && need(CREATE_PROC_ACL)) ||
      (!events.empty() && need(EVENT_ACL)))
    return true;
  for (const dd::View *v : views) {
    if (check_definer(thd, v->definer_user(), v->definer_host())) return true;
  }
  for (const dd::Routine *r : routines) {
    if (r->type() == dd::Routine::RT_LIBRARY)
      return unsupported("CREATE WORKTREE of a schema with JavaScript libraries");
    if (check_definer(thd, r->definer_user(), r->definer_host())) return true;
  }
  for (const dd::Event *e : events) {
    if (check_definer(thd, e->definer_user(), e->definer_host())) return true;
  }

  std::vector<std::string> names;
  {
    std::vector<const dd::Table *> tables;
    if (dc.fetch_schema_components(src_schema, &tables)) return true;
    for (const dd::Table *t : tables) {
      if (t->hidden() != dd::Abstract_table::HT_VISIBLE) continue;
      names.emplace_back(t->name().c_str());
    }
  }

  /* Every source table must be fully readable: table-level SELECT, not just
  some columns (a clone exposes all of them). */
  for (const auto &name : names) {
    auto *tl = new (thd->mem_root)
        Table_ref(src, strlen(src), thd->mem_strdup(name.c_str()), name.length(),
                  thd->mem_strdup(name.c_str()), TL_IGNORE);
    if (tl == nullptr ||
        check_table_access(thd, SELECT_ACL, tl, false, 1, false))
      return true;
    if (!(tl->grant.privilege & SELECT_ACL)) {
      my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0), "SELECT",
               thd->security_context()->priv_user().str,
               thd->security_context()->host_or_ip().str, tl->table_name);
      return true;
    }
  }

  /* Block writers to the source tables for the whole statement. Waiting for
  SNW also waits for open transactions that wrote them to finish. */
  if (lock_tables(thd, in_schema(src, names), MDL_SHARED_NO_WRITE)) return true;

  std::vector<const dd::Table *> src_tables;
  for (const auto &name : names) {
    const dd::Table *t = nullptr;
    if (dc.acquire(src, name.c_str(), &t)) return true;
    assert(t != nullptr);
    if (my_strcasecmp(system_charset_info, t->engine().c_str(), "InnoDB") != 0)
      return unsupported("CREATE WORKTREE of a schema with non-InnoDB tables");
    if (t->has_trigger()) {
      if (need(TRIGGER_ACL) || need_src(TRIGGER_ACL)) return true;
      for (const dd::Trigger *tr : t->triggers()) {
        if (check_definer(thd, tr->definer_user(), tr->definer_host()))
          return true;
      }
    }
    src_tables.push_back(t);
  }

  /* FK parents outside the source schema gain the clones as children: their
  DD objects are updated, so they need X locks, and their caches are
  invalidated after the commit (as for CREATE TABLE with a foreign key). */
  Table_names ext_parents;
  for (const dd::Table *t : src_tables) {
    for (const dd::Foreign_key *fk : t->foreign_keys()) {
      if (my_strcasecmp(table_alias_charset,
                        fk->referenced_table_schema_name().c_str(), src) != 0) {
        ext_parents.emplace_back(fk->referenced_table_schema_name().c_str(),
                                 fk->referenced_table_name().c_str());
      }
    }
  }
  if (lock_tables(thd, ext_parents, MDL_EXCLUSIVE)) return true;

  HA_CREATE_INFO create_info;
  create_info.default_table_charset =
      get_charset(src_schema->default_collation_id(), MYF(0));
  if (create_info.default_table_charset != nullptr) {
    create_info.used_fields |= HA_CREATE_USED_DEFAULT_CHARSET;
  }

  const auto populate = [&]() -> bool {
    if (lock_tables(thd, in_schema(wt, names), MDL_EXCLUSIVE)) return true;

    const dd::Schema *wt_schema = nullptr;
    if (dc.acquire(wt, &wt_schema)) return true;
    assert(wt_schema != nullptr);

    for (const dd::Table *src_table : src_tables) {
      /* A serialize/deserialize round trip is a deep copy without object ids
      (the same path IMPORT TABLE uses), keeping columns incl. hidden
      instantly-dropped ones, indexes and the SE private data. */
      const dd::Sdi_type sdi = dd::serialize(thd, *src_table, src_schema->name());
      std::unique_ptr<dd::Table> dst(dd::create_object<dd::Table>());
      dd::String_type sdi_schema;
      if (dd::deserialize(thd, sdi, dst.get(), dd::CheckDefaultCompatibility,
                          &sdi_schema))
        return true;
      dst->set_schema_id(wt_schema->id());

      for (dd::Foreign_key *fk : *dst->foreign_keys()) {
        if (my_strcasecmp(table_alias_charset,
                          fk->referenced_table_schema_name().c_str(),
                          src) == 0) {
          fk->set_referenced_table_schema_name(wt);
        }
      }

      handlerton *hton = nullptr;
      if (dd::table_storage_engine(thd, src_table, &hton)) return true;
      if (hton->clone_table == nullptr)
        return unsupported("CREATE WORKTREE for this storage engine");
      if (hton->clone_table(hton, thd, *src_table, dst.get(), wt)) return true;

      /* Triggers are not in the SDI; copy them (with fresh ids) and remap. */
      if (src_table->has_trigger()) {
        dst->copy_triggers(src_table);
        for (dd::Trigger *tr : *dst->triggers()) {
          bool warned = false;
          tr->set_action_statement(remap_body(thd, "trigger", wt, tr->name(),
                                              tr->action_statement(),
                                              tr->sql_mode(), src, &warned));
          tr->set_action_statement_utf8(remap_body(
              thd, "trigger", wt, tr->name(), tr->action_statement_utf8(),
              tr->sql_mode(), src, &warned));
        }
      }

      if (dc.store(dst.get())) return true;
    }

    std::vector<MDL_key> keys;
    for (const dd::View *v : views) {
      keys.emplace_back();
      keys.back().mdl_key_init(MDL_key::TABLE, mdl_name(thd, wt),
                               mdl_name(thd, v->name().c_str()));
    }
    for (const dd::Routine *r : routines) {
      keys.emplace_back();
      dd::Routine::create_mdl_key(r->type(), wt, r->name(), &keys.back());
    }
    for (const dd::Event *e : events) {
      keys.emplace_back();
      dd::Event::create_mdl_key(wt, e->name(), &keys.back());
    }
    if (lock_objects(thd, keys)) return true;

    /* Deep copies with fresh object ids (clone() keeps them), moved to wt,
    with `src.` references spliced to `wt.` at their exact token positions. */
    for (const dd::View *src_view : views) {
      std::unique_ptr<dd::View> v(src_view->clone());
      dynamic_cast<dd::View_impl *>(v.get())->set_id(dd::INVALID_OBJECT_ID);
      for (dd::Column *c : *v->columns())
        dynamic_cast<dd::Column_impl *>(c)->set_id(dd::INVALID_OBJECT_ID);
      v->set_schema_id(wt_schema->id());
      bool warned = false;
      const ulonglong mode = thd->variables.sql_mode & ~MODE_ANSI_QUOTES;
      v->set_definition(remap_body(thd, "view", wt, v->name(), v->definition(),
                                   mode, src, &warned));
      v->set_definition_utf8(remap_body(thd, "view", wt, v->name(),
                                        v->definition_utf8(), mode, src, &warned));
      for (const dd::View_table *vt : v->tables()) {
        if (same_schema_name(vt->table_schema().c_str(), src))
          const_cast<dd::View_table *>(vt)->set_table_schema(wt);
      }
      for (const dd::View_routine *vr : v->routines()) {
        if (same_schema_name(vr->routine_schema().c_str(), src))
          const_cast<dd::View_routine *>(vr)->set_routine_schema(wt);
      }
      if (dc.store(v.get())) return true;
    }
    for (const dd::Routine *src_routine : routines) {
      std::unique_ptr<dd::Routine> r(src_routine->clone());
      dynamic_cast<dd::Routine_impl *>(r.get())->set_id(dd::INVALID_OBJECT_ID);
      for (const dd::Parameter *prm : r->parameters())
        dynamic_cast<dd::Parameter_impl *>(const_cast<dd::Parameter *>(prm))
            ->set_id(dd::INVALID_OBJECT_ID);
      r->set_schema_id(wt_schema->id());
      bool warned = false;
      r->set_definition(remap_body(thd, "routine", wt, r->name(), r->definition(),
                                   r->sql_mode(), src, &warned));
      r->set_definition_utf8(remap_body(thd, "routine", wt, r->name(),
                                        r->definition_utf8(), r->sql_mode(), src,
                                        &warned));
      const bool fail = r->type() == dd::Routine::RT_FUNCTION
                            ? dc.store(dynamic_cast<dd::Function *>(r.get()))
                            : dc.store(dynamic_cast<dd::Procedure *>(r.get()));
      if (fail) return true;
    }
    for (const dd::Event *src_event : events) {
      std::unique_ptr<dd::Event> e(src_event->clone());
      dynamic_cast<dd::Event_impl *>(e.get())->set_id(dd::INVALID_OBJECT_ID);
      e->set_schema_id(wt_schema->id());
      /* Never run a copy of the source's jobs by surprise (§10.6). */
      e->set_event_status(dd::Event::ES_DISABLED);
      bool warned = false;
      e->set_definition(remap_body(thd, "event", wt, e->name(), e->definition(),
                                   e->sql_mode(), src, &warned));
      e->set_definition_utf8(remap_body(thd, "event", wt, e->name(),
                                        e->definition_utf8(), e->sql_mode(), src,
                                        &warned));
      if (dc.store(e.get())) return true;
    }

    /* Lineage, nearest ancestor first ("src,src's source,..."): the runtime
    remap of dynamic SQL inside cloned objects, whose string literals may
    still name any ancestor. */
    dd::String_type lineage(src);
    if (src_schema->options().exists("worktree_of")) {
      dd::String_type older;
      if (!src_schema->options().get("worktree_of", &older)) lineage += "," + older;
    }
    dd::Schema *wt_mod = nullptr;
    if (dc.acquire_for_modification(wt, &wt_mod) || wt_mod == nullptr)
      return true;
    wt_mod->options().set("worktree_of", lineage);
    if (dc.update(wt_mod)) return true;

    /* The deserialized parents do not know about their (also new) children
    yet; rebuild that from the stored foreign keys. */
    for (const auto &name : names) {
      if (adjust_fk_parents(thd, wt, name.c_str(), true, nullptr)) return true;
    }

    DBUG_EXECUTE_IF("wt_crash_before_commit", DBUG_SUICIDE(););
    return false;
  };

  if (mysql_create_db(thd, wt, &create_info, populate)) return true;
  worktree_schema_changed(wt, true);

  /* Still under X MDL: drop cached definitions of the external parents so
  they are reloaded with their new children (TDC, DD cache and SE cache). */
  for (const auto &[db, name] : ext_parents) {
    close_all_tables_for_name(thd, db.c_str(), name.c_str(), false);
    Dummy_error_handler error_handler;
    thd->push_internal_handler(&error_handler);
    std::ignore = dc.invalidate(db.c_str(), name.c_str());
    thd->pop_internal_handler();
    handlerton *hton = nullptr;
    const dd::Table *parent = nullptr;
    if (!dc.acquire(db.c_str(), name.c_str(), &parent) && parent != nullptr &&
        !dd::table_storage_engine(thd, parent, &hton) &&
        hton->dict_cache_reset != nullptr) {
      hton->dict_cache_reset(mdl_name(thd, db.c_str()),
                             mdl_name(thd, name.c_str()));
    }
  }

  DBUG_EXECUTE_IF("wt_crash_after_commit", DBUG_SUICIDE(););
  return false;
}

namespace {
/** Which schemas are worktrees: answers the common "no" without a lock.
Set by CREATE WORKTREE, cleared by DROP DATABASE, filled lazily from the DD. */
std::mutex wt_known_mutex;
std::unordered_map<std::string, bool> wt_known;

std::string wt_key(const char *db) {
  std::string k(db);
  if (lower_case_table_names != 0)
    for (auto &ch : k) ch = tolower(static_cast<unsigned char>(ch));
  return k;
}
}  // namespace

void worktree_schema_changed(const char *db, bool is_worktree) {
  const std::lock_guard<std::mutex> guard(wt_known_mutex);
  if (is_worktree)
    wt_known[wt_key(db)] = true;
  else
    wt_known.erase(wt_key(db));
}

const char *worktree_remap_dynamic_sql(THD *thd, const char *query,
                                       size_t *query_len) {
  /* Only PREPARE issued inside a stored program of a worktree; top-level user
  SQL is never remapped (fork-design.md §10.6). */
  if (thd->sp_runtime_ctx == nullptr || thd->sp_runtime_ctx->sp == nullptr)
    return query;
  const LEX_STRING &db = thd->sp_runtime_ctx->sp->m_db;
  if (db.length == 0) return query;
  {
    const std::lock_guard<std::mutex> guard(wt_known_mutex);
    auto it = wt_known.find(wt_key(db.str));
    if (it != wt_known.end() && !it->second) return query;
  }

  /* Fail closed from here on: running the statement unremapped would run it
  against the source. A lock conflict (e.g. a concurrent CREATE WORKTREE FROM
  this worktree) fails the PREPARE; the caller's transaction can retry.
  The lock must outlive the releaser of the DD objects acquired under it. */
  MDL_ticket *ticket = nullptr;
  auto release = create_scope_guard([&]() {
    if (ticket != nullptr) thd->mdl_context.release_lock(ticket);
  });
  DBUG_EXECUTE_IF("wt_remap_lock_fails", {
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    return nullptr;
  });
  if (dd::mdl_lock_schema(thd, db.str, MDL_EXPLICIT, &ticket)) return nullptr;
  dd::cache::Dictionary_client &dc = *thd->dd_client();
  const dd::cache::Dictionary_client::Auto_releaser releaser(&dc);
  const dd::Schema *schema = nullptr;
  if (dc.acquire(db.str, &schema)) return nullptr;
  dd::String_type src;
  const bool is_worktree =
      schema != nullptr && schema->options().exists("worktree_of");
  {
    const std::lock_guard<std::mutex> guard(wt_known_mutex);
    if (schema != nullptr) wt_known[wt_key(db.str)] = is_worktree;
  }
  if (!is_worktree) return query;
  if (schema->options().get("worktree_of", &src)) {
    my_error(ER_INTERNAL_ERROR, MYF(0), "CREATE WORKTREE: lineage");
    return nullptr;
  }

  /* Remap src.x only if the worktree has x (a table, view or routine). */
  std::vector<dd::String_type> names, procs, funcs;
  if (dc.fetch_schema_component_names<dd::Abstract_table>(schema, &names) ||
      dc.fetch_schema_component_names<dd::Procedure>(schema, &procs) ||
      dc.fetch_schema_component_names<dd::Function>(schema, &funcs))
    return nullptr;
  names.insert(names.end(), procs.begin(), procs.end());
  names.insert(names.end(), funcs.begin(), funcs.end());
  const auto exists = [&](const std::string &name) {
    for (const auto &n : names) {
      if (my_strcasecmp(table_alias_charset, n.c_str(), name.c_str()) == 0)
        return true;
    }
    return false;
  };
  std::string text(query, *query_len);
  int remapped = 0;
  for (size_t b = 0; b <= src.size();) {
    size_t e = src.find(',', b);
    if (e == dd::String_type::npos) e = src.size();
    const std::string ancestor(src.c_str() + b, e - b);
    const Remap_result r = remap_schema(text, ancestor.c_str(), db.str,
                                        thd->variables.sql_mode, exists);
    remapped += r.remapped;
    text = r.text;
    b = e + 1;
  }
  if (remapped == 0) return query;
  const Remap_result r{text, remapped};
  char *out = thd->strmake(r.text.c_str(), r.text.size());
  if (out == nullptr) return nullptr;  // OOM is reported by strmake
  *query_len = r.text.size();
  return out;
}
