#include <terminal/ControlFunctionDef.h>
#include <array>

using namespace std;

namespace terminal {

ControlFunctionDef const* controlFunctionById(uint32_t _id) noexcept
{
    static constexpr auto defs = array{
        CHA,
        CNL,
        CPL,
        CPR,
        CUB,
        CUD,
        CUF,
        CUP,
        CUU,
        DA1,
        DA2,
        DCH,
        DECDC,
        DECIC,
        DECRM,
        DECRQM_ANSI,
        DECRQM,
        DECSLRM,
        DECSM,
        DECSTBM,
        DECSTR,
        DECXCPR,
        DL,
        ECH,
        ED,
        EL,
        HPA,
        HPR,
        HVP,
        ICH,
        IL,
        RM,
        SD,
        SGR,
        SM,
        SU,
        VPA,
    };

    for (ControlFunctionDef const& def: defs)
        if (_id == def.id())
            return &def;

    return nullptr;
}

} // namespace terminal
