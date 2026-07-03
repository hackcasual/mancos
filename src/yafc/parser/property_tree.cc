#include "yafc/parser/property_tree.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace yafc {

namespace {

class Reader {
 public:
  explicit Reader(const std::string& bytes) : data_(bytes) {}

  template <typename T>
  T Read() {
    if (pos_ + sizeof(T) > data_.size()) throw std::runtime_error("settings: eof");
    T value;
    std::memcpy(&value, data_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return value;
  }

  int ReadSpaceOptimizedUint() {
    uint8_t b = Read<uint8_t>();
    if (b < 255) return b;
    return Read<int32_t>();
  }

  std::string ReadString() {
    if (Read<uint8_t>() != 0) return "";  // "empty string" flag
    int len = ReadSpaceOptimizedUint();
    if (pos_ + len > data_.size()) throw std::runtime_error("settings: eof");
    std::string s = data_.substr(pos_, len);
    pos_ += len;
    return s;
  }

 private:
  const std::string& data_;
  size_t pos_ = 0;
};

// Reads one value and leaves it on the Lua stack.
void ReadAny(Reader& reader, lua_State* L) {
  uint8_t type = reader.Read<uint8_t>();
  reader.Read<uint8_t>();  // "any-type" flag, unused

  switch (type) {
    case 0: lua_pushnil(L); break;
    case 1: lua_pushboolean(L, reader.Read<uint8_t>() != 0); break;
    case 2: lua_pushnumber(L, reader.Read<double>()); break;
    case 3: {
      std::string s = reader.ReadString();
      lua_pushlstring(L, s.data(), s.size());
      break;
    }
    case 4: {  // list
      int count = reader.Read<int32_t>();
      lua_createtable(L, count, 0);
      for (int i = 0; i < count; i++) {
        reader.ReadString();  // discarded key
        ReadAny(reader, L);
        lua_rawseti(L, -2, i + 1);
      }
      break;
    }
    case 5: {  // dictionary
      int count = reader.Read<int32_t>();
      lua_createtable(L, 0, count);
      for (int i = 0; i < count; i++) {
        std::string key = reader.ReadString();
        lua_pushlstring(L, key.data(), key.size());
        ReadAny(reader, L);
        lua_rawset(L, -3);
      }
      break;
    }
    case 6: lua_pushnumber(L, static_cast<double>(reader.Read<int64_t>())); break;
    case 7: lua_pushnumber(L, static_cast<double>(reader.Read<uint64_t>())); break;
    default: throw std::runtime_error("settings: unknown property tree type");
  }
}

}  // namespace

std::optional<int> ReadModSettings(const std::string& bytes, LuaContext& lua) {
  try {
    Reader reader(bytes);
    int16_t major = reader.Read<int16_t>();
    reader.Read<int16_t>();  // minor
    reader.Read<int32_t>();  // patch
    reader.Read<uint8_t>();  // reserved bool
    if (major != 1 && major != 2) return std::nullopt;

    ReadAny(reader, lua.raw());
    return luaL_ref(lua.raw(), LUA_REGISTRYINDEX);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace yafc
