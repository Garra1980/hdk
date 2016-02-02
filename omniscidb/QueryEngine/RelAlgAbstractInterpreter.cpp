#ifdef HAVE_CALCITE
#include "RelAlgAbstractInterpreter.h"

#include <glog/logging.h>

#include <string>
#include <unordered_map>

ScanBufferDesc::ScanBufferDesc() : td_(nullptr) {
}

ScanBufferDesc::ScanBufferDesc(const TableDescriptor* td) : td_(td) {
  CHECK(td_);
}

namespace {

// Checked json field retrieval.
const rapidjson::Value& field(const rapidjson::Value& obj, const char field[]) noexcept {
  CHECK(obj.IsObject());
  const auto field_it = obj.FindMember(field);
  CHECK(field_it != obj.MemberEnd());
  return field_it->value;
}

const int64_t json_i64(const rapidjson::Value& obj) noexcept {
  CHECK(obj.IsInt64());
  return obj.GetInt64();
}

const std::string json_str(const rapidjson::Value& obj) noexcept {
  CHECK(obj.IsString());
  return obj.GetString();
}

const bool json_bool(const rapidjson::Value& obj) noexcept {
  CHECK(obj.IsBool());
  return obj.GetBool();
}

const double json_double(const rapidjson::Value& obj) noexcept {
  CHECK(obj.IsDouble());
  return obj.GetDouble();
}

unsigned node_id(const rapidjson::Value& ra_node) noexcept {
  const auto& id = field(ra_node, "id");
  return std::stoi(json_str(id));
}

RexAbstractInput* parse_abstract_input(const rapidjson::Value& expr) noexcept {
  const auto& input = field(expr, "input");
  return new RexAbstractInput(json_i64(input));
}

RexLiteral* parse_literal(const rapidjson::Value& expr) noexcept {
  CHECK(expr.IsObject());
  const auto& literal = field(expr, "literal");
  const auto type = to_sql_type(json_str(field(expr, "type")));
  const auto scale = json_i64(field(expr, "scale"));
  const auto precision = json_i64(field(expr, "precision"));
  const auto type_scale = json_i64(field(expr, "type_scale"));
  const auto type_precision = json_i64(field(expr, "type_precision"));
  switch (type) {
    case kDECIMAL:
      return new RexLiteral(json_i64(literal), type, scale, precision, type_scale, type_precision);
    case kDOUBLE:
      return new RexLiteral(json_double(literal), type, scale, precision, type_scale, type_precision);
    case kTEXT:
      return new RexLiteral(json_str(literal), type, scale, precision, type_scale, type_precision);
    case kBOOLEAN:
      return new RexLiteral(json_bool(literal), type, scale, precision, type_scale, type_precision);
    case kNULLT:
      return new RexLiteral();
    default:
      CHECK(false);
  }
  CHECK(false);
  return nullptr;
}

RexScalar* parse_scalar_expr(const rapidjson::Value& expr);

RexOperator* parse_operator(const rapidjson::Value& expr) {
  const auto op = to_sql_op(json_str(field(expr, "op")));
  const auto& operators_json_arr = field(expr, "operands");
  CHECK(operators_json_arr.IsArray());
  std::vector<const RexScalar*> operands;
  for (auto operators_json_arr_it = operators_json_arr.Begin(); operators_json_arr_it != operators_json_arr.End();
       ++operators_json_arr_it) {
    operands.push_back(parse_scalar_expr(*operators_json_arr_it));
  }
  return new RexOperator(op, operands);
}

std::vector<std::string> strings_from_json_array(const rapidjson::Value& json_str_arr) {
  CHECK(json_str_arr.IsArray());
  std::vector<std::string> fields;
  for (auto json_str_arr_it = json_str_arr.Begin(); json_str_arr_it != json_str_arr.End(); ++json_str_arr_it) {
    CHECK(json_str_arr_it->IsString());
    fields.emplace_back(json_str_arr_it->GetString());
  }
  return fields;
}

std::vector<size_t> indices_from_json_array(const rapidjson::Value& json_idx_arr) {
  CHECK(json_idx_arr.IsArray());
  std::vector<size_t> indices;
  for (auto json_idx_arr_it = json_idx_arr.Begin(); json_idx_arr_it != json_idx_arr.End(); ++json_idx_arr_it) {
    CHECK(json_idx_arr_it->IsInt());
    CHECK_GE(json_idx_arr_it->GetInt(), 0);
    indices.emplace_back(json_idx_arr_it->GetInt());
  }
  return indices;
}

RexAgg* parse_aggregate_expr(const rapidjson::Value& expr) {
  const auto agg = to_agg_kind(json_str(field(expr, "agg")));
  const auto distinct = json_bool(field(expr, "distinct"));
  const auto& type_json = field(expr, "type");
  CHECK(type_json.IsObject() && type_json.MemberCount() == 2);
  const auto type = to_sql_type(json_str(field(type_json, "type")));
  const auto nullable = json_bool(field(type_json, "nullable"));
  const auto operands = indices_from_json_array(field(expr, "operands"));
  return new RexAgg(agg, distinct, type, nullable, operands);
}

RexScalar* parse_scalar_expr(const rapidjson::Value& expr) {
  CHECK(expr.IsObject());
  if (expr.IsObject() && expr.HasMember("input")) {
    return parse_abstract_input(expr);
  }
  if (expr.IsObject() && expr.HasMember("literal")) {
    return parse_literal(expr);
  }
  if (expr.IsObject() && expr.HasMember("op")) {
    return parse_operator(expr);
  }
  CHECK(false);
  return nullptr;
}

RelJoinType to_join_type(const std::string& join_type_name) {
  if (join_type_name == "inner") {
    return RelJoinType::INNER;
  }
  if (join_type_name == "left") {
    return RelJoinType::LEFT;
  }
  CHECK(false);
  return RelJoinType::INNER;
}

// Creates an output with n columns.
std::vector<RexInput> n_outputs(const RelAlgNode* node, const size_t n) {
  std::vector<RexInput> outputs;
  for (size_t i = 0; i < n; ++i) {
    outputs.emplace_back(node, i);
  }
  return outputs;
}

typedef std::vector<RexInput> RANodeOutput;

RANodeOutput get_node_output(const RelAlgNode* ra_node) {
  RANodeOutput outputs;
  const auto scan_node = dynamic_cast<const RelScan*>(ra_node);
  if (scan_node) {
    // Scan node has no inputs, output contains all columns in the table.
    CHECK_EQ(size_t(0), scan_node->inputCount());
    return n_outputs(scan_node, scan_node->size());
  }
  const auto project_node = dynamic_cast<const RelProject*>(ra_node);
  if (project_node) {
    // Project output count doesn't depend on the input
    CHECK_EQ(size_t(1), project_node->inputCount());
    return n_outputs(project_node, project_node->size());
  }
  const auto filter_node = dynamic_cast<const RelFilter*>(ra_node);
  if (filter_node) {
    // Filter preserves shape
    CHECK_EQ(size_t(1), filter_node->inputCount());
    const auto prev_out = get_node_output(filter_node->getInput(0));
    return n_outputs(filter_node, prev_out.size());
  }
  const auto aggregate_node = dynamic_cast<const RelAggregate*>(ra_node);
  if (aggregate_node) {
    // Aggregate output count doesn't depend on the input
    CHECK_EQ(size_t(1), aggregate_node->inputCount());
    return n_outputs(aggregate_node, aggregate_node->size());
  }
  const auto join_node = dynamic_cast<const RelJoin*>(ra_node);
  if (join_node) {
    // Join concatenates the outputs from the inputs and the output
    // directly references the nodes in the input.
    CHECK_EQ(size_t(2), join_node->inputCount());
    auto lhs_out = get_node_output(join_node->getInput(0));
    const auto rhs_out = get_node_output(join_node->getInput(1));
    lhs_out.insert(lhs_out.end(), rhs_out.begin(), rhs_out.end());
    return lhs_out;
  }
  CHECK(false);
  return outputs;
}

const RexScalar* disambiguate_rex(const RexScalar* rex_scalar, const RelAlgNode* ra_output) {
  const auto rex_abstract_input = dynamic_cast<const RexAbstractInput*>(rex_scalar);
  if (rex_abstract_input) {
    return new RexInput(ra_output, rex_abstract_input->getIndex());
  }
  const auto rex_operator = dynamic_cast<const RexOperator*>(rex_scalar);
  if (rex_operator) {
    std::vector<const RexScalar*> disambiguated_operands;
    for (size_t i = 0; i < rex_operator->size(); ++i) {
      disambiguated_operands.push_back(disambiguate_rex(rex_operator->getOperand(i), ra_output));
    }
    return new RexOperator(rex_operator->getOperator(), disambiguated_operands);
  }
  const auto rex_literal = dynamic_cast<const RexLiteral*>(rex_scalar);
  CHECK(rex_literal);
  return new RexLiteral(*rex_literal);
}

void bind_inputs(const std::vector<RelAlgNode*>& nodes) {
  for (auto ra_node : nodes) {
    auto filter_node = dynamic_cast<RelFilter*>(ra_node);
    if (filter_node) {
      CHECK_EQ(size_t(1), filter_node->inputCount());
      const auto disambiguated_condition = disambiguate_rex(filter_node->getCondition(), filter_node->getInput(0));
      filter_node->setCondition(disambiguated_condition);
      continue;
    }
    const auto project_node = dynamic_cast<RelProject*>(ra_node);
    if (project_node) {
      CHECK_EQ(size_t(1), project_node->inputCount());
      std::vector<const RexScalar*> disambiguated_exprs;
      for (size_t i = 0; i < project_node->size(); ++i) {
        disambiguated_exprs.push_back(disambiguate_rex(project_node->getProjectAt(i), project_node->getInput(0)));
      }
      project_node->setExpressions(disambiguated_exprs);
      continue;
    }
  }
}

enum class CoalesceState { Initial, Filter, FirstProject, Aggregate };

void coalesce_nodes(std::vector<RelAlgNode*>& nodes) {
  std::vector<const RelAlgNode*> crt_pattern;
  CoalesceState crt_state{CoalesceState::Initial};
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto ra_node = nodes[i];
    switch (crt_state) {
      case CoalesceState::Initial: {
        if (dynamic_cast<const RelFilter*>(ra_node)) {
          crt_pattern.push_back(ra_node);
          crt_state = CoalesceState::Filter;
        } else if (dynamic_cast<const RelProject*>(ra_node)) {
          crt_pattern.push_back(ra_node);
          crt_state = CoalesceState::FirstProject;
        }
        break;
      }
      case CoalesceState::Filter: {
        CHECK(dynamic_cast<const RelProject*>(ra_node));  // TODO: is filter always followed by project?
        crt_pattern.push_back(ra_node);
        crt_state = CoalesceState::FirstProject;
        break;
      }
      case CoalesceState::FirstProject: {
        if (dynamic_cast<const RelAggregate*>(ra_node)) {
          crt_pattern.push_back(ra_node);
          crt_state = CoalesceState::Aggregate;
        } else {
          crt_state = CoalesceState::Initial;
          // TODO(alex): found a F?P pattern which ends here, create the compound node
          decltype(crt_pattern)().swap(crt_pattern);
        }
        break;
      }
      case CoalesceState::Aggregate: {
        if (dynamic_cast<const RelProject*>(ra_node) && static_cast<RelProject*>(ra_node)->isSimple()) {
          crt_pattern.push_back(ra_node);
        }
        crt_state = CoalesceState::Initial;
        // TODO(alex): found a F?P(A|AP)? pattern which ends here, create the compound node
        decltype(crt_pattern)().swap(crt_pattern);
        break;
      }
      default:
        CHECK(false);
    }
  }
  // TODO(alex): wrap-up this function
}

