add_library(yafc_core
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
