#include "yafc/lua/lua_vm.h"

#include "doctest/doctest.h"

using yafc::LuaVm;

TEST_CASE("lua 5.2.1 runs") {
  LuaVm vm;
  CHECK(*vm.EvalToString("_VERSION") == "Lua 5.2");
  CHECK(*vm.EvalToString("2^10") == "1024");
  CHECK(*vm.EvalToString("('factorio'):upper()") == "FACTORIO");
}

TEST_CASE("factorio patch: pairs() iterates in insertion order") {
  LuaVm vm;
  // Stock Lua iterates hash keys in unspecified order; Factorio's patch makes
  // it insertion-ordered, which mod data stages rely on for determinism.
  auto result = vm.EvalToString(R"lua(
    (function()
      local t = {}
      t.zebra = 1; t.apple = 2; t.mango = 3; t.banana = 4
      local order = {}
      for k in pairs(t) do order[#order + 1] = k end
      return table.concat(order, ",")
    end)()
  )lua");
  REQUIRE(result.has_value());
  CHECK(*result == "zebra,apple,mango,banana");
}

TEST_CASE("factorio patch: next() over a vanished key does not error") {
  LuaVm vm;
  auto result = vm.EvalToString(
      "tostring(pcall(function() return next({a = 1}, 'missing') end))");
  REQUIRE(result.has_value());
  CHECK(*result == "true");
}
