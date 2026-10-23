/* Copyright (c) 2026, MySQL worktrees contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#include "sql/sql_worktree.h"

#include <memory>
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

  /* Views, routines, events and triggers can name the source schema in their
  bodies; remapping them is not implemented yet (fork-design.md §10.6). */
  {
    std::vector<const dd::View *> views;
    std::vector<const dd::Routine *> routines;
    std::vector<dd::String_type> events;
    if (dc.fetch_schema_components(src_schema, &views) ||
        dc.fetch_schema_components(src_schema, &routines) ||
        dc.fetch_schema_component_names<dd::Event>(src_schema, &events))
      return true;
    if (!views.empty())
      return unsupported("CREATE WORKTREE of a schema with views");
    if (!routines.empty())
      return unsupported("CREATE WORKTREE of a schema with stored routines");
    if (!events.empty())
      return unsupported("CREATE WORKTREE of a schema with events");
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
    if (t->partition_type() != dd::Table::PT_NONE)
      return unsupported("CREATE WORKTREE of partitioned tables");
    if (t->has_trigger())
      return unsupported("CREATE WORKTREE of a schema with triggers");
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

      if (dc.store(dst.get())) return true;
    }

    /* The deserialized parents do not know about their (also new) children
    yet; rebuild that from the stored foreign keys. */
    for (const auto &name : names) {
      if (adjust_fk_parents(thd, wt, name.c_str(), true, nullptr)) return true;
    }

    DBUG_EXECUTE_IF("wt_crash_before_commit", DBUG_SUICIDE(););
    return false;
  };

  if (mysql_create_db(thd, wt, &create_info, populate)) return true;

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
