/* Copyright (c) 2026, MySQL worktrees contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation. */

#ifndef SQL_WORKTREE_INCLUDED
#define SQL_WORKTREE_INCLUDED

#include "lex_string.h"
#include "my_sqlcommand.h"
#include "sql/sql_cmd_ddl.h"

class THD;

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

#endif /* SQL_WORKTREE_INCLUDED */
