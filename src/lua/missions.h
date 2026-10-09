#pragma once

#include <cstddef>
#include <iosfwd>
#include <string_view>

namespace PCSX {
class Lua;
namespace Missions {
enum class Role { Mission, Module };
struct Resource {
    const char* name;
    Role role;
    const char* path;
    const char* chunk;
    size_t length;
};
const Resource* select(std::string_view name);
void catalogue(std::ostream& output);
void installModules(Lua L);
void run(Lua L, const Resource& selected);
}
}
