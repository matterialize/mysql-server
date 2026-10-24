/* Copyright (c) 2026, MySQL worktrees contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef SQL_WORKTREE_INCLUDED
#define SQL_WORKTREE_INCLUDED

#include "lex_string.h"
#include "my_sqlcommand.h"
#include "sql/sql_cmd_ddl.h"

#include <functional>
#include <string>

class THD;

/** Result of splicing a schema name in SQL text. */
struct Remap_result {
  std::string text;
  int remapped{0};
  bool literal_with_src{false};  ///< "src." inside a string literal
  bool literal_is_src{false};    ///< the literal 'src' (schema name as data)
  bool src_as_name{false};       ///< `src` used other than as a qualifier
};

/**
  Replace every identifier naming schema src that qualifies another identifier
  (src.t, `src`.`t`.`c`) with dst, keeping its quoting. String literals and
  comments are not touched (versioned comments are code). If exists is given,
  only src.x with exists(x) is remapped.
*/
Remap_result remap_schema(
    const std::string &in, const char *src, const char *dst,
    unsigned long long sql_mode,
    const std::function<bool(const std::string &)> &exists = nullptr);

/**
  PREPARE inside a stored program of a worktree: remap src. references to
  objects the worktree has. Returns query, or a remapped copy on thd's
  mem_root (query_len updated).
*/
const char *worktree_remap_dynamic_sql(THD *thd, const char *query,
                                       size_t *query_len);

/** Keep the cache of which schemas are worktrees current (CREATE WORKTREE,
DROP DATABASE). worktree_remap_dynamic_sql() returns nullptr, with the error
reported, when it cannot tell whether to remap: it fails closed. */
void worktree_schema_changed(const char *db, bool is_worktree);

/**
  CREATE WORKTREE wt FROM src: a new schema whose tables are copy-on-write
  clones of src's tables. The destination name is LEX::name, like CREATE
  DATABASE, so privilege checks and replication filters treat it the same.
*/
class Sql_cmd_create_worktree final : public Sql_cmd_ddl {
 public:
  explicit Sql_cmd_create_worktree(const LEX_CSTRING &src) : m_src(src) {}

  enum_sql_command sql_command_code() const override {
    return SQLCOM_CREATE_DB;
  }

  bool execute(THD *thd) override;

 private:
  LEX_CSTRING m_src;
};

/**
  ALTER WORKTREE <db> MATERIALIZE: make every overlay file of the schema a
  standalone stock-format file (limits/overlay-v3.md §8).
*/
class Sql_cmd_materialize_worktree final : public Sql_cmd_ddl {
 public:
  enum_sql_command sql_command_code() const override {
    return SQLCOM_ALTER_DB;
  }

  bool execute(THD *thd) override;
};

/**
  For every table of schema db: the engine's worktree refresh (before a
  read-only schema becomes writable) or materialize operation.
  @retval true failure, my_error() was called
*/
bool worktree_tables_op(THD *thd, const char *db, bool refresh);

#endif /* SQL_WORKTREE_INCLUDED */
