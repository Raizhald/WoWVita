// Standard Vanilla server `transports` periods (milliseconds), not the duration of an approximated client spline.
// Reference: cmangos/classic-db, Full_DB/ClassicDB_1_12_1_z2815.sql.gz, `transports` table.
// Servers with custom routes/periods can override the P rows in transports.txt.
#pragma once
#include <stdint.h>

struct W3dTransportPeriod { uint32_t entry, periodMs; };
inline constexpr W3dTransportPeriod kW3dClassicTransportPeriods[] = {
    {20808, 350818}, {164871, 356284}, {175080, 303463},
    {176231, 329313}, {176244, 316251}, {176310, 295579},
    {176495, 333044}, {177233, 317038}, {181056, 1204730},
};
