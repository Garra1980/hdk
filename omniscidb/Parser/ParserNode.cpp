/**
 * @file		ParserNode.cpp
 * @author	Wei Hong <wei@map-d.com>
 * @brief		Functions for ParserNode classes
 * 
 * Copyright (c) 2014 MapD Technologies, Inc.  All rights reserved.
 **/

#include <cassert>
#include <stdexcept>
#include <typeinfo>
#include <boost/algorithm/string/predicate.hpp>
#include "../Catalog/Catalog.h"
#include "ParserNode.h"
#include "../Planner/Planner.h"
#include "parser.h"

namespace Parser {
	SubqueryExpr::~SubqueryExpr() 
	{
		delete query;
	}

	ExistsExpr::~ExistsExpr() 
	{
		delete query;
	}

	InValues::~InValues()
	{
		for (auto p : *value_list)
			delete p;
		delete value_list;
	}

	BetweenExpr::~BetweenExpr() 
	{
		delete arg;
		delete lower;
		delete upper;
	}

	LikeExpr::~LikeExpr() 
	{
		delete arg;
		delete like_string;
		if (escape_string != nullptr)
			delete escape_string;
	}

	ColumnRef::~ColumnRef() 
	{
		if (table != nullptr)
			delete table;
		if (column != nullptr)
			delete column;
	}

	FunctionRef::~FunctionRef() 
	{
		delete name;
		if (arg != nullptr)
			delete arg;
	}
	
	TableRef::~TableRef() 
	{
		delete table_name;
		if (range_var != nullptr)
			delete range_var;
	}

	ColumnConstraintDef::~ColumnConstraintDef() 
	{
		if (defaultval != nullptr)
			delete defaultval;
		if (check_condition != nullptr)
			delete check_condition;
		if (foreign_table != nullptr)
			delete foreign_table;
		if (foreign_column != nullptr)
			delete foreign_column;
	}

	ColumnDef::~ColumnDef() 
	{
		delete column_name;
		delete column_type;
		if (compression != nullptr)
			delete compression;
		if (column_constraint != nullptr)
			delete column_constraint;
	}

	UniqueDef::~UniqueDef() 
	{
		for (auto p : *column_list)
			delete p;
		delete column_list;
	}

	ForeignKeyDef::~ForeignKeyDef() 
	{
		for (auto p : *column_list)
			delete p;
		delete column_list;
		delete foreign_table;
		if (foreign_column_list != nullptr) {
			for (auto p : *foreign_column_list)
				delete p;
			delete foreign_column_list;
		}
	}

	CreateTableStmt::~CreateTableStmt() 
	{
		delete table;
		for (auto p : *table_element_list)
			delete p;
		delete table_element_list;
	}

	SelectEntry::~SelectEntry()
	{
		delete select_expr;
		if (alias != nullptr)
			delete alias;
	}

	QuerySpec::~QuerySpec() 
	{
		if (select_clause != nullptr) {
			for (auto p : *select_clause)
				delete p;
			delete select_clause;
		}
		for (auto p : *from_clause)
			delete p;
		delete from_clause;
		if (where_clause != nullptr)
			delete where_clause;
		if (groupby_clause != nullptr)
			delete groupby_clause;
		if (having_clause != nullptr)
			delete having_clause;
	}

	SelectStmt::~SelectStmt()
	{
		delete query_expr;
		if (orderby_clause != nullptr) {
			for (auto p : *orderby_clause)
				delete p;
			delete orderby_clause;
		}
	}

	CreateViewStmt::~CreateViewStmt() 
	{
		delete view_name;
		if (column_list != nullptr) {
			for (auto p : *column_list)
				delete p;
			delete column_list;
		}
		delete query;
		if (matview_options != nullptr) {
			for (auto p : *matview_options)
				delete p;
			delete matview_options;
		}
	}

	InsertStmt::~InsertStmt()
	{
		delete table;
		if (column_list != nullptr) {
			for (auto p : *column_list)
				delete p;
			delete column_list;
		}
	}

	InsertValuesStmt::~InsertValuesStmt()
	{
		for (auto p : *value_list)
			delete p;
		delete value_list;
	}

	UpdateStmt::~UpdateStmt()
	{
		delete table;
		for (auto p : *assignment_list)
			delete p;
		delete assignment_list;
		if (where_clause != nullptr)
			delete where_clause;
	}

	DeleteStmt::~DeleteStmt()
	{
		delete table;
		if (where_clause != nullptr)
			delete where_clause;
	}
	
