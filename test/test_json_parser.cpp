#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "model/tdg.h"
#include "parse/json.h"

using parse::JsonNode;
using parse::Parser;
using parse::ValidationResult;

namespace {

// Parses a one-task graph with the given task object and returns validation.
ValidationResult validate_single_task(const std::string& task_json) {
  const std::string json = R"({
    "configuration": {"num_cpus": 2, "cores_per_cpu": 4, "shared_locks": []},
    "nodes": [)" + task_json +
                           R"(],
    "edges": []
  })";

  Parser parser;
  const auto result = parser.parse_string(json);
  EXPECT_TRUE(result.success) << result.error_message;
  return parser.validate();
}

bool has_error_containing(const ValidationResult& result, const std::string& needle) {
  return std::any_of(result.errors.begin(), result.errors.end(), [&](const std::string& error) {
    return error.find(needle) != std::string::npos;
  });
}

}  // namespace

class JsonParserTest : public ::testing::Test {
 protected:
  void SetUp() override {}
};

TEST_F(JsonParserTest, ValidJsonParsing) {
  std::string json = R"({
    "graph": {"name": "TestGraph"},
    "configuration": {"num_cpus": 2, "cores_per_cpu": 4, "shared_locks": ["lock1", "lock2"]},
    "nodes": [
      {"id": "TaskA", "type": "task", "priority": 97, "core": 0, "time": [[3, 8]], "locks": ["lock1"]},
      {"id": "TaskB", "type": "task", "priority": 98, "core": 1, "time": [[3, 5]], "locks": []}
    ],
    "edges": [
      {"source": "TaskA", "target": "TaskB"}
    ]
  })";

  Parser parser;
  auto result = parser.parse_string(json);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(parser.get_graph_name(), "TestGraph");
  EXPECT_EQ(parser.get_num_cpus(), 2);
  EXPECT_EQ(parser.get_cores_per_cpu(), 4);
  EXPECT_EQ(parser.get_nodes().size(), 2);
  EXPECT_EQ(parser.get_edges().size(), 1);
  EXPECT_TRUE(parser.get_start_tasks().empty());
  EXPECT_TRUE(parser.get_end_tasks().empty());
  EXPECT_TRUE(parser.get_periodic_tasks().empty());
}

TEST_F(JsonParserTest, ConfigurationStartEndPeriodicParsing) {
  std::string json = R"({
    "graph": {"name": "ConfigTest"},
    "configuration": {
      "num_cpus": 2,
      "cores_per_cpu": 4,
      "shared_locks": [],
      "start": [{"task": "TaskA", "tokens": 2}, "TaskB"],
      "end": ["TaskC"],
      "periodic": [{"task": "TaskA", "period": 100}]
    },
    "nodes": [
      {"id": "TaskA", "type": "task", "priority": 97, "core": 0, "time": [[3, 8]], "locks": []},
      {"id": "TaskB", "type": "task", "priority": 98, "core": 1, "time": [[3, 5]], "locks": []},
      {"id": "TaskC", "type": "task", "priority": 99, "core": 1, "time": [[4, 6]], "locks": []}
    ],
    "edges": []
  })";

  Parser parser;
  auto result = parser.parse_string(json);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(parser.get_start_tasks().size(), 2);
  EXPECT_EQ(parser.get_start_tasks()[0].task, "TaskA");
  EXPECT_EQ(parser.get_start_tasks()[0].tokens, 2);
  EXPECT_EQ(parser.get_start_tasks()[1].task, "TaskB");
  EXPECT_EQ(parser.get_start_tasks()[1].tokens, 1);
  ASSERT_EQ(parser.get_end_tasks().size(), 1);
  EXPECT_EQ(parser.get_end_tasks()[0], "TaskC");
  ASSERT_EQ(parser.get_periodic_tasks().size(), 1);
  EXPECT_EQ(parser.get_periodic_tasks()[0].task, "TaskA");
  EXPECT_EQ(parser.get_periodic_tasks()[0].period, 100);
}

TEST_F(JsonParserTest, InvalidJsonErrorHandling) {
  std::string invalid_json = "{ invalid json }";

  Parser parser;
  auto result = parser.parse_string(invalid_json);

  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

TEST_F(JsonParserTest, NodeTypeConversion) {
  JsonNode node;
  node.id = "TaskA";
  node.type = "task";
  node.priority = 97;
  node.core = 0;
  node.time = {{3, 8}};
  node.locks = {"lock1"};

  NodeType node_type = node.to_node_type();

  EXPECT_TRUE(std::holds_alternative<::TaskNode>(node_type));
}

TEST_F(JsonParserTest, DotExport) {
  std::string json = R"({
    "graph": {"name": "ExportTest"},
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "shared_locks": []},
    "nodes": [
      {"id": "TaskA", "type": "task", "priority": 97, "core": 0, "time": [[3, 8]], "locks": []}
    ],
    "edges": []
  })";

  Parser parser;
  auto result = parser.parse_string(json);
  ASSERT_TRUE(result.success);

  std::string dot = parser.to_dot_string();

  EXPECT_NE(dot.find("digraph ExportTest"), std::string::npos);
  EXPECT_NE(dot.find("TaskA"), std::string::npos);
}

