#pragma once
#include "ShadowPatch.h"

namespace shadows {
// Owned changes only. The native setting is observed, never written.
class NormalShadows {
public:
    using Range = TerrainRange;
    static constexpr bool defaultSimple=false;
    static constexpr Range defaultRange=Range::Full;
    bool simple=defaultSimple;
    Range requested=defaultRange;

    bool sync(std::uint8_t* base, bool standalone=false) {
        if (owner!=base) {
            owner=nullptr;
            layout=nullptr;
            active=false;
            applied=Range::Native;
        }
        // Keep the requested settings while the client loads or the standalone plugin owns shadows.
        if (!base || ((!simple && requested==Range::Native) || standalone) && !active && applied==Range::Native)
            return true;
        if (!layout) {
            layout=layout_for(base);
            if (!layout || !matches(base,false) || !terrain_matches(base,Range::Native)) {
                layout=nullptr;
                return false;
            }
            owner=base;
        }
        const bool normal=read<DWORD>(base+layout->setting)==0 && !standalone;
        const bool wantSimple=normal && simple;
        const Range wantRange=normal ? requested : Range::Native;
        if (wantSimple!=active) {
            if (set(base,wantSimple)!=Result::Changed) return false;
            active=wantSimple;
        }
        if (wantRange!=applied) {
            if (set_terrain(base,applied,wantRange)!=Result::Changed) return false;
            applied=wantRange;
        }
        return true;
    }

    bool reset(std::uint8_t* base) {
        simple=false;
        requested=Range::Native;
        return sync(base);
    }
private:
    const Layout* layout=nullptr;
    std::uint8_t* owner=nullptr;
    bool active=false;
    Range applied=Range::Native;
};
}