	Analyzer::Expr *
	NullLiteral::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		Analyzer::Constant *c = new Analyzer::Constant(kNULLT, true);
		Datum d;
		d.pointerval = nullptr;
		c->set_constval(d);
		return c;
	}
	
	Analyzer::Expr *
	StringLiteral::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		SQLTypeInfo ti;
		ti.type = kVARCHAR;
		ti.dimension = stringval->length();
		ti.scale = 0;
		char *s = new char[stringval->length() + 1];
		strcpy(s, stringval->c_str());
		Datum d;
		d.pointerval = (void*)s;
		return new Analyzer::Constant(ti, false, d);
	}

	Analyzer::Expr *
	IntLiteral::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		SQLTypes t;
		Datum d;
		if (intval >= INT16_MIN && intval <= INT16_MAX) {
			t = kSMALLINT;
			d.smallintval = (int16_t)intval;
		} else if (intval >= INT32_MIN && intval <= INT32_MAX) {
			t = kINT;
			d.intval = (int32_t)intval;
		} else {
			t = kBIGINT;
			d.bigintval = intval;
		}
		Analyzer::Constant *c = new Analyzer::Constant(t, false, d);
		return c;
	}

	Analyzer::Expr *
	FixedPtLiteral::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		assert(fixedptval->length() <= 20);
		size_t dot = fixedptval->find_first_of('.', 0);
		assert(dot != std::string::npos);
		std::string before_dot = fixedptval->substr(0, dot);
		std::string after_dot = fixedptval->substr(dot+1);
		Datum d;
		d.bigintval = std::stoll(before_dot);
		int64_t fraction = std::stoll(after_dot);
		SQLTypeInfo ti;
		ti.type = kNUMERIC;
		ti.scale = after_dot.length();
		ti.dimension = before_dot.length() + ti.scale;
		// the following loop can be made more efficient if needed
		for (int i = 0; i < ti.scale; i++)
			d.bigintval *= 10;
		d.bigintval += fraction;
		return new Analyzer::Constant(ti, false, d);
	}

	Analyzer::Expr *
	FloatLiteral::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		Datum d;
		d.floatval = floatval;
		return new Analyzer::Constant(kFLOAT, false, d);
	}
	
	Analyzer::Expr *
	DoubleLiteral::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		Datum d;
		d.doubleval = doubleval;
		return new Analyzer::Constant(kDOUBLE, false, d);
	}

	Analyzer::Expr *
	UserLiteral::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		throw std::runtime_error("USER literal not supported yet.");
		return nullptr;
	}

	Analyzer::Expr *
	OperExpr::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		SQLTypeInfo result_type, left_type, right_type;
		SQLTypeInfo new_left_type, new_right_type;
		Analyzer::Expr *left_expr, *right_expr;
		SQLQualifier qual = kONE;
		if (typeid(*right) == typeid(SubqueryExpr))
			qual = dynamic_cast<SubqueryExpr*>(right)->get_qualifier();
		left_expr = left->analyze(catalog, query);
		right_expr = right->analyze(catalog, query);
		left_type = left_expr->get_type_info();
		right_type = right_expr->get_type_info();
		result_type = Analyzer::BinOper::analyze_type_info(optype, left_type, right_type, &new_left_type, &new_right_type);
		if (left_type != new_left_type)
			left_expr = left_expr->add_cast(new_left_type);
		if (right_type != new_right_type)
			right_expr = right_expr->add_cast(new_right_type);
		return new Analyzer::BinOper(result_type, optype, qual, left_expr, right_expr);
	}

	Analyzer::Expr *
	SubqueryExpr::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		throw std::runtime_error("Subqueries are not supported yet.");
		return nullptr;
	}

	Analyzer::Expr *
	IsNullExpr::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		Analyzer::Expr *arg_expr = arg->analyze(catalog, query);
		Analyzer::Expr *result = new Analyzer::UOper(kBOOLEAN, kISNULL, arg_expr);
		if (is_not)
			result = new Analyzer::UOper(kBOOLEAN, kNOT, result);
		return result;
	}

	Analyzer::Expr *
	InSubquery::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		throw std::runtime_error("Subqueries are not supported yet.");
		return nullptr;
	}

	Analyzer::Expr *
	InValues::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		Analyzer::Expr *arg_expr = arg->analyze(catalog, query);
		std::list<Analyzer::Expr*> *value_exprs = new std::list<Analyzer::Expr*>();
		for (auto p : *value_list) {
			Analyzer::Expr *e = p->analyze(catalog, query);
			value_exprs->push_back(e->add_cast(arg_expr->get_type_info()));
		}
		Analyzer::Expr *result = new Analyzer::InValues(arg_expr, value_exprs);
		if (is_not)
			result = new Analyzer::UOper(kBOOLEAN, kNOT, result);
		return result;
	}

	Analyzer::Expr *
	BetweenExpr::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		Analyzer::Expr *arg_expr = arg->analyze(catalog, query);
		Analyzer::Expr *lower_expr = lower->analyze(catalog, query);
		Analyzer::Expr *upper_expr = upper->analyze(catalog, query);
		SQLTypeInfo new_left_type, new_right_type;
		(void)Analyzer::BinOper::analyze_type_info(kGE, arg_expr->get_type_info(), lower_expr->get_type_info(), &new_left_type, &new_right_type);
		Analyzer::BinOper *lower_pred = new Analyzer::BinOper(kBOOLEAN, kGE, kONE, arg_expr->add_cast(new_left_type), lower_expr->add_cast(new_right_type));
		(void)Analyzer::BinOper::analyze_type_info(kLE, arg_expr->get_type_info(), lower_expr->get_type_info(), &new_left_type, &new_right_type);
		Analyzer::BinOper *upper_pred = new Analyzer::BinOper(kBOOLEAN, kLE, kONE, arg_expr->deep_copy()->add_cast(new_left_type), upper_expr->add_cast(new_right_type));
		Analyzer::Expr *result = new Analyzer::BinOper(kBOOLEAN, kAND, kONE, lower_pred, upper_pred);
		if (is_not)
			result = new Analyzer::UOper(kBOOLEAN, kNOT, result);
		return result;
	}

	Analyzer::Expr *
	LikeExpr::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		Analyzer::Expr *arg_expr = arg->analyze(catalog, query);
		Analyzer::Expr *like_expr = like_string->analyze(catalog, query);
		Analyzer::Expr *escape_expr = escape_string == nullptr ? nullptr: escape_string->analyze(catalog, query);
		if (!IS_STRING(arg_expr->get_type_info().type))
			throw std::runtime_error("expression before LIKE must be of a string type.");
		if (!IS_STRING(like_expr->get_type_info().type))
			throw std::runtime_error("expression after LIKE must be of a string type.");
		if (escape_expr != nullptr && !IS_STRING(escape_expr->get_type_info().type))
			throw std::runtime_error("expression after ESCAPE must be of a string type.");
		Analyzer::Expr *result = new Analyzer::LikeExpr(arg_expr, like_expr, escape_expr);
		if (is_not)
			result = new Analyzer::UOper(kBOOLEAN, kNOT, result);
		return result;
	}

	Analyzer::Expr *
	ExistsExpr::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		throw std::runtime_error("Subqueries are not supported yet.");
		return nullptr;
	}

	Analyzer::Expr *
	ColumnRef::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		int table_id;
		int rte_idx;
		const ColumnDescriptor *cd;
		if (column == nullptr)
			throw std::runtime_error("invalid column name *.");
		if (table != nullptr) {
			rte_idx = query.get_rte_idx(*table);
			if (rte_idx < 0)
				throw std::runtime_error("range variable or table name " + *table + " does not exist.");
			Analyzer::RangeTblEntry *rte = query.get_rte(rte_idx);
			cd = rte->get_column_desc(catalog, *column);
			if (cd == nullptr)
				throw std::runtime_error("Column name " + *column + " does not exist.");
			table_id = rte->get_table_id();
		} else {
			bool found = false;
			int i = 0;
			for (auto rte : query.get_rangetable()) {
				cd = rte->get_column_desc(catalog, *column);
				if (cd != nullptr && !found) {
					found = true;
					rte_idx = i;
					table_id = rte->get_table_id();
				} else if (cd != nullptr && found)
					throw std::runtime_error("Column name " + *column + " is ambiguous.");
				i++;
			}
			if (cd == nullptr)
				throw std::runtime_error("Column name " + *column + " does not exist.");
		}
		return new Analyzer::ColumnVar(cd->columnType, table_id, cd->columnId, rte_idx, cd->compression, cd->comp_param);
	}

	Analyzer::Expr *
	FunctionRef::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const 
	{
		SQLTypeInfo result_type;
		SQLAgg agg_type;
		Analyzer::Expr *arg_expr;
		bool is_distinct = false;
		if (boost::iequals(*name, "count")) {
			result_type.type = kINT;
			agg_type = kCOUNT;
			if (arg == nullptr)
				arg_expr = nullptr;
			else
				arg_expr = arg->analyze(catalog, query);
			is_distinct = distinct;
		}
		else if (boost::iequals(*name, "min")) {
			agg_type = kMIN;
			arg_expr = arg->analyze(catalog, query);
			result_type = arg_expr->get_type_info();
		}
		else if (boost::iequals(*name, "max")) {
			agg_type = kMAX;
			arg_expr = arg->analyze(catalog, query);
			result_type = arg_expr->get_type_info();
		}
		else if (boost::iequals(*name, "avg")) {
			agg_type = kAVG;
			arg_expr = arg->analyze(catalog, query);
			result_type = arg_expr->get_type_info();
		}
		else if (boost::iequals(*name, "sum")) {
			agg_type = kSUM;
			arg_expr = arg->analyze(catalog, query);
			result_type = arg_expr->get_type_info();
		}
		else
			throw std::runtime_error("invalid function name: " + *name);
		int naggs = query.get_num_aggs();
		query.set_num_aggs(naggs+1);
		return new Analyzer::AggExpr(result_type, agg_type, arg_expr, is_distinct);
	}

	void
	UnionQuery::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		left->analyze(catalog, query);
		Analyzer::Query *right_query = new Analyzer::Query();
		right->analyze(catalog, *right_query);
		query.set_next_query(right_query);
		query.set_is_unionall(is_unionall);
	}

	void
	QuerySpec::analyze_having_clause(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		Analyzer::Expr *p = nullptr;
		if (having_clause != nullptr) {
			p = having_clause->analyze(catalog, query);
			if (p->get_type_info().type != kBOOLEAN)
				throw std::runtime_error("Only boolean expressions can be in HAVING clause.");
			p->check_group_by(query.get_group_by());
	  }
		query.set_having_predicate(p);
	}

	void
	QuerySpec::analyze_group_by(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		std::list<Analyzer::Expr*> *groupby = nullptr;
		if (groupby_clause != nullptr) {
			groupby = new std::list<Analyzer::Expr*>();
			for (auto c : *groupby_clause) {
				Analyzer::Expr *e = c->analyze(catalog, query);
				groupby->push_back(e);
			}
		}
		if (query.get_num_aggs() > 0 || groupby != nullptr)
			for (auto t : query.get_targetlist()) {
				Analyzer::Expr *e = t->get_expr();
				if (typeid(*e) != typeid(Analyzer::AggExpr))
					e->check_group_by(groupby);
			}
		query.set_group_by(groupby);
	}

	void
	QuerySpec::analyze_where_clause(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		if (where_clause == nullptr) {
			query.set_where_predicate(nullptr);
			return;
		}
		Analyzer::Expr *p = where_clause->analyze(catalog, query);
		if (p->get_type_info().type != kBOOLEAN)
			throw std::runtime_error("Only boolean expressions can be in WHERE clause.");
		query.set_where_predicate(p);
	}

	void
	QuerySpec::analyze_select_clause(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		std::list<Analyzer::TargetEntry*> &tlist = query.get_targetlist_nonconst();
		if (select_clause == nullptr) {
			// this means SELECT *
			int rte_idx = 0;
			for (auto rte : query.get_rangetable()) {
				rte->expand_star_in_targetlist(catalog, tlist, rte_idx++);
			}
		}
		else {
			for (auto p : *select_clause) {
				const Parser::Expr *select_expr = p->get_select_expr();
				// look for the case of range_var.*
				if (typeid(*select_expr) == typeid(ColumnRef) &&
						dynamic_cast<const ColumnRef*>(select_expr)->get_column() == nullptr) {
						const std::string *range_var_name = dynamic_cast<const ColumnRef*>(select_expr)->get_table();
						int rte_idx = query.get_rte_idx(*range_var_name);
						if (rte_idx < 0)
							throw std::runtime_error("invalid range variable name: " + *range_var_name);
						Analyzer::RangeTblEntry *rte = query.get_rte(rte_idx);
						rte->expand_star_in_targetlist(catalog, tlist, rte_idx);
				}
				else {
					Analyzer::Expr *e = select_expr->analyze(catalog, query);
					std::string resname;

					if (p->get_alias() != nullptr)
						resname = *p->get_alias();
					else if (typeid(*e) == typeid(Analyzer::ColumnVar)) {
						Analyzer::ColumnVar *colvar = dynamic_cast<Analyzer::ColumnVar*>(e);
						const ColumnDescriptor *col_desc = catalog.getMetadataForColumn(colvar->get_table_id(), colvar->get_column_id());
						resname = col_desc->columnName;
					}
					Analyzer::TargetEntry *tle = new Analyzer::TargetEntry(resname, e);
					tlist.push_back(tle);
				}
			}
		}
	}

	void
	QuerySpec::analyze_from_clause(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		Analyzer::RangeTblEntry *rte;
		for (auto p : *from_clause) {
			const TableDescriptor *table_desc;
			table_desc = catalog.getMetadataForTable(*p->get_table_name());
			if (table_desc == nullptr)
				throw std::runtime_error("Table " + *p->get_table_name() + " does not exist." );
			if (table_desc->isView && !table_desc->isMaterialized)
				throw std::runtime_error("Non-materialized view " + *p->get_table_name() + " is not supported yet.");
			std::string range_var;
			if (p->get_range_var() == nullptr)
				range_var = *p->get_table_name();
			else
				range_var = *p->get_range_var();
			rte = new Analyzer::RangeTblEntry(range_var, table_desc, nullptr);
			query.add_rte(rte);
		}
	}

	void
	QuerySpec::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		query.set_is_distinct(is_distinct);
		analyze_from_clause(catalog, query);
		analyze_select_clause(catalog, query);
		analyze_where_clause(catalog, query);
		analyze_group_by(catalog, query);
		analyze_having_clause(catalog, query);
	}

	void
	SelectStmt::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		query.set_stmt_type(kSELECT);
		query_expr->analyze(catalog, query);
		if (orderby_clause == nullptr) {
			query.set_order_by(nullptr);
			return;
		}
		const std::list<Analyzer::TargetEntry*> &tlist = query.get_targetlist();
		std::list<Analyzer::OrderEntry> *order_by = new std::list<Analyzer::OrderEntry>();
		for (auto p : *orderby_clause) {
			int tle_no = p->get_colno();
			if (tle_no == 0) {
				// use column name
				// search through targetlist for matching name
				const std::string *name = p->get_column()->get_column();
				tle_no = 1;
				bool found = false;
				for (auto tle : tlist) {
					if (tle->get_resname() == *name) {
						found = true;
						break;
					}
					tle_no++;
				}
				if (!found)
					throw std::runtime_error("invalid name in order by: " + *name);
			}
			order_by->push_back(Analyzer::OrderEntry(tle_no, p->get_is_desc(), p->get_nulls_first()));
		}
		query.set_order_by(order_by);
	}

	std::string
	SelectEntry::to_string() const
	{
		std::string str = select_expr->to_string();
		if (alias != nullptr)
			str += "AS " + *alias;
		return str;
	}

	std::string
	TableRef::to_string() const
	{
		std::string str = *table_name;
		if (range_var != nullptr)
			str += " " + *range_var;
		return str;
	}

	std::string
	ColumnRef::to_string() const
	{
		std::string str;
		if (table == nullptr)
			str = *column;
		else if (column == nullptr)
			str = *table + ".*";
		else
			str = *table + "." + *column;
		return str;
	}

	std::string
	OperExpr::to_string() const
	{
		std::string op_str[] = { "=", "<>", "<", ">", "<=", ">=", " AND ", " OR ", "NOT", "-", "+", "*", "/" };
		std::string str;
		if (optype == kUMINUS)
			str = "-(" + left->to_string() + ")";
		else if (optype == kNOT)
			str = "NOT (" + left->to_string() + ")";
		else
			str = "(" + left->to_string() + op_str[optype] + right->to_string() + ")";
		return str;
	}

	std::string
	InExpr::to_string() const
	{
		std::string str = arg->to_string();
		if (is_not)
			str += " NOT IN ";
		else
			str += " IN ";
		return str;
	}

	std::string
	ExistsExpr::to_string() const
	{
		return "EXISTS (" + query->to_string() + ")";
	}

	std::string
	SubqueryExpr::to_string() const
	{
		std::string str;
		if (qualifier == kANY)
			str = "ANY (";
		else if (qualifier == kALL)
			str = "ALL (";
		else
			str = "(";
		str += query->to_string();
		str += ")";
		return str;
	}

	std::string
	IsNullExpr::to_string() const
	{
		std::string str = arg->to_string();
		if (is_not)
			str += " IS NOT NULL";
		else
			str += " IS NULL";
		return str;
	}

	std::string
	InSubquery::to_string() const
	{
		std::string str = InExpr::to_string();
		str += subquery->to_string();
		return str;
	}

	std::string
	InValues::to_string() const
	{
		std::string str = InExpr::to_string() + "(";
		bool notfirst = false;
		for (auto p : *value_list) {
			if (notfirst)
				str += ", ";
			else
				notfirst = true;
			str += p->to_string();
		}
		str += ")";
		return str;
	}

	std::string
	BetweenExpr::to_string() const
	{
		std::string str = arg->to_string();
		if (is_not)
			str += " NOT BETWEEN ";
		else
			str += " BETWEEN ";
		str += lower->to_string() + " AND " + upper->to_string();
		return str;
	}

	std::string
	LikeExpr::to_string() const
	{
		std::string str = arg->to_string();
		if (is_not)
			str += " NOT LIKE ";
		else
			str += " LIKE ";
		str += like_string->to_string();
		if (escape_string != nullptr)
			str += " ESCAPE " + escape_string->to_string();
		return str;
	}

	std::string
	FunctionRef::to_string() const
	{
		std::string str = *name + "(";
		if (distinct)
			str += "DISTINCT ";
		if (arg == nullptr)
			str += "*)";
		else
			str += arg->to_string() + ")";
		return str;
	}

	std::string
	QuerySpec::to_string() const
	{
		std::string query_str = "SELECT ";
		if (is_distinct)
			query_str += "DISTINCT ";
		if (select_clause == nullptr)
			query_str += "* ";
		else {
			bool notfirst = false;
			for (auto p : *select_clause) {
				if (notfirst)
					query_str += ", ";
				else
					notfirst = true;
				query_str += p->to_string();
			}
		}
		query_str += " FROM ";
		bool notfirst = false;
		for (auto p : *from_clause) {
			if (notfirst)
				query_str += ", ";
			else
				notfirst = true;
			query_str += p->to_string();
		}
		if (where_clause != nullptr)
			query_str += " WHERE " + where_clause->to_string();
		if (groupby_clause != nullptr) {
			query_str += " GROUP BY ";
			bool notfirst = false;
			for (auto p : *groupby_clause) {
				if (notfirst)
					query_str += ", ";
				else
					notfirst = true;
				query_str += p->to_string();
			}
		}
		if (having_clause != nullptr) {
			query_str += " HAVING " + having_clause->to_string();
		}
		query_str += ";";
		return query_str;
	}

	void
	InsertStmt::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		query.set_stmt_type(kINSERT);
		const TableDescriptor *td = catalog.getMetadataForTable(*table);
		if (td == nullptr)
			throw std::runtime_error("Table " + *table + " does not exist.");
		if (td->isView && !td->isMaterialized)
			throw std::runtime_error("Insert to views is not supported yet.");
		Analyzer::RangeTblEntry *rte = new Analyzer::RangeTblEntry(*table, td, nullptr);
		query.set_result_table_id(td->tableId);
		std::list<int> result_col_list;
		if (column_list == nullptr) {
			const std::list<const ColumnDescriptor *> all_cols = catalog.getAllColumnMetadataForTable(td->tableId);
			for (auto cd : all_cols)
				result_col_list.push_back(cd->columnId);
		} else {
			for (auto c : *column_list) {
				const ColumnDescriptor *cd = catalog.getMetadataForColumn(td->tableId, *c);
				if (cd == nullptr)
					throw std::runtime_error("Column " + *c + " does not exist.");
				result_col_list.push_back(cd->columnId);
			}
		}
		query.set_result_col_list(result_col_list);
	}

	void
	InsertValuesStmt::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		InsertStmt::analyze(catalog, query);
		std::list<Analyzer::TargetEntry*> &tlist = query.get_targetlist_nonconst();
		for (auto v : *value_list) {
			Analyzer::Expr *e = v->analyze(catalog, query);
			tlist.push_back(new Analyzer::TargetEntry("", e));
		}
	}

	void
	InsertQueryStmt::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &insert_query) const
	{
		InsertStmt::analyze(catalog, insert_query);
		query->analyze(catalog, insert_query);
	}

	void
	UpdateStmt::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		throw std::runtime_error("UPDATE statement not supported yet.");
	}

	void
	DeleteStmt::analyze(const Catalog_Namespace::Catalog &catalog, Analyzer::Query &query) const
	{
		throw std::runtime_error("DELETE statement not supported yet.");
	}

	void
	CreateTableStmt::execute(Catalog_Namespace::Catalog &catalog)
	{
		if (catalog.getMetadataForTable(*table) != nullptr)
			throw std::runtime_error("Table " + *table + " already exits.");
		std::list<ColumnDescriptor> columns;
		for (auto e : *table_element_list) {
			if (typeid(*e) != typeid(ColumnDef))
				throw std::runtime_error("Table constraints are not supported yet.");
			ColumnDef *coldef = dynamic_cast<ColumnDef*>(e);
			ColumnDescriptor cd;
			cd.columnName = *coldef->get_column_name();
			const SQLType *t = coldef->get_column_type();
			cd.columnType.type = t->get_type();
			cd.columnType.dimension = t->get_param1();
			cd.columnType.scale = t->get_param2();
			const ColumnConstraintDef *cc = coldef->get_column_constraint();
			if (cc == nullptr)
				cd.columnType.notnull = false;
			else {
				cd.columnType.notnull = cc->get_notnull();
			}
			const CompressDef *compression = coldef->get_compression();
			if (compression == nullptr) {
				cd.compression = kENCODING_NONE;
				cd.comp_param = 0;
			} else {
				const std::string &comp = *compression->get_encoding_name();
				if (boost::iequals(comp, "fixed")) {
					// fixed-bits encoding
					if (compression->get_encoding_param() == 0 || compression->get_encoding_param() % 8 != 0 || compression->get_encoding_param() > 48)
						throw std::runtime_error("Must specify number of bits as 8, 16, 24, 32 or 48 as the parameter to fixed-bits encoding.");
					cd.compression = kENCODING_FIXED;
					cd.comp_param = compression->get_encoding_param();
				} else if (boost::iequals(comp, "rl")) {
					// run length encoding
					cd.compression = kENCODING_RL;
					cd.comp_param = 0;
				} else if (boost::iequals(comp, "diff")) {
					// differential encoding
					cd.compression = kENCODING_DIFF;
					cd.comp_param = 0;
				} else if (boost::iequals(comp, "dict")) {
					// diciontary encoding
					cd.compression = kENCODING_DICT;
					cd.comp_param = 0;
				} else if (boost::iequals(comp, "sparse")) {
					// sparse column encoding with mostly NULL values
					if (cd.columnType.notnull)
						throw std::runtime_error("Cannot do sparse column encoding on a NOT NULL column.");
					if (compression->get_encoding_param() == 0 || compression->get_encoding_param() % 8 != 0 || compression->get_encoding_param() > 48)
						throw std::runtime_error("Must specify number of bits as 8, 16, 24, 32 or 48 as the parameter to sparse-column encoding.");
					cd.compression = kENCODING_SPARSE;
					cd.comp_param = compression->get_encoding_param();
				} else
					throw std::runtime_error("Invalid column compression scheme " + comp);
			}
			columns.push_back(cd);
		}
		TableDescriptor td;
		td.tableName = *table;
		td.nColumns = columns.size();
		td.isView = false;
		td.isMaterialized = false;
		td.storageOption = kDISK;
		td.refreshOption = kMANUAL;
		td.checkOption = false;
		td.isReady = true;
		catalog.createTable(td, columns);
	}

	void
	DropTableStmt::execute(Catalog_Namespace::Catalog &catalog)
	{
		const TableDescriptor *td = catalog.getMetadataForTable(*table);
		if (td == nullptr)
			throw std::runtime_error("Table " + *table + " does not exist.");
		if (td->isView)
			throw std::runtime_error(*table + " is a view.  Use DROP VIEW.");
		catalog.dropTable(td);
	}

	void
	CreateViewStmt::execute(Catalog_Namespace::Catalog &catalog)
	{
		if (catalog.getMetadataForTable(*view_name) != nullptr)
			throw std::runtime_error("Table or View " + *view_name + " already exists.");
		ViewStorageOption matview_storage = kDISK;
		ViewRefreshOption matview_refresh = kMANUAL;
		if (matview_options != nullptr) {
			for (auto p : *matview_options) {
				if (boost::iequals(*p->get_name(), "storage")) {
					if (boost::iequals(*p->get_value(), "gpu") || boost::iequals(*p->get_value(), "mic"))
						matview_storage = kGPU;
					else if (boost::iequals(*p->get_value(), "cpu"))
						matview_storage = kCPU;
					else if (boost::iequals(*p->get_value(), "disk"))
						matview_storage = kDISK;
					else
						throw std::runtime_error("Invalid storage option " + *p->get_value() + ". Should be GPU, MIC, CPU or DISK.");
				} else if (boost::iequals(*p->get_name(), "refresh")) {
					if (boost::iequals(*p->get_value(), "auto"))
						matview_refresh = kAUTO;
					else if (boost::iequals(*p->get_value(), "manual"))
						matview_refresh = kMANUAL;
					else if (boost::iequals(*p->get_value(), "immediate"))
						matview_refresh = kIMMEDIATE;
					else
						throw std::runtime_error("Invalid refresh option " + *p->get_value() + ". Should be AUTO, MANUAL or IMMEDIATE.");
				} else
					throw std::runtime_error("Invalid CREATE MATERIALIZED VIEW option " + *p->get_name() + ".  Should be STORAGE or REFRESH.");
			}
		}
		Analyzer::Query analyzed_query;
		query->analyze(catalog, analyzed_query);
		const std::list<Analyzer::TargetEntry*> &tlist = analyzed_query.get_targetlist();
		// @TODO check column name uniqueness.  for now let sqlite enforce.
		if (column_list != nullptr) {
			if (column_list->size() != tlist.size())
				throw std::runtime_error("Number of column names does not match the number of expressions in SELECT clause.");
			std::list<std::string*>::iterator it = column_list->begin();
			for (auto tle : tlist) { 
				tle->set_resname(**it);
				++it;
			}
		}
		std::list<ColumnDescriptor> columns;
		for (auto tle : tlist) {
			ColumnDescriptor cd;
			if (tle->get_resname().empty())
				throw std::runtime_error("Must specify a column name for expression.");
			cd.columnName = tle->get_resname();
			cd.columnType = tle->get_expr()->get_type_info();
			cd.compression = kENCODING_NONE;
			cd.comp_param = 0;
			columns.push_back(cd);
		}
		TableDescriptor td;
		td.tableName = *view_name;
		td.nColumns = columns.size();
		td.isView = true;
		td.isMaterialized = is_materialized;
		td.viewSQL = query->to_string();
		td.checkOption = checkoption;
		td.storageOption = matview_storage;
		td.refreshOption = matview_refresh;
		td.isReady = !is_materialized;
		catalog.createTable(td, columns);
	}

	void
	RefreshViewStmt::execute(Catalog_Namespace::Catalog &catalog)
	{
		const TableDescriptor *td = catalog.getMetadataForTable(*view_name);
		if (td == nullptr)
			throw std::runtime_error("Materialied view " + *view_name + " does not exist.");
		if (!td->isView)
			throw std::runtime_error(*view_name + " is a table not a materialized view.");
		if (!td->isMaterialized)
			throw std::runtime_error(*view_name + " is not a materialized view.");
		SQLParser parser;
		std::list<Stmt*> parse_trees;
		std::string last_parsed;
		std::string query_str = "INSERT INTO " + *view_name + " " + td->viewSQL;
		int numErrors = parser.parse(query_str, parse_trees, last_parsed);
		if (numErrors > 0)
			throw std::runtime_error("Internal Error: syntax error at: " + last_parsed);
		DMLStmt *view_stmt = dynamic_cast<DMLStmt*>(parse_trees.front());
		unique_ptr<Stmt> stmt_ptr(view_stmt); // make sure it's deleted
		Analyzer::Query query;
		view_stmt->analyze(catalog, query);
		Planner::Optimizer optimizer(query, catalog);
		Planner::RootPlan *plan = optimizer.optimize();
		unique_ptr<Planner::RootPlan> plan_ptr(plan); // make sure it's deleted
		// @TODO execute plan
		// plan->print();
	}

	void
	DropViewStmt::execute(Catalog_Namespace::Catalog &catalog)
	{
		const TableDescriptor *td = catalog.getMetadataForTable(*view_name);
		if (td == nullptr)
			throw std::runtime_error("View " + *view_name + " does not exist.");
		if (!td->isView)
			throw std::runtime_error(*view_name + " is a table.  Use DROP TABLE.");
		catalog.dropTable(td);
	}

	CreateUserStmt::~CreateUserStmt()
	{
		delete user_name;
		if (name_value_list != nullptr) {
			for (auto p : *name_value_list)
				delete p;
			delete name_value_list;
		}
	}

	CreateDBStmt::~CreateDBStmt()
	{
		delete db_name;
		if (name_value_list != nullptr) {
			for (auto p : *name_value_list)
				delete p;
			delete name_value_list;
		}
	}

	void
	CreateDBStmt::execute(Catalog_Namespace::Catalog &catalog)
	{
		if (catalog.get_currentDB().dbName != MAPD_SYSTEM_DB)
			throw std::runtime_error("Must be in the system database to create databases.");
		Catalog_Namespace::SysCatalog &syscat = static_cast<Catalog_Namespace::SysCatalog&>(catalog);
		int ownerId = catalog.get_currentUser().userId;
		if (name_value_list != nullptr) {
			for (auto p : *name_value_list) {
				if (boost::iequals(*p->get_name(), "owner")) {
					Catalog_Namespace::UserMetadata user;
					if (!syscat.getMetadataForUser(*p->get_value(), user))
						throw std::runtime_error("User " + *p->get_value() + " does not exist.");
					ownerId = user.userId;
				}
				else
					throw std::runtime_error("Invalid CREATE DATABASE option " + *p->get_name() + ". Only OWNER supported.");
			}
		}
		syscat.createDatabase(*db_name, ownerId);
	}

	void
	DropDBStmt::execute(Catalog_Namespace::Catalog &catalog)
	{
		if (catalog.get_currentDB().dbName != MAPD_SYSTEM_DB)
			throw std::runtime_error("Must be in the system database to drop databases.");
		Catalog_Namespace::SysCatalog &syscat = static_cast<Catalog_Namespace::SysCatalog&>(catalog);
		syscat.dropDatabase(*db_name);
	}

	void
	CreateUserStmt::execute(Catalog_Namespace::Catalog &catalog)
	{
		std::string passwd;
		bool is_super = false;
		for (auto p : *name_value_list) {
			if (boost::iequals(*p->get_name(), "password"))
				passwd = *p->get_value();
			else if (boost::iequals(*p->get_name(), "is_super")) {
				if (boost::iequals(*p->get_value(), "true"))
					is_super = true;
				else if (boost::iequals(*p->get_value(), "false"))
					is_super = false;
				else
					throw std::runtime_error("Value to IS_SUPER must be TRUE or FALSE.");
			} else
				throw std::runtime_error("Invalid CREATE USER option " + *p->get_name() + ".  Should be PASSWORD or IS_SUPER.");
		}
		if (passwd.empty())
			throw std::runtime_error("Must have a password for CREATE USER.");
		if (catalog.get_currentDB().dbName != MAPD_SYSTEM_DB)
			throw std::runtime_error("Must be in the system database to create users.");
		Catalog_Namespace::SysCatalog &syscat = static_cast<Catalog_Namespace::SysCatalog&>(catalog);
		syscat.createUser(*user_name, passwd, is_super);
	}

	AlterUserStmt::~AlterUserStmt()
	{
		delete user_name;
		if (name_value_list != nullptr) {
			for (auto p : *name_value_list)
				delete p;
			delete name_value_list;
		}
	}

	void
	AlterUserStmt::execute(Catalog_Namespace::Catalog &catalog)
	{
		const std::string *passwd = nullptr;
		bool is_super = false;
		bool *is_superp = nullptr;
		for (auto p : *name_value_list) {
			if (boost::iequals(*p->get_name(), "password"))
				passwd = p->get_value();
			else if (boost::iequals(*p->get_name(), "is_super")) {
				if (boost::iequals(*p->get_value(), "true")) {
					is_super = true;
					is_superp = &is_super;
				} else if (boost::iequals(*p->get_value(), "false")) {
					is_super = false;
					is_superp = &is_super;
				} else
					throw std::runtime_error("Value to IS_SUPER must be TRUE or FALSE.");
			} else
				throw std::runtime_error("Invalid CREATE USER option " + *p->get_name() + ".  Should be PASSWORD or IS_SUPER.");
		}
		Catalog_Namespace::SysCatalog &syscat = static_cast<Catalog_Namespace::SysCatalog&>(catalog);
		syscat.alterUser(*user_name, passwd, is_superp);
	}

	void
	DropUserStmt::execute(Catalog_Namespace::Catalog &catalog)
	{
		if (catalog.get_currentDB().dbName != MAPD_SYSTEM_DB)
			throw std::runtime_error("Must be in the system database to drop users.");
		Catalog_Namespace::SysCatalog &syscat = static_cast<Catalog_Namespace::SysCatalog&>(catalog);
		syscat.dropUser(*user_name);
	}

}