class RaAbstractInterp {
 public:
  RaAbstractInterp(const rapidjson::Value& query_ast, const Catalog_Namespace::Catalog& cat)
      : query_ast_(query_ast), cat_(cat) {}

  std::unique_ptr<const RelAlgNode> run() {
    const auto& rels = field(query_ast_, "rels");
    CHECK(rels.IsArray());
    for (auto rels_it = rels.Begin(); rels_it != rels.End(); ++rels_it) {
      const auto& crt_node = *rels_it;
      const auto id = node_id(crt_node);
      CHECK_EQ(static_cast<size_t>(id), nodes_.size());
      CHECK(crt_node.IsObject());
      RelAlgNode* ra_node = nullptr;
      const auto rel_op = json_str(field(crt_node, "relOp"));
      if (rel_op == std::string("LogicalTableScan")) {
        ra_node = dispatchTableScan(crt_node);
      } else if (rel_op == std::string("LogicalProject")) {
        ra_node = dispatchProject(crt_node);
      } else if (rel_op == std::string("LogicalFilter")) {
        ra_node = dispatchFilter(crt_node);
      } else if (rel_op == std::string("LogicalAggregate")) {
        ra_node = dispatchAggregate(crt_node);
      } else if (rel_op == std::string("LogicalJoin")) {
        ra_node = dispatchJoin(crt_node);
      } else {
        CHECK(false);
      }
      nodes_.push_back(ra_node);
    }
    CHECK(!nodes_.empty());
    bind_inputs(nodes_);
    coalesce_nodes(nodes_);
    return std::unique_ptr<const RelAlgNode>(nodes_.back());
  }

