add_library(yafc_core
  yafc/analysis/automation_analysis.cc
  yafc/analysis/cost_analysis.cc
  yafc/analysis/dependencies.cc
  yafc/analysis/dependency_node.cc
  yafc/analysis/milestones.cc
  yafc/analysis/technology_science_analysis.cc
  yafc/model/data_classes.cc
  yafc/model/database.cc
  yafc/model/production_table.cc
  yafc/parser/data_deserializer.cc
  yafc/parser/data_deserializer_entity.cc
  yafc/parser/data_deserializer_recipes.cc
  yafc/parser/factorio_data_source.cc
  yafc/parser/lua_context.cc
  yafc/parser/property_tree.cc
  yafc/parser/zip_archive.cc
  yafc/serialization/database_dump.cc
  yafc/serialization/serialization.cc
  yafc/model/production_table_solver.cc
  yafc/solver/lp.cc
  yafc/lua/lua_vm.cc
)
target_include_directories(yafc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(yafc_core
  PRIVATE ortools::glop lua::lua miniz
  PUBLIC nlohmann_json::nlohmann_json
)
