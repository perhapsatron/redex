/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "MethodOverrideGraph.h"
#include "Pass.h"
#include "PassManager.h"

enum CseSpecialLocations : size_t {
  GENERAL_MEMORY_BARRIER,
  ARRAY_COMPONENT_TYPE_INT,
  ARRAY_COMPONENT_TYPE_BYTE,
  ARRAY_COMPONENT_TYPE_CHAR,
  ARRAY_COMPONENT_TYPE_WIDE,
  ARRAY_COMPONENT_TYPE_SHORT,
  ARRAY_COMPONENT_TYPE_OBJECT,
  ARRAY_COMPONENT_TYPE_BOOLEAN,
  END
};

// A (tracked) location is either a special location, or a field.
// Stored in a union, special locations are effectively represented as illegal
// pointer values.
// The nullptr field and CseSpecialLocations::OTHER_LOCATION are in effect
// aliases.
struct CseLocation {
  explicit CseLocation(const DexField* f) : field(f) {}
  explicit CseLocation(CseSpecialLocations sl) : special_location(sl) {}
  union {
    const DexField* field;
    CseSpecialLocations special_location;
  };
};

inline bool operator==(const CseLocation& a, const CseLocation& b) {
  return a.field == b.field;
}

inline bool operator!=(const CseLocation& a, const CseLocation& b) {
  return !(a == b);
}

inline bool operator<(const CseLocation& a, const CseLocation& b) {
  if (a.special_location < CseSpecialLocations::END) {
    if (b.special_location < CseSpecialLocations::END) {
      return a.special_location < b.special_location;
    } else {
      return true;
    }
  }
  if (b.special_location < CseSpecialLocations::END) {
    return false;
  }
  return dexfields_comparator()(a.field, b.field);
}

struct CseLocationHasher {
  size_t operator()(const CseLocation& l) const { return (size_t)l.field; }
};

std::ostream& operator<<(std::ostream&, const CseLocation&);
std::ostream& operator<<(
    std::ostream&, const std::unordered_set<CseLocation, CseLocationHasher>&);

namespace cse_impl {

struct Stats {
  size_t results_captured{0};
  size_t stores_captured{0};
  size_t array_lengths_captured{0};
  size_t instructions_eliminated{0};
  size_t max_value_ids{0};
  size_t methods_using_other_tracked_location_bit{0};
  // keys are IROpcode encoded as uint16_t, to make OSS build happy
  std::unordered_map<uint16_t, size_t> eliminated_opcodes;
};

struct MethodBarriersStats {
  size_t inlined_barriers_iterations{0};
  size_t inlined_barriers_into_methods{0};
};

// A barrier is defined by a particular opcode, and possibly some extra data
// (field, method)
struct Barrier {
  IROpcode opcode;
  union {
    const DexField* field{nullptr};
    const DexMethod* method;
  };
};

inline bool operator==(const Barrier& a, const Barrier& b) {
  return a.opcode == b.opcode && a.field == b.field;
}

struct BarrierHasher {
  size_t operator()(const Barrier& b) const {
    return b.opcode ^ (size_t)b.field;
  }
};

class SharedState {
 public:
  SharedState(const std::unordered_set<DexMethodRef*>& pure_methods);
  MethodBarriersStats init_method_barriers(const Scope&);
  boost::optional<CseLocation> get_relevant_written_location(
      const IRInstruction* insn,
      DexType* exact_virtual_scope,
      const std::unordered_set<CseLocation, CseLocationHasher>& read_locations);
  void log_barrier(const Barrier& barrier);
  bool has_pure_method(const IRInstruction* insn) const;
  void cleanup();

 private:
  bool may_be_barrier(const IRInstruction* insn, DexType* exact_virtual_scope);
  bool is_invoke_safe(const IRInstruction* insn, DexType* exact_virtual_scope);
  bool is_invoke_a_barrier(
      const IRInstruction* insn,
      const std::unordered_set<CseLocation, CseLocationHasher>& read_locations);
  std::unordered_set<DexMethodRef*> m_pure_methods;
  std::unordered_set<DexMethodRef*> m_safe_methods;
  std::unique_ptr<ConcurrentMap<Barrier, size_t, BarrierHasher>> m_barriers;
  std::unordered_map<const DexMethod*,
                     std::unordered_set<CseLocation, CseLocationHasher>>
      m_method_written_locations;
  std::unique_ptr<const method_override_graph::Graph> m_method_override_graph;
};

class CommonSubexpressionElimination {
 public:
  CommonSubexpressionElimination(SharedState* shared_state,
                                 cfg::ControlFlowGraph&);

  const Stats& get_stats() const { return m_stats; }

  /*
   * Patch code based on analysis results.
   */
  bool patch(bool is_static,
             DexType* declaring_type,
             DexTypeList* args,
             bool runtime_assertions = false);

 private:
  // CSE is finding instances where the result (in the dest register) of an
  // earlier instruction can be forwarded to replace the result of another
  // (later) instruction.
  struct Forward {
    IRInstruction* earlier_insn;
    IRInstruction* insn;
  };
  std::vector<Forward> m_forward;
  SharedState* m_shared_state;
  cfg::ControlFlowGraph& m_cfg;
  Stats m_stats;

  void insert_runtime_assertions(
      bool is_static,
      DexType* declaring_type,
      DexTypeList* args,
      const std::vector<std::pair<Forward, IRInstruction*>>& to_check);
};

} // namespace cse_impl

class CommonSubexpressionEliminationPass : public Pass {
 public:
  CommonSubexpressionEliminationPass()
      : Pass("CommonSubexpressionEliminationPass") {}

  virtual void bind_config() override;
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_debug;
  bool m_runtime_assertions;
};