TEST_F(JsonParserTest, ParseFileNotFound) {
  Parser parser;
  auto result = parser.parse_file("/nonexistent/path/file.json");

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("Failed to open file"), std::string::npos);
}

TEST_F(JsonParserTest, AllocationAndProvenanceFieldsDefaultWhenAbsent) {
  std::string json = R"({
    "configuration": {"num_cpus": 1, "cores_per_cpu": 2, "shared_locks": []},
    "nodes": [
      {"id": "TaskA", "type": "task", "priority": 10, "time": [[3, 8]], "locks": []}
    ],
    "edges": []
  })";

  Parser parser;
  ASSERT_TRUE(parser.parse_string(json).success);
  EXPECT_TRUE(parser.validate().success);

  ASSERT_EQ(parser.get_nodes().size(), 1);
  const JsonNode& node = parser.get_nodes()[0];
  EXPECT_EQ(node.core, 0);
  EXPECT_FALSE(node.has_core);
  EXPECT_FALSE(node.has_allowed_cores);
  EXPECT_TRUE(node.allowed_cores.empty());
  EXPECT_EQ(node.wcet_kind, "model");
  EXPECT_TRUE(node.wcet_method.empty());
  EXPECT_TRUE(node.wcet_artifact.empty());
}

TEST_F(JsonParserTest, AllowedCoresProvideInitialCoreAndProvenance) {
  std::string json = R"({
    "configuration": {"num_cpus": 8, "cores_per_cpu": 1, "shared_locks": []},
    "nodes": [
      {"id": "TaskA", "type": "task", "priority": 10, "allowed_cores": [5, 2, 7],
       "time": [[3, 8]], "locks": []},
      {"id": "TaskB", "type": "task", "priority": 11, "core": 3, "allowed_cores": [1, 3],
       "time": [[1, 2]], "locks": [],
       "wcet_kind": "certified", "wcet_method": "OTAWA", "wcet_artifact": "sha256:abc"}
    ],
    "edges": []
  })";

  Parser parser;
  ASSERT_TRUE(parser.parse_string(json).success);
  const auto validation = parser.validate();
  EXPECT_TRUE(validation.success) << (validation.errors.empty() ? std::string()
                                                                : validation.errors.front());

  ASSERT_EQ(parser.get_nodes().size(), 2);
  const JsonNode& task_a = parser.get_nodes()[0];
  EXPECT_FALSE(task_a.has_core);
  EXPECT_TRUE(task_a.has_allowed_cores);
  EXPECT_EQ(task_a.core, 5);  // first element of allowed_cores
  EXPECT_EQ(task_a.allowed_cores, (std::vector<int>{5, 2, 7}));

  const JsonNode& task_b = parser.get_nodes()[1];
  EXPECT_TRUE(task_b.has_core);
  EXPECT_EQ(task_b.core, 3);
  EXPECT_EQ(task_b.wcet_kind, "certified");
  EXPECT_EQ(task_b.wcet_method, "OTAWA");
  EXPECT_EQ(task_b.wcet_artifact, "sha256:abc");
}

TEST_F(JsonParserTest, InvalidAllowedCoresAreRejected) {
  const auto empty = validate_single_task(
      R"({"id": "T", "type": "task", "allowed_cores": [], "time": [[1, 2]], "locks": []})");
  EXPECT_FALSE(empty.success);
  EXPECT_TRUE(has_error_containing(empty, "empty allowed_cores"));

  const auto out_of_range = validate_single_task(
      R"({"id": "T", "type": "task", "allowed_cores": [0, 8], "time": [[1, 2]], "locks": []})");
  EXPECT_FALSE(out_of_range.success);
  EXPECT_TRUE(has_error_containing(out_of_range, "Invalid allowed core"));

  const auto negative = validate_single_task(
      R"({"id": "T", "type": "task", "allowed_cores": [-1, 2], "time": [[1, 2]], "locks": []})");
  EXPECT_FALSE(negative.success);
  EXPECT_TRUE(has_error_containing(negative, "Invalid allowed core"));

  const auto duplicate = validate_single_task(
      R"({"id": "T", "type": "task", "allowed_cores": [1, 1], "time": [[1, 2]], "locks": []})");
  EXPECT_FALSE(duplicate.success);
  EXPECT_TRUE(has_error_containing(duplicate, "Duplicate allowed core"));

  const auto core_outside_domain = validate_single_task(
      R"({"id": "T", "type": "task", "core": 0, "allowed_cores": [1, 2],
          "time": [[1, 2]], "locks": []})");
  EXPECT_FALSE(core_outside_domain.success);
  EXPECT_TRUE(has_error_containing(core_outside_domain, "not in its allowed_cores domain"));
}

