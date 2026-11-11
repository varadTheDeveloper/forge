#include "kavach/Ops.h"

#include <string.h>

namespace forge::kavach
{

namespace
{

const OpInfo kOps[] = {
#define FORGE_KAVACH_OP_INFO(id, name, permission) { name, Permission::permission },
    FORGE_KAVACH_OPS(FORGE_KAVACH_OP_INFO)
#undef FORGE_KAVACH_OP_INFO
};

static_assert(sizeof(kOps) / sizeof(kOps[0]) == kOpCount, "one row per op");

} // namespace

const OpInfo& GetOpInfo(
    Op op) noexcept
{
    const size_t index = static_cast<size_t>(op);
    return kOps[index < kOpCount ? index : 0];
}

const char* PermissionName(
    Permission permission) noexcept
{
    switch (permission)
    {
    case Permission::None: return "none";
    case Permission::Read: return "read";
    case Permission::Write: return "write";
    case Permission::Env: return "env";
    case Permission::EnvAll: return "env (every variable)";
    case Permission::ReadWrite: return "read and write";
    }

    return "unknown";
}

bool OpAllows(
    Permission permission,
    Access access) noexcept
{
    switch (permission)
    {
    case Permission::Read: return access == Access::Read;
    case Permission::Write: return access == Access::Write;
    case Permission::ReadWrite: return true;
    default: return false;
    }
}

Op FindOp(
    const char* name,
    size_t length) noexcept
{
    for (size_t i = 0; i < kOpCount; ++i)
    {
        if (strlen(kOps[i].name) == length && memcmp(kOps[i].name, name, length) == 0)
        {
            return static_cast<Op>(i);
        }
    }

    return Op::Count;
}

} // namespace forge::kavach