 private:
  RelScan* dispatchTableScan(const rapidjson::Value& scan_ra) {
    CHECK(scan_ra.IsObject());
    const auto td = getTableFromScanNode(scan_ra);
    const auto field_names = getFieldNamesFromScanNode(scan_ra);
    return new RelScan(td, field_names);
  }

  RelProject* dispatchProject(const rapidjson::Value& proj_ra) {
    const auto& exprs_json = field(proj_ra, "exprs");
    CHECK(exprs_json.IsArray());
    std::vector<const RexScalar*> exprs;
    for (auto exprs_json_it = exprs_json.Begin(); exprs_json_it != exprs_json.End(); ++exprs_json_it) {
      exprs.push_back(parse_scalar_expr(*exprs_json_it));
    }
    const auto& fields = field(proj_ra, "fields");
    return new RelProject(exprs, strings_from_json_array(fields), prev(proj_ra));
  }

  RelFilter* dispatchFilter(const rapidjson::Value& filter_ra) {
    const auto id = node_id(filter_ra);
    CHECK(id);
    return new RelFilter(parse_operator(field(filter_ra, "condition")), prev(filter_ra));
  }

  const RelAlgNode* prev(const rapidjson::Value& crt_node) {
    const auto id = node_id(crt_node);
    CHECK(id);
    CHECK_EQ(static_cast<size_t>(id), nodes_.size());
    return nodes_.back();
  }

