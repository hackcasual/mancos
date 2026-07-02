add_library(yafc_core
  yafc/analysis/automation_analysis.cc
  yafc/analysis/cost_analysis.cc
  yafc/analysis/dependencies.cc
  yafc/analysis/dependency_node.cc
  yafc/analysis/milestones.cc
  yafc/model/data_classes.cc
  yafc/model/database.cc
  yafc/model/production_table_solver.cc
  yafc/solver/lp.cc
  yafc/lua/lua_vm.cc
)
target_include_directories(yafc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(yafc_core
  PRIVATE ortools::glop lua::lua
)
