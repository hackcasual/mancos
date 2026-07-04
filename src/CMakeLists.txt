add_library(yafc_core
  yafc/analysis/automation_analysis.cc
  yafc/analysis/cost_analysis.cc
  yafc/analysis/dependencies.cc
  yafc/analysis/dependency_node.cc
  yafc/analysis/milestones.cc
  yafc/analysis/technology_science_analysis.cc
  yafc/model/blueprint.cc
  yafc/model/data_classes.cc
  yafc/model/database.cc
  yafc/model/production_table.cc
  yafc/model/recipe_parameters.cc
  yafc/parser/data_deserializer.cc
  yafc/parser/data_deserializer_entity.cc
  yafc/parser/data_deserializer_recipes.cc
  yafc/parser/factorio_data_source.cc
  yafc/parser/locale.cc
  yafc/parser/lua_context.cc
  yafc/parser/property_tree.cc
  yafc/parser/zip_archive.cc
  yafc/serialization/database_dump.cc
  yafc/serialization/icon_scale.cc
  yafc/serialization/serialization.cc
  yafc/model/production_table_solver.cc
  yafc/solver/lp.cc
  yafc/lua/lua_vm.cc
)
target_include_directories(yafc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# stb (icon_scale.cc only): single-header PNG codec for bundle icon scaling.
target_include_directories(yafc_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb)
target_link_libraries(yafc_core
  PRIVATE ortools::glop lua::lua miniz
  PUBLIC nlohmann_json::nlohmann_json
)