  RelAggregate* dispatchAggregate(const rapidjson::Value& agg_ra) {
    const auto fields = strings_from_json_array(field(agg_ra, "fields"));
    const auto group = indices_from_json_array(field(agg_ra, "group"));
    const auto& aggs_json_arr = field(agg_ra, "aggs");
    CHECK(aggs_json_arr.IsArray());
    std::vector<const RexAgg*> aggs;
    for (auto aggs_json_arr_it = aggs_json_arr.Begin(); aggs_json_arr_it != aggs_json_arr.End(); ++aggs_json_arr_it) {
      aggs.push_back(parse_aggregate_expr(*aggs_json_arr_it));
    }
    return new RelAggregate(group, aggs, fields, prev(agg_ra));
  }

  RelJoin* dispatchJoin(const rapidjson::Value& join_ra) {
    const auto join_type = to_join_type(json_str(field(join_ra, "joinType")));
    const auto filter_rex = parse_scalar_expr(field(join_ra, "condition"));
    const auto str_input_indices = strings_from_json_array(field(join_ra, "inputs"));
    CHECK_EQ(size_t(2), str_input_indices.size());
    std::vector<size_t> input_indices;
    for (const auto& str_index : str_input_indices) {
      input_indices.push_back(std::stoi(str_index));
    }
    CHECK_LT(input_indices[0], nodes_.size());
    CHECK_LT(input_indices[1], nodes_.size());
    return new RelJoin(nodes_[input_indices[0]], nodes_[input_indices[1]], filter_rex, join_type);
  }

  const TableDescriptor* getTableFromScanNode(const rapidjson::Value& scan_ra) const {
    const auto& table_json = field(scan_ra, "table");
    CHECK(table_json.IsArray());
    CHECK_EQ(unsigned(3), table_json.Size());
    const auto td = cat_.getMetadataForTable(table_json[2].GetString());
    CHECK(td);
    return td;
  }

  std::vector<std::string> getFieldNamesFromScanNode(const rapidjson::Value& scan_ra) const {
    const auto& fields_json = field(scan_ra, "fieldNames");
    return strings_from_json_array(fields_json);
  }

  const rapidjson::Value& query_ast_;
  const Catalog_Namespace::Catalog& cat_;
  std::vector<RelAlgNode*> nodes_;
};

}  // namespace

std::unique_ptr<const RelAlgNode> ra_interpret(const rapidjson::Value& query_ast,
                                               const Catalog_Namespace::Catalog& cat) {
  RaAbstractInterp interp(query_ast, cat);
  return interp.run();
}
#endif  // HAVE_CALCITE
