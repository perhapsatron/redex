/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "GlobalTypeAnalyzer.h"
#include "IRCode.h"
#include "LocalTypeAnalyzer.h"
#include "WholeProgramState.h"

namespace type_analyzer {

/**
 * Optimize the given code by:
 *   - removing dead nonnull assertions generated by Kotlin
 * (checkParameterIsNotNull/checkExpressionValueIsNotNull)
 */
class Transform final {
 public:
  using NullAssertionSet = std::unordered_set<DexMethodRef*>;
  struct Config {
    bool remove_redundant_null_checks{true};
    bool remove_kotlin_null_check_assertions{false};
    bool remove_redundant_type_checks{true};
    Config() {}
  };

  struct Stats {
    size_t null_check_removed{0};
    size_t unsupported_branch{0};
    size_t kotlin_null_check_removed{0};
    size_t type_check_removed{0};
    size_t null_check_only_type_checks{0};

    Stats& operator+=(const Stats& that) {
      null_check_removed += that.null_check_removed;
      unsupported_branch += that.unsupported_branch;
      kotlin_null_check_removed += that.kotlin_null_check_removed;
      type_check_removed += that.type_check_removed;
      null_check_only_type_checks += that.null_check_only_type_checks;
      return *this;
    }

    bool is_empty() {
      return null_check_removed == 0 && kotlin_null_check_removed == 0 &&
             type_check_removed == 0;
    }

    void report(PassManager& mgr) const {
      mgr.incr_metric("null_check_removed", null_check_removed);
      mgr.incr_metric("unsupported_branch", unsupported_branch);
      mgr.incr_metric("kotlin_null_check_removed", kotlin_null_check_removed);
      mgr.incr_metric("type_check_removed", type_check_removed);
      mgr.incr_metric("null_check_only_type_checks",
                      null_check_only_type_checks);
      TRACE(TYPE_TRANSFORM, 2, "TypeAnalysisTransform Stats:");
      TRACE(TYPE_TRANSFORM, 2, " null checks removed = %u", null_check_removed);
      TRACE(TYPE_TRANSFORM, 2, " unsupported branch = %u", unsupported_branch);
      TRACE(TYPE_TRANSFORM,
            2,
            " Kotlin null checks removed = %u",
            kotlin_null_check_removed);
      TRACE(TYPE_TRANSFORM, 2, " type checks removed = %u", type_check_removed);
      TRACE(TYPE_TRANSFORM,
            2,
            " null check only type checks = %u",
            null_check_only_type_checks);
    }
  };

  explicit Transform(Config config = Config()) : m_config(config) {}
  Stats apply(const type_analyzer::local::LocalTypeAnalyzer& lta,
              const WholeProgramState& wps,
              DexMethod* method,
              const NullAssertionSet& null_assertion_set);
  static void setup(NullAssertionSet& null_assertion_set);

 private:
  void apply_changes(IRCode*);

  void remove_redundant_null_checks(const DexTypeEnvironment& env,
                                    cfg::Block* block,
                                    Stats& stats);
  void remove_redundant_type_checks(const DexTypeEnvironment& env,
                                    IRList::iterator& it,
                                    Stats& stats);

  const Config m_config;
  // A set of methods excluded from null check removal
  ConcurrentSet<DexMethod*> m_excluded_for_null_check_removal;
  std::vector<std::pair<IRInstruction*, IRInstruction*>> m_replacements;
  std::vector<IRList::iterator> m_deletes;
};

} // namespace type_analyzer
