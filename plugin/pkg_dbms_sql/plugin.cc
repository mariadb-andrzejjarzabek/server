/*
   Copyright (c) 2019, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#define MYSQL_SERVER

#include <my_global.h>
#include <sql_class.h>
#include <mysql/plugin_function.h>
#include <sql_prepare.h>
#include <sp_head.h>
#include <sp_rcontext.h>
#include <sp_cache.h>


/*** General purpose routines ************************************************/


/*
  SQL_SET_PARAM_BY_NAME(ps_name VARCHAR,
                        param_name VARCHAR,
                        value <datatype>)
*/
class Item_func_sql_set_param_by_name: public Item_bool_func
{
public:
  using Item_bool_func::Item_bool_func;
  using Create= TFuncPluginCreate<Create_func_arg3,
                                  Item_func_sql_set_param_by_name,
                                  Create_func::TYPE_PACKAGE_PUBLIC_PROCEDURE>;

  bool set_param_by_name(THD *thd, const Lex_ident_sys &ps_name,
                         Item *param_name_item, Item *param_value_item)
  {
    StringBuffer<64> param_name_value_buffer, param_name_conv_buffer;
    String *param_name= param_name_item->val_str(&param_name_value_buffer,
                                                 &param_name_conv_buffer,
                                                 system_charset_info);
    if ((null_value= param_name == nullptr))
      return 0;
    param_name->c_ptr(); // Lex_ident_column expects a 0-terminated string
    null_value= mysql_sql_stmt_set_placeholder_by_name(thd,
                   ps_name,
                   Lex_ident_column(param_name->to_lex_cstring()),
                   param_value_item);
    return false;
  }
  bool val_bool() override
  {
    StringBuffer<64> ps_name_value_buffer, ps_name_conv_buffer;
    String *ps_name= args[0]->val_str(&ps_name_value_buffer,
                                      &ps_name_conv_buffer,
                                      system_charset_info);
    if ((null_value= ps_name == nullptr))
      return 0;
    return set_param_by_name(current_thd, Lex_ident_sys(ps_name->ptr(),
                                                        ps_name->length()),
                             args[1], args[2]);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "sql_set_param_by_name"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_sql_set_param_by_name>(thd, this);
  }
};


/*
  SQL_COLUMN_VALUE(sys_refcursor_id INTEGER,
                   position INTEGER,
                   destination OUT <datatype>)
*/
class Item_func_sql_column_value: public Item_bool_func
{
public:
  using Item_bool_func::Item_bool_func;
  using Create= TFuncPluginCreate<Create_func_arg3,
                                  Item_func_sql_column_value,
                                  Create_func::TYPE_PACKAGE_PUBLIC_PROCEDURE>;

  bool check_arguments() const override
  {
    if (Item_bool_func::check_arguments())
      return true;
    Settable_routine_parameter *dst= args[2]->get_settable_routine_parameter();
    if (!dst)
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
      return true;
    }
    return false;
  }
  bool column_value(THD *thd, Item *sys_refcursor_id_item,
                    Item *pos_item, Item *dst_item)
  {
    Longlong_hybrid_null id= sys_refcursor_id_item->to_longlong_hybrid_null();
    if ((null_value= id.is_null() || id.neg() ||
         thd->statement_cursors()->elements() <= (size_t) id.value() ||
         !thd->statement_cursors()->at(id.value()).is_open()))
    {
      my_error(ER_SP_CURSOR_MISMATCH, MYF(0), id.is_null() ? "NULL" :
                                              ErrConvInteger(id).ptr());
      return true;
    }
    sp_cursor_array_element *cursor= &thd->statement_cursors()->at(id.value());
    longlong pos= pos_item->val_int();
    if ((null_value= pos_item->null_value || pos < 1 || pos > cursor->cols()))
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
      return false;
    }
    Settable_routine_parameter *dst= dst_item->get_settable_routine_parameter();
    DBUG_ASSERT(dst);
    null_value= cursor->column_value(thd, (uint) pos - 1, dst);
    return false;
  }

  bool val_bool() override
  {
    return column_value(current_thd, args[0], args[1], args[2]);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "sql_column_value"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_sql_column_value>(thd, this);
  }
};



/*** DBMS_SQL specific routines **********************************************/

/*
  DBMS_SQL.BIND_VARIABLE (c     IN INTEGER,
                          name  IN VARCHAR2,
                          value IN <datatype>);
*/
class Item_func_dbms_sql_bind_variable: public Item_func_sql_set_param_by_name
{
public:
  using Item_func_sql_set_param_by_name::Item_func_sql_set_param_by_name;
  using Create= TFuncPluginCreate<Create_func_arg3,
                                  Item_func_dbms_sql_bind_variable,
                                  Create_func::TYPE_PACKAGE_PUBLIC_PROCEDURE>;

