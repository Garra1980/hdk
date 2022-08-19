/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "TestRelAlgDagBuilder.h"

#include "QueryEngine/RelAlgTranslator.h"

RelAlgNodePtr TestRelAlgDagBuilder::addScan(const TableRef& table) {
  return addScan(schema_provider_->getTableInfo(table));
}

RelAlgNodePtr TestRelAlgDagBuilder::addScan(int db_id, int table_id) {
  return addScan(schema_provider_->getTableInfo(db_id, table_id));
}

RelAlgNodePtr TestRelAlgDagBuilder::addScan(int db_id, const std::string& table_name) {
  return addScan(schema_provider_->getTableInfo(db_id, table_name));
}

RelAlgNodePtr TestRelAlgDagBuilder::addScan(TableInfoPtr table_info) {
  auto col_infos = schema_provider_->listColumns(*table_info);
  auto scan_node = std::make_shared<RelScan>(table_info, std::move(col_infos));
  nodes_.push_back(scan_node);
  return scan_node;
}

RelAlgNodePtr TestRelAlgDagBuilder::addProject(RelAlgNodePtr input,
                                               const std::vector<std::string>& fields,
                                               const std::vector<int>& cols) {
  hdk::ir::ExprPtrVector exprs;
  auto input_cols = get_node_output(input.get());
  for (auto col_idx : cols) {
    CHECK_LT((size_t)col_idx, input_cols.size());
    exprs.push_back(hdk::ir::makeExpr<hdk::ir::ColumnRef>(
        input_cols[col_idx].getSourceNode(), input_cols[col_idx].getIndex()));
  }
  return addProject(input, fields, std::move(exprs));
}

RelAlgNodePtr TestRelAlgDagBuilder::addProject(RelAlgNodePtr input,
                                               const std::vector<std::string>& fields,
                                               hdk::ir::ExprPtrVector exprs) {
  auto res = std::make_shared<RelProject>(std::move(exprs), fields, input);
  nodes_.push_back(res);
  return res;
}

RelAlgNodePtr TestRelAlgDagBuilder::addProject(RelAlgNodePtr input,
                                               const std::vector<int>& cols) {
  auto fields = buildFieldNames(cols.size());
  return addProject(input, fields, std::move(cols));
}

RelAlgNodePtr TestRelAlgDagBuilder::addProject(RelAlgNodePtr input,
                                               hdk::ir::ExprPtrVector exprs) {
  auto fields = buildFieldNames(exprs.size());
  return addProject(input, fields, std::move(exprs));
}

RelAlgNodePtr TestRelAlgDagBuilder::addAgg(
    RelAlgNodePtr input,
    const std::vector<std::string>& fields,
    size_t group_size,
    std::vector<std::unique_ptr<const RexAgg>> aggs) {
  hdk::ir::ExprPtrVector input_exprs = getInputExprsForAgg(input.get());
  hdk::ir::ExprPtrVector exprs;
  for (auto& rex : aggs) {
    exprs.push_back(
        RelAlgTranslator::translateAggregateRex(rex.get(), input_exprs, false));
  }
  auto res = std::make_shared<RelAggregate>(group_size, std::move(exprs), fields, input);
  nodes_.push_back(res);
  return res;
}

RelAlgNodePtr TestRelAlgDagBuilder::addAgg(RelAlgNodePtr input,
                                           const std::vector<std::string>& fields,
                                           size_t group_size,
                                           std::vector<AggDesc> aggs) {
  std::vector<std::unique_ptr<const RexAgg>> rex_aggs;
  for (auto& agg : aggs) {
    rex_aggs.push_back(
        std::make_unique<RexAgg>(agg.agg, agg.distinct, agg.type, agg.operands));
  }
  return addAgg(input, fields, group_size, std::move(rex_aggs));
}

RelAlgNodePtr TestRelAlgDagBuilder::addAgg(
    RelAlgNodePtr input,
    size_t group_size,
    std::vector<std::unique_ptr<const RexAgg>> aggs) {
  auto fields = buildFieldNames(group_size + aggs.size());
  return addAgg(input, fields, group_size, std::move(aggs));
}

RelAlgNodePtr TestRelAlgDagBuilder::addAgg(RelAlgNodePtr input,
                                           size_t group_size,
                                           std::vector<AggDesc> aggs) {
  auto fields = buildFieldNames(group_size + aggs.size());
  return addAgg(input, fields, group_size, std::move(aggs));
}

RelAlgNodePtr TestRelAlgDagBuilder::addSort(RelAlgNodePtr input,
                                            const std::vector<SortField>& collation,
                                            const size_t limit,
                                            const size_t offset) {
  auto res = std::make_shared<RelSort>(collation, limit, offset, input);
  nodes_.push_back(res);
  return res;
}

RelAlgNodePtr TestRelAlgDagBuilder::addJoin(RelAlgNodePtr lhs,
                                            RelAlgNodePtr rhs,
                                            const JoinType join_type,
                                            hdk::ir::ExprPtr condition) {
  auto res = std::make_shared<RelJoin>(lhs, rhs, std::move(condition), join_type);
  nodes_.push_back(res);
  return res;
}

RelAlgNodePtr TestRelAlgDagBuilder::addEquiJoin(RelAlgNodePtr lhs,
                                                RelAlgNodePtr rhs,
                                                const JoinType join_type,
                                                size_t lhs_col_idx,
                                                size_t rhs_col_idx) {
  std::vector<std::unique_ptr<const RexScalar>> eq_ops;
  eq_ops.push_back(std::make_unique<RexInput>(lhs.get(), lhs_col_idx));
  eq_ops.push_back(std::make_unique<RexInput>(rhs.get(), rhs_col_idx));
  auto lhs_expr = hdk::ir::makeExpr<hdk::ir::ColumnRef>(
      getColumnType(lhs.get(), lhs_col_idx), lhs.get(), lhs_col_idx);
  auto rhs_expr = hdk::ir::makeExpr<hdk::ir::ColumnRef>(
      getColumnType(rhs.get(), rhs_col_idx), rhs.get(), rhs_col_idx);
  auto eq_expr =
      hdk::ir::makeExpr<hdk::ir::BinOper>(kBOOLEAN, kEQ, kONE, lhs_expr, rhs_expr);
  return addJoin(lhs, rhs, join_type, std::move(eq_expr));
}

std::vector<std::string> TestRelAlgDagBuilder::buildFieldNames(size_t count) const {
  std::vector<std::string> res;
  for (size_t i = 1; i <= count; ++i) {
    res.push_back(std::string("field_") + std::to_string(i));
  }
  return res;
}

void TestRelAlgDagBuilder::setRoot(RelAlgNodePtr root) {
  root_ = root;
}