TEST_F(JsonParserTest, WcetKindAcceptsKnownValuesAndRejectsUnknown) {
  for (const char* kind : {"model", "certified", "measured", "estimated"}) {
    SCOPED_TRACE(kind);
    const auto validation =
        validate_single_task(std::string(R"({"id": "T", "type": "task", "wcet_kind": ")") + kind +
                             R"(", "time": [[1, 2]], "locks": []})");
    EXPECT_TRUE(validation.success)
        << (validation.errors.empty() ? std::string() : validation.errors.front());
  }

  const auto unknown = validate_single_task(
      R"({"id": "T", "type": "task", "wcet_kind": "guessed", "time": [[1, 2]], "locks": []})");
  EXPECT_FALSE(unknown.success);
  EXPECT_TRUE(has_error_containing(unknown, "Invalid wcet_kind"));
}

TEST_F(JsonParserTest, DeadlineParsesAndRejectsNegativeValues) {
  const std::string json = R"({
    "configuration": {"num_cpus": 1, "cores_per_cpu": 1, "shared_locks": []},
    "nodes": [
      {"id": "A", "type": "task", "deadline": 7, "time": [[1, 2]], "locks": []},
      {"id": "B", "type": "task", "time": [[1, 2]], "locks": []}
    ],
    "edges": []
  })";

  Parser parser;
  ASSERT_TRUE(parser.parse_string(json).success);
  ASSERT_TRUE(parser.validate().success);
  ASSERT_EQ(parser.get_nodes().size(), 2);
  EXPECT_TRUE(parser.get_nodes()[0].has_deadline);
  EXPECT_EQ(parser.get_nodes()[0].deadline, 7);
  EXPECT_FALSE(parser.get_nodes()[1].has_deadline);
  EXPECT_EQ(parser.get_nodes()[1].deadline, 0);

  const auto negative = validate_single_task(
      R"({"id": "T", "type": "task", "deadline": -1, "time": [[1, 2]], "locks": []})");
  EXPECT_FALSE(negative.success);
  EXPECT_TRUE(has_error_containing(negative, "Invalid deadline"));
}

TEST_F(JsonParserTest, CoreIdsAreBoundedByNumCpusNotCoresPerCpu) {
  // num_cpus is the number of real core resources; cores_per_cpu is a
  // capacity parameter, not an additional range of core ids.
  const auto invalid_core = validate_single_task(
      R"({"id": "T", "type": "task", "core": 2, "time": [[1, 2]], "locks": []})");
  EXPECT_FALSE(invalid_core.success);
  EXPECT_TRUE(has_error_containing(invalid_core, "Invalid core number"));

  const auto invalid_domain = validate_single_task(
      R"({"id": "T", "type": "task", "allowed_cores": [0, 2],
          "time": [[1, 2]], "locks": []})");
  EXPECT_FALSE(invalid_domain.success);
  EXPECT_TRUE(has_error_containing(invalid_domain, "Invalid allowed core"));
}

TEST_F(JsonParserTest, TdgPopulatesAllocationDomainsAndProvenance) {
  std::string json = R"({
    "configuration": {"num_cpus": 8, "cores_per_cpu": 1, "shared_locks": []},
    "nodes": [
      {"id": "TaskA", "type": "task", "allowed_cores": [2, 4],
       "wcet_kind": "measured", "wcet_method": "MBPTA", "wcet_artifact": "bin/a.elf",
       "time": [[1, 2]], "locks": []},
      {"id": "TaskB", "type": "task", "core": 3, "time": [[1, 2]], "locks": []}
    ],
    "edges": []
  })";

  Parser parser;
  ASSERT_TRUE(parser.parse_string(json).success);
  ASSERT_TRUE(parser.validate().success);

  tdg::TDG model(parser.get_num_cpus(), parser.get_cores_per_cpu());
  model.load_from_parser(parser);

  EXPECT_EQ(model.task_allowed_cores.at("TaskA"), (std::vector<int>{2, 4}));
  EXPECT_EQ(model.task_allowed_cores.at("TaskB"), (std::vector<int>{3}));

  const tdg::WcetProvenance& task_a = model.task_wcet_provenance.at("TaskA");
  EXPECT_EQ(task_a.kind, "measured");
  EXPECT_EQ(task_a.method, "MBPTA");
  EXPECT_EQ(task_a.artifact, "bin/a.elf");

  const tdg::WcetProvenance& task_b = model.task_wcet_provenance.at("TaskB");
  EXPECT_EQ(task_b.kind, "model");
  EXPECT_TRUE(task_b.method.empty());
  EXPECT_TRUE(task_b.artifact.empty());
}