  bool val_bool() override
  {
    THD *thd= current_thd;
    // Evaluate DBMS_SQL cursor ID
    Longlong_null dbms_sql_cursor_id= args[0]->to_longlong_null();
    if ((null_value= dbms_sql_cursor_id.is_null() ||
                     dbms_sql_cursor_id.value() < 0))
      return true;
    // Make DBMS_SQL prepared statement name
    char buff[NAME_LEN + 1];
    Lex_ident_sys ps_name= {buff, 0};
    ps_name.length= my_snprintf(buff, sizeof(buff), "_dbms_sql_%lld",
                                dbms_sql_cursor_id.value());
    return set_param_by_name(thd, ps_name, args[1], args[2]);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "dbms_sql_bind_variable"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_dbms_sql_bind_variable>(thd, this);
  }
};


/*
   DBMS_SQL.COLUMN_VALUE(c                IN  INTEGER,
                         position         IN  INTEGER,
                         value            OUT <datatype>
                         [,column_error   OUT NUMBER]    -- Not yet
                         [,actual_length  OUT INTEGER]); -- Not yet
*/
class Item_func_dbms_sql_column_value: public Item_func_sql_column_value
{
public:
  using Item_func_sql_column_value::Item_func_sql_column_value;
  using Create= TFuncPluginCreate<Create_func_arg3,
                                  Item_func_dbms_sql_column_value,
                                  Create_func::TYPE_PACKAGE_PUBLIC_PROCEDURE>;

  sp_package *find_package_body_dbms_sql(THD *thd)
  {
    if (!thd->spcont)
      return nullptr;
    Database_qualified_name tmp(thd->spcont->m_sp->m_db,
                                "DBMS_SQL"_LEX_CSTRING);
    sp_head *sp= sp_cache_lookup(&thd->sp_package_body_cache, &tmp);
    sp_package *pkg= sp ? sp->get_package() : nullptr;
    return pkg;
  }

  bool val_bool() override
  {
    THD *thd= current_thd;
    StringBuffer<64> buffer;
    String *args0str;
    sp_package *pkg= find_package_body_dbms_sql(thd);
    Item_field *assoc;
    Item_composite_base *assoc1;
    Item *assoc_row;
    Item *sys_refcursor_id_item;
    /*
      Evaluate the DBMS_SQL cursor ID from args[0] and convert it into
      SYS_REFCURSOR cursor ID. The below code does effectively
      the same thing with this PL/SQL code:
        sys_refcursor_id:= assoc0(dbms_sql_cursor_id).cursor_id;

      For safety, let's check that:
      - The name of get_variable(0) is "assoc0"
      - The second field in "assoc0" is "cursor_id"
      This is how 'CREATE PACKAGE BODY DBMS_SQL' is defined.

      Return an error if the variable assoc0 does not exist on the
      expected position, or if "cursor_id" is not found inside its element
      on the expected position. In such cases we cannot proceed.
    */
    if ((null_value= !pkg || pkg->m_rcontext->max_var_index() < 1 ||
                     !(assoc1= dynamic_cast<Item_composite_base *>
                              (assoc= pkg->m_rcontext->get_variable(0))) ||
                     !assoc->name.streq("assoc0"_Lex_ident_column) ||
                     !(args0str= args[0]->val_str(&buffer)) ||
                     !(assoc_row= assoc1->element_by_key(thd, args0str)) ||
                     !(sys_refcursor_id_item= assoc_row->element_index(1))) ||
                     !sys_refcursor_id_item->name.streq(
                                                 "cursor_id"_Lex_ident_column))
    {
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }
    return column_value(thd, sys_refcursor_id_item, args[1], args[2]);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "dbms_sql_column_value"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_dbms_sql_column_value>(thd, this);
  }
};


/*************************************************************************/

maria_declare_plugin(pkg_dbms_sql)
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_sql_set_param_by_name::Create::plugin_descriptor(),
  "sql_set_param_by_name",      // plugin name
  "MariaDB Corporation",        // plugin author
  "Function SQL_SET_PARAM_BY_NAME()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_sql_column_value::Create::plugin_descriptor(),
  "sql_column_value",           // plugin name
  "MariaDB Corporation",        // plugin author
  "Function SQL_COLUMN_VALUE()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_dbms_sql_bind_variable::Create::plugin_descriptor(),
  "dbms_sql_bind_variable",     // plugin name
  "MariaDB Corporation",        // plugin author
  "Function DBMS_SQL_BIND_VARIABLE()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_dbms_sql_column_value::Create::plugin_descriptor(),
  "dbms_sql_column_value",      // plugin name
  "MariaDB Corporation",        // plugin author
  "Function DBMS_SQL_COLUMN_VALUE()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
}

maria_declare_plugin_end;
