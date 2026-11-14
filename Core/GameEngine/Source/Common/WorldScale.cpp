/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// FILE: WorldScale.cpp ///////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature mohamed 14/11/2026
// Desc:   See WorldScale.h. Holds the effective scales, the INI parse context, and the audited
//         table of INI fields that -collisionScale / -rangeScale multiply.
//
// HOW THE TABLE WAS BUILT
//   Every INI field table in GameEngine + the W3D device layer was enumerated (642 rows that use
//   parseReal / parseCoord3D / the velocity parsers, plus every Int/UInt field whose name says
//   Range/Radius/Dist/Height/Altitude/Offset/Width/Length). Each row was then classified:
//
//   COLLISION  (-collisionScale)  numbers that describe the physical footprint of something:
//              building exits / fence spacing / production exit points / the geometry that
//              DynamicGeometryInfoUpdate writes into an object every frame.
//              (GeometryMajorRadius / MinorRadius / Height themselves are scaled in Geometry.cpp.)
//   RANGE      (-rangeScale)      distances the simulation compares against world distances:
//              weapon range + damage / scatter radii, vision / shroud, ability + scan ranges,
//              flight heights and approach distances, special power areas, spawn offsets...
//   NOT SCALED (and why) -- see the block comment at the bottom of the table.
//
//   A name is only listed without a context when it is unique across ALL field tables (checked by
//   script); names that mean different things in different blocks (Radius, Offset, MaxRange,
//   AttackRange, ...) are restricted to the block ("Weapon") or module ("AutoHealBehavior") that
//   owns them. The entry's expected parse function is checked too, so a same-named field of a
//   different type can never be touched by accident.
//
// DEBUGGING
//   -worldScaleLog writes WorldScale.log with the effective values, sample template values, and
//   at the end a per-entry hit count, so entries that never matched stand out (stale name, or a
//   module the loaded data simply doesn't use).
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/WorldScale.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Weapon.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// STATE //////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

Real WorldScale::s_collision = 1.0f;
Real WorldScale::s_speed = 1.0f;
Real WorldScale::s_range = 1.0f;

static char s_blockName[64] = "";
enum { MODULE_STACK_DEPTH = 4 };
static char s_moduleStack[MODULE_STACK_DEPTH][64];
static int s_moduleDepth = 0;
static const char *s_moduleName = "";
static const char *s_field = nullptr;	// points into the INI line buffer, valid only while a field is being parsed

static FILE *s_logFile = nullptr;
static Bool s_logTried = FALSE;
static Bool s_trackHits = FALSE;	// -worldScaleLog: count table matches even at scale 1.0 so the table can be audited

///////////////////////////////////////////////////////////////////////////////////////////////////
// THE TABLE //////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

enum { G_COLLISION = 0, G_RANGE = 1 };
enum { K_REAL = 0, K_COORD3D = 1, K_INT = 2, K_UINT = 3 };

struct ScaleEntry
{
	const char *ctx;		///< block ("Weapon") or module ("AutoHealBehavior") that owns the field; null = any
	const char *field;	///< INI field name (case-sensitive, exactly as in the field parse table)
	Int group;					///< G_COLLISION or G_RANGE
	Int kind;						///< expected parse function family
};

#define RANGE_REAL(ctx, field)			{ ctx, field, G_RANGE, K_REAL }
#define RANGE_COORD(ctx, field)			{ ctx, field, G_RANGE, K_COORD3D }
#define RANGE_INT(ctx, field)				{ ctx, field, G_RANGE, K_INT }
#define RANGE_UINT(ctx, field)			{ ctx, field, G_RANGE, K_UINT }
#define COLL_REAL(ctx, field)				{ ctx, field, G_COLLISION, K_REAL }
#define COLL_COORD(ctx, field)			{ ctx, field, G_COLLISION, K_COORD3D }

static const ScaleEntry s_table[] =
{
	// ---- Weapon block ---------------------------------------------------------------------------
	// Everything that is a world distance. AttackRange is also used by AI to pick targets, so
	// scaling it keeps AI behaviour proportional.
	RANGE_REAL( "Weapon", "AttackRange" ),
	RANGE_REAL( "Weapon", "MinimumAttackRange" ),
	RANGE_REAL( "Weapon", "PrimaryDamageRadius" ),
	RANGE_REAL( "Weapon", "SecondaryDamageRadius" ),
	RANGE_REAL( "Weapon", "ShockWaveRadius" ),
	RANGE_REAL( "Weapon", "RequestAssistRange" ),
	RANGE_REAL( "Weapon", "ScatterRadius" ),
	RANGE_REAL( "Weapon", "ScatterRadiusVsInfantry" ),
	RANGE_REAL( "Weapon", "ScatterTargetScalar" ),	// multiplies the unit-circle ScatterTarget offsets -> a distance
	RANGE_REAL( "Weapon", "HistoricBonusRadius" ),
	RANGE_REAL( "Weapon", "ContinueAttackRange" ),

	// ---- Object block (ThingTemplate) -----------------------------------------------------------
	RANGE_REAL( nullptr, "VisionRange" ),
	RANGE_REAL( nullptr, "ShroudClearingRange" ),
	RANGE_REAL( nullptr, "ShroudRevealToAllRange" ),
	// building footprint companions of Geometry*: these only make sense relative to the footprint
	COLL_REAL( nullptr, "FenceWidth" ),
	COLL_REAL( nullptr, "FenceXOffset" ),
	COLL_REAL( nullptr, "FactoryExitWidth" ),
	COLL_REAL( nullptr, "FactoryExtraBibWidth" ),
	COLL_COORD( nullptr, "UnitCreatePoint" ),			// where units appear, relative to the factory
	COLL_COORD( nullptr, "NaturalRallyPoint" ),		// where they walk to first, relative to the factory

	// ---- Locomotor block ------------------------------------------------------------------------
	RANGE_REAL( "Locomotor", "PreferredHeight" ),
	RANGE_REAL( "Locomotor", "CirclingRadius" ),
	RANGE_REAL( "Locomotor", "CloseEnoughDist" ),
	RANGE_REAL( "Locomotor", "WanderAboutPointRadius" ),
	RANGE_INT(  "Locomotor", "AirborneTargetingHeight" ),	// altitude above which a target counts as airborne

	// ---- GameData block -------------------------------------------------------------------------
	RANGE_REAL( "GameData", "CommandCenterHealRange" ),
	RANGE_REAL( "GameData", "StandardMinefieldDistance" ),
	COLL_REAL(  "GameData", "DefaultStructureRubbleHeight" ),

	// ---- SpecialPower block ---------------------------------------------------------------------
	RANGE_REAL( "SpecialPower", "ViewObjectRange" ),
	RANGE_REAL( "SpecialPower", "RadiusCursorRadius" ),

	// ---- ObjectCreationList block ---------------------------------------------------------------
	RANGE_COORD( "ObjectCreationList", "Offset" ),			// spawn offset of CreateObject/CreateDebris nuggets
	RANGE_REAL( nullptr, "WeaponErrorRadius" ),
	RANGE_REAL( nullptr, "FormationSpacing" ),
	RANGE_REAL( nullptr, "MinDistanceAFormation" ),
	RANGE_REAL( nullptr, "MinDistanceBFormation" ),
	RANGE_REAL( nullptr, "MaxDistanceFormation" ),
	// DeliverPayload (same names are used by the DeliverPayloadAIUpdate module and by the OCL nugget)
	RANGE_REAL( nullptr, "DeliveryDistance" ),
	RANGE_REAL( nullptr, "PreOpenDistance" ),
	RANGE_REAL( nullptr, "DiveStartDistance" ),
	RANGE_REAL( nullptr, "DiveEndDistance" ),
	RANGE_REAL( nullptr, "StrafeLength" ),
	RANGE_REAL( nullptr, "DeliveryDecalRadius" ),
	RANGE_COORD( nullptr, "DropOffset" ),
	RANGE_COORD( nullptr, "DropVariance" ),

	// ---- Object modules: unit behaviour ranges --------------------------------------------------
	RANGE_REAL( nullptr, "ScanRange" ),									// AutoFindHealing, CleanupHazard, CommandButtonHunt, PilotFindVehicle, PointDefenseLaser
	RANGE_REAL( nullptr, "ClearRangeRequiredToContinueAttackMove" ),	// AssaultTransportAIUpdate
	RANGE_REAL( nullptr, "BoredRange" ),								// DozerAIUpdate, WorkerAIUpdate
	RANGE_REAL( nullptr, "SupplyWarehouseScanDistance" ),				// SupplyTruckAIUpdate, WorkerAIUpdate
	RANGE_REAL( nullptr, "AtPrisonDistance" ),							// POWTruckAIUpdate
	RANGE_REAL( nullptr, "MaxMoveDistanceFromLocation" ),				// CleanupAreaPower
	RANGE_REAL( nullptr, "TriggerDetonationRange" ),					// DemoTrapUpdate
	RANGE_REAL( nullptr, "RepeatDetonateMoveThresh" ),					// MinefieldBehavior
	RANGE_REAL( nullptr, "SpreadTryRange" ),							// FireSpreadUpdate
	RANGE_REAL( nullptr, "DetectionRange" ),							// StealthDetectorUpdate
	RANGE_REAL( nullptr, "RevealDistanceFromTarget" ),					// StealthUpdate
	RANGE_REAL( nullptr, "BonusRange" ),								// WeaponBonusUpdate
	RANGE_REAL( nullptr, "NewShroudRange" ),							// ActiveShroudUpgrade
	RANGE_REAL( nullptr, "FinalVision" ),								// DynamicShroudClearingRangeUpdate
	RANGE_REAL( nullptr, "DistanceAroundObject" ),						// GenerateMinefieldBehavior
	RANGE_REAL( nullptr, "ToleranceDistance" ),							// RailedTransportDockUpdate
	RANGE_REAL( nullptr, "CenterOfMassOffset" ),						// PhysicsBehavior
	RANGE_REAL( nullptr, "StartRadius" ),								// GrantStealthBehavior
	RANGE_REAL( nullptr, "FinalRadius" ),								// GrantStealthBehavior
	RANGE_REAL( nullptr, "RadiusGrowRate" ),							// GrantStealthBehavior (distance per frame)
	RANGE_REAL( nullptr, "EffectRadius" ),								// EMPUpdate
	RANGE_REAL( nullptr, "AffectRadius" ),								// LeafletDropBehavior
	RANGE_REAL( nullptr, "SeismicEffectRadius" ),						// BunkerBusterBehavior
	RANGE_REAL( nullptr, "BlastRadius" ),								// NeutronBlastBehavior
	RANGE_REAL( "AutoHealBehavior", "Radius" ),
	RANGE_REAL( "PropagandaTowerBehavior", "Radius" ),
	RANGE_REAL( "HordeUpdate", "Radius" ),
	RANGE_REAL( "HordeUpdate", "RubOffRadius" ),
	RANGE_REAL( "DemoralizeSpecialPower", "MaxRange" ),
	RANGE_REAL( nullptr, "BaseRange" ),									// DemoralizeSpecialPower
	RANGE_REAL( nullptr, "BonusRangePerCaptured" ),						// DemoralizeSpecialPower
	RANGE_REAL( nullptr, "FatCursorRadius" ),							// DefectorSpecialPower
	RANGE_REAL( nullptr, "StartAbilityRange" ),							// SpecialAbilityUpdate
	RANGE_REAL( nullptr, "AbilityAbortRange" ),
	RANGE_REAL( nullptr, "FleeRangeAfterCompletion" ),
	// SlavedUpdate / MobMemberSlavedUpdate / VeterancyCrateCollide use Int / UInt for these
	RANGE_INT(  "SlavedUpdate", "GuardMaxRange" ),
	RANGE_INT(  "SlavedUpdate", "GuardWanderRange" ),
	RANGE_INT(  "SlavedUpdate", "AttackRange" ),
	RANGE_INT(  "SlavedUpdate", "AttackWanderRange" ),
	RANGE_INT(  "SlavedUpdate", "ScoutRange" ),
	RANGE_INT(  "SlavedUpdate", "ScoutWanderRange" ),
	RANGE_INT(  "SlavedUpdate", "RepairRange" ),
	RANGE_INT(  "SlavedUpdate", "DistToTargetToGrantRangeBonus" ),
	RANGE_REAL( nullptr, "RepairMinAltitude" ),							// SlavedUpdate
	RANGE_REAL( nullptr, "RepairMaxAltitude" ),
	RANGE_INT(  "MobMemberSlavedUpdate", "MustCatchUpRadius" ),
	RANGE_INT(  "MobMemberSlavedUpdate", "NoNeedToCatchUpRadius" ),
	RANGE_UINT( nullptr, "EffectRange" ),								// VeterancyCrateCollide

	// ---- Object modules: flight / altitude / trajectory -----------------------------------------
	RANGE_REAL( nullptr, "MinDropHeight" ),								// ChinookAIUpdate
	RANGE_REAL( nullptr, "RopeFinalHeight" ),
	RANGE_REAL( "JetAIUpdate", "MinHeight" ),
	RANGE_REAL( nullptr, "ParkingOffset" ),								// JetAIUpdate
	RANGE_REAL( nullptr, "SneakyOffsetWhenAttacking" ),
	RANGE_REAL( nullptr, "LockonInitialDist" ),
	RANGE_REAL( nullptr, "ApproachHeight" ),							// FlightDeckBehavior, ParkingPlaceBehavior
	RANGE_REAL( nullptr, "LandingDeckHeightOffset" ),
	RANGE_REAL( nullptr, "FirstHeight" ),								// DumbProjectileBehavior arc heights
	RANGE_REAL( nullptr, "SecondHeight" ),
	RANGE_REAL( nullptr, "DistanceToTravelBeforeTurning" ),				// MissileAIUpdate, NeutronMissileUpdate
	RANGE_REAL( nullptr, "DistanceToTargetBeforeDiving" ),
	RANGE_REAL( nullptr, "DistanceToTargetForLock" ),
	RANGE_REAL( nullptr, "DistanceScatterWhenJammed" ),
	RANGE_REAL( nullptr, "SpecialSpeedHeight" ),						// NeutronMissileUpdate
	RANGE_REAL( nullptr, "SpecialJitterDistance" ),
	RANGE_REAL( nullptr, "TargetHeight" ),								// HeightDieUpdate
	RANGE_REAL( nullptr, "DestroyAttachedParticlesAtHeight" ),
	RANGE_REAL( nullptr, "DestructionAltitude" ),						// SlowDeathBehavior
	RANGE_REAL( nullptr, "ParachuteOpenDist" ),							// ParachuteContain
	RANGE_REAL( nullptr, "KillWhenLandingInWaterSlop" ),
	RANGE_REAL( "StickyBombUpdate", "OffsetZ" ),

	// ---- Special weapons ------------------------------------------------------------------------
	RANGE_REAL( nullptr, "RevealRange" ),								// ParticleUplinkCannonUpdate
	RANGE_REAL( nullptr, "SwathOfDeathDistance" ),
	RANGE_REAL( nullptr, "SwathOfDeathAmplitude" ),
	RANGE_REAL( nullptr, "AttackAreaRadius" ),							// SpectreGunshipUpdate + SpectreGunshipDeploymentUpdate
	RANGE_REAL( nullptr, "RandomOffsetForHowitzer" ),
	RANGE_REAL( nullptr, "TargetingReticleRadius" ),
	RANGE_REAL( nullptr, "GunshipOrbitRadius" ),
	RANGE_REAL( nullptr, "StrafingIncrement" ),
	RANGE_REAL( nullptr, "YSize" ),										// WaveGuideUpdate
	RANGE_REAL( nullptr, "PreferredHeight" ),
	RANGE_REAL( nullptr, "ShorelineEffectDistance" ),
	RANGE_REAL( "WaveGuideUpdate", "DamageRadius" ),
	RANGE_REAL( nullptr, "LinearWaveSpacing" ),
	RANGE_REAL( nullptr, "WaveBendMagnitude" ),
	RANGE_REAL( nullptr, "Blast1InnerRadius" ),							// NeutronMissileSlowDeathUpdate
	RANGE_REAL( nullptr, "Blast1OuterRadius" ),
	RANGE_REAL( nullptr, "Blast2InnerRadius" ),
	RANGE_REAL( nullptr, "Blast2OuterRadius" ),
	RANGE_REAL( nullptr, "Blast3InnerRadius" ),
	RANGE_REAL( nullptr, "Blast3OuterRadius" ),
	RANGE_REAL( nullptr, "Blast4InnerRadius" ),
	RANGE_REAL( nullptr, "Blast4OuterRadius" ),
	RANGE_REAL( nullptr, "Blast5InnerRadius" ),
	RANGE_REAL( nullptr, "Blast5OuterRadius" ),
	RANGE_REAL( nullptr, "Blast6InnerRadius" ),
	RANGE_REAL( nullptr, "Blast6OuterRadius" ),
	RANGE_REAL( nullptr, "Blast7InnerRadius" ),
	RANGE_REAL( nullptr, "Blast7OuterRadius" ),
	RANGE_REAL( nullptr, "Blast8InnerRadius" ),
	RANGE_REAL( nullptr, "Blast8OuterRadius" ),
	RANGE_REAL( nullptr, "Blast9InnerRadius" ),
	RANGE_REAL( nullptr, "Blast9OuterRadius" ),

	// ---- Dynamic geometry: these overwrite the object's GeometryInfo while a weapon effect grows,
	//      so they have to follow -collisionScale or the footprint would snap back to full size.
	COLL_REAL( nullptr, "InitialHeight" ),
	COLL_REAL( nullptr, "InitialMajorRadius" ),
	COLL_REAL( nullptr, "InitialMinorRadius" ),
	COLL_REAL( nullptr, "FinalHeight" ),
	COLL_REAL( nullptr, "FinalMajorRadius" ),
	COLL_REAL( nullptr, "FinalMinorRadius" ),
	COLL_REAL( nullptr, "MaxHeightForDamage" ),

	{ nullptr, nullptr, 0, 0 }
};

// NOT SCALED, deliberately:
//   Angular rates (TurnRate, TurretTurnRate, pitch/roll/yaw/spin rates, *PitchRate): radians per
//     frame are size-independent, and a unit that drives 0.3x as fast at the same TurnRate already
//     turns in a 0.3x radius, which is exactly what a miniature should do.
//   Percentages / factors / scalars (BattlePlan *SightRangeScalar, WeaponBonus RANGE 120%,
//     DamageRadiusScalar, PredictTargetVelocityFactor...): ratios of an already-scaled value.
//   Damage, health, costs, times, durations, veterancy, armor: not distances.
//   AIData block (SkirmishGroupFudgeDistance, MaxRecruitRadius, RepulsedDistance, ...) and script
//     radii/areas stored in the map: AI/script tuning, not object data (see limits in the summary).
//   Particle / FX / W3D draw fields (VolSphereRadius, ScorchSize, beam widths, MaxRecoilDistance,
//     tread/tire animation, ScrollRate, shadow sizes): cosmetic and tied to the model scale,
//     which -unitScale owns.
//   Audio ranges (MinRange/MaxRange in AudioEvent, ZoomMin/MaxDistance, CameraAudibleRadius).
//   Camera / UI (shake, scroll factors, pip offsets, tooltips).
//   Physics impulses (ShockWaveAmount, FlingForce, ThrowForce, Min/MaxForceMagnitude, *PushForce):
//     forces/impulses, not distances; they only drive cosmetic debris.
//   PartitionCellSize / pathfind cell size: spatial-index and pathfinder granularity, not object
//     data. Raising unit density per cell costs performance but not correctness.
//   Terrain / water / bridge / road tables, MinDistFromEdgeOfMapForBuild, SupplyBuildBorder,
//     AllowedHeightVariationForBuilding: map data, not unit data.
//   StructureRubbleHeight (UnsignedByte): rounds to 0 for small structures; the GameData default
//     above is scaled instead.
//   MinesPerSquareFoot / SparksPerCubicFoot (densities): intentionally left so a smaller area
//     simply holds proportionally fewer mines / sparks.
//   ParticleUplinkCannon ManualDrivingSpeed / ManualFastDrivingSpeed, Railroad speeds, neutron
//     missile RelativeSpeed: speeds stored with plain parseReal (not the velocity parser) in
//     special-case modules; left at retail value.
//   FXList / TracerFX "Speed": cosmetic tracer (see the speed exclusions below).

static const int s_tableCount = sizeof(s_table) / sizeof(s_table[0]) - 1;
static unsigned s_hits[sizeof(s_table) / sizeof(s_table[0])];

// Velocity / acceleration fields that must keep their retail value even with -speedScale because
// they animate visuals or UI rather than moving things in the world.
struct SpeedExclusion
{
	const char *ctx;
	const char *field;
};

static const SpeedExclusion s_speedExclusions[] =
{
	{ nullptr, "FloatingTextMoveUpSpeed" },		// InGameUI
	{ nullptr, "FloatingTextVanishRate" },			// InGameUI
	{ nullptr, "TreadAnimationRate" },				// W3DTankDraw / W3DTankTruckDraw (texture scroll)
	{ nullptr, "InitialRecoilSpeed" },				// W3DModelDraw recoil animation
	{ nullptr, "RecoilSettleSpeed" },
	{ nullptr, "LateralScaffoldSpeed" },			// BridgeBehavior repair scaffolds (map bridge size)
	{ nullptr, "VerticalScaffoldSpeed" },
	{ "FXList", "Speed" },							// TracerFXNugget
	{ nullptr, nullptr }
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// HELPERS ////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

static void copyName( char *dst, size_t dstSize, const char *src )
{
	if( src == nullptr )
	{
		dst[0] = 0;
		return;
	}
	strncpy( dst, src, dstSize - 1 );
	dst[dstSize - 1] = 0;
}

static Bool contextMatches( const char *ctx )
{
	if( ctx == nullptr )
		return TRUE;
	if( s_moduleName[0] && stricmp( ctx, s_moduleName ) == 0 )
		return TRUE;
	if( s_blockName[0] && stricmp( ctx, s_blockName ) == 0 )
		return TRUE;
	return FALSE;
}

static Real pickScale( Real specific, Real world )
{
	if( specific > 0.0f )
		return specific;
	if( world > 0.0f )
		return world;
	return 1.0f;
}

static void wsLog( const char *fmt, ... )
{
	if( s_logFile == nullptr )
		return;

	char buf[512];
	va_list args;
	va_start( args, fmt );
	_vsnprintf( buf, sizeof(buf) - 1, fmt, args );
	va_end( args );
	buf[sizeof(buf) - 1] = 0;

	fputs( buf, s_logFile );
	fputc( '\n', s_logFile );
	fflush( s_logFile );
	DEBUG_LOG(( "WorldScale: %s", buf ));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// PUBLIC /////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
void WorldScale::resolveFromGlobalData()
{
	static Bool resolved = FALSE;
	if( TheGlobalData == nullptr || resolved )
		return;

	resolved = TRUE;	// the fields below are overwritten with the effective values, so only ever resolve once

	// Raw values from the command line: 0 (or anything <= 0 / unparsable) means "not given".
	const Real world = TheGlobalData->m_worldScale;
	s_collision = pickScale( TheGlobalData->m_collisionScale, world );
	s_speed     = pickScale( TheGlobalData->m_speedScale,     world );
	s_range     = pickScale( TheGlobalData->m_rangeScale,     world );
	s_trackHits = TheGlobalData->m_worldScaleLog;

	// Publish the effective values back so everything can read them from GlobalData.
	TheWritableGlobalData->m_collisionScale = s_collision;
	TheWritableGlobalData->m_speedScale = s_speed;
	TheWritableGlobalData->m_rangeScale = s_range;
}

//-------------------------------------------------------------------------------------------------
void WorldScale::setBlock( const char *blockName )
{
	copyName( s_blockName, sizeof(s_blockName), blockName );
	s_moduleDepth = 0;
	s_moduleName = "";
	s_field = nullptr;
}

//-------------------------------------------------------------------------------------------------
void WorldScale::setField( const char *fieldName )
{
	s_field = fieldName;
}

//-------------------------------------------------------------------------------------------------
void WorldScale::pushModule( const char *moduleName )
{
	// Module data creation does not nest in practice; the depth cap only guards against a
	// malformed INI. Past the cap the innermost name simply stays active.
	if( s_moduleDepth < MODULE_STACK_DEPTH )
	{
		copyName( s_moduleStack[s_moduleDepth], sizeof(s_moduleStack[0]), moduleName );
		s_moduleName = s_moduleStack[s_moduleDepth];
	}
	++s_moduleDepth;
}

//-------------------------------------------------------------------------------------------------
void WorldScale::popModule()
{
	if( s_moduleDepth > 0 )
		--s_moduleDepth;
	s_moduleName = ( s_moduleDepth > 0 && s_moduleDepth <= MODULE_STACK_DEPTH ) ? s_moduleStack[s_moduleDepth - 1] : "";
}

//-------------------------------------------------------------------------------------------------
Real WorldScale::getSpeedScaleForCurrentField()
{
	if( s_speed == 1.0f )
		return 1.0f;

	for( const SpeedExclusion *e = s_speedExclusions; e->field; ++e )
	{
		if( s_field && strcmp( e->field, s_field ) == 0 && contextMatches( e->ctx ) )
			return 1.0f;
	}
	return s_speed;
}

//-------------------------------------------------------------------------------------------------
void WorldScale::postParseField( const char *fieldName, INIFieldParseProc parse, void *store )
{
	// Fast path: nothing to do at the default scale of 1.0 (the retail case).
	if( s_collision == 1.0f && s_range == 1.0f && !s_trackHits )
		return;

	Int kind;
	if( parse == INI::parseReal || parse == INI::parsePositiveNonZeroReal )
		kind = K_REAL;
	else if( parse == INI::parseCoord3D )
		kind = K_COORD3D;
	else if( parse == INI::parseInt )
		kind = K_INT;
	else if( parse == INI::parseUnsignedInt )
		kind = K_UINT;
	else
		return;

	for( int i = 0; i < s_tableCount; ++i )
	{
		const ScaleEntry *e = &s_table[i];
		if( e->field[0] != fieldName[0] || e->kind != kind || strcmp( e->field, fieldName ) != 0 )
			continue;
		if( !contextMatches( e->ctx ) )
			continue;

		const Real scale = ( e->group == G_COLLISION ) ? s_collision : s_range;
		++s_hits[i];
		if( scale == 1.0f )
			return;

		switch( kind )
		{
			case K_REAL:
				*(Real *)store *= scale;
				break;

			case K_COORD3D:
			{
				Coord3D *c = (Coord3D *)store;
				c->x *= scale;
				c->y *= scale;
				c->z *= scale;
				break;
			}

			case K_INT:
			{
				Int *v = (Int *)store;
				if( *v > 0 )
				{
					// round to nearest, but never let a positive range collapse to 0 (= "disabled")
					Int scaled = (Int)( (Real)*v * scale + 0.5f );
					*v = ( scaled < 1 ) ? 1 : scaled;
				}
				break;
			}

			case K_UINT:
			{
				UnsignedInt *v = (UnsignedInt *)store;
				if( *v > 0 )
				{
					UnsignedInt scaled = (UnsignedInt)( (Real)*v * scale + 0.5f );
					*v = ( scaled < 1 ) ? 1 : scaled;
				}
				break;
			}
		}
		return;
	}
}

//-------------------------------------------------------------------------------------------------
static void openLog()
{
	if( s_logTried )
		return;
	s_logTried = TRUE;

	const Bool wanted = TheGlobalData && ( TheGlobalData->m_worldScaleLog
		|| WorldScale::getCollisionScale() != 1.0f
		|| WorldScale::getSpeedScale() != 1.0f
		|| WorldScale::getRangeScale() != 1.0f );
	if( wanted )
		s_logFile = fopen( "WorldScale.log", "w" );
}

//-------------------------------------------------------------------------------------------------
void WorldScale::logEffective()
{
	openLog();
	if( TheGlobalData )
		wsLog( "command line: worldScale=%g (0 = not given); effective values below", TheGlobalData->m_worldScale );
	wsLog( "effective: collisionScale=%g speedScale=%g rangeScale=%g unitScale(visual)=%g",
		s_collision, s_speed, s_range, TheGlobalData ? TheGlobalData->m_unitVisualScale : 1.0f );
	wsLog( "table: %d entries", s_tableCount );
}

//-------------------------------------------------------------------------------------------------
void WorldScale::logSamples()
{
	openLog();
	if( s_logFile == nullptr )
		return;

	wsLog( "---- sample template values (after all object INIs loaded) ----" );
	wsLog( "Gravity (accel per frame^2) = %g", TheGlobalData ? TheGlobalData->m_gravity : 0.0f );

	static const char *const objects[] = { "ChinaTankOverlord", "AmericaInfantryRanger", "AmericaCommandCenter", "AmericaVehicleChinook", nullptr };
	for( int oi = 0; objects[oi]; ++oi )
	{
		const ThingTemplate *tt = TheThingFactory ? TheThingFactory->findTemplate( AsciiString( objects[oi] ), FALSE ) : nullptr;
		if( tt == nullptr )
		{
			wsLog( "Object %s: not found", objects[oi] );
			continue;
		}
		const GeometryInfo &g = tt->getTemplateGeometryInfo();
		wsLog( "Object %s: Geometry type=%d major=%g minor=%g height=%g boundingCircle=%g | VisionRange=%g ShroudClearingRange=%g",
			objects[oi], (int)g.getGeomType(), g.getMajorRadius(), g.getMinorRadius(), g.getMaxHeightAbovePosition(), g.getBoundingCircleRadius(),
			tt->friend_calcVisionRange(), tt->friend_calcShroudClearingRange() );
	}

	static const char *const locos[] = { "OverlordLocomotor", "ChinookLocomotor", nullptr };
	for( int li = 0; locos[li]; ++li )
	{
		const LocomotorTemplate *lt = TheLocomotorStore ? TheLocomotorStore->findLocomotorTemplate( NAMEKEY( locos[li] ) ) : nullptr;
		if( lt == nullptr )
		{
			wsLog( "Locomotor %s: not found", locos[li] );
			continue;
		}
		wsLog( "Locomotor %s: Speed=%g/frame Accel=%g/frame^2 TurnRate=%g rad/frame PreferredHeight=%g",
			locos[li], lt->m_maxSpeed, lt->m_acceleration, lt->m_maxTurnRate, lt->m_preferredHeight );
	}

	static const char *const weapons[] = { "OverlordTankGun", "StrategyCenterGun", nullptr };
	WeaponBonus noBonus;
	for( int wi = 0; weapons[wi]; ++wi )
	{
		const WeaponTemplate *wt = TheWeaponStore ? TheWeaponStore->findWeaponTemplate( weapons[wi] ) : nullptr;
		if( wt == nullptr )
		{
			wsLog( "Weapon %s: not found", weapons[wi] );
			continue;
		}
		wsLog( "Weapon %s: AttackRange=%g MinimumAttackRange=%g PrimaryDamageRadius=%g SecondaryDamageRadius=%g WeaponSpeed=%g/frame",
			weapons[wi], wt->getAttackRange( noBonus ), wt->getMinimumAttackRange(), wt->getPrimaryDamageRadius( noBonus ),
			wt->getSecondaryDamageRadius( noBonus ), wt->getWeaponSpeed() );
	}

	wsLog( "---- table hits (times a matching field was parsed; 0 = entry unused by the loaded data) ----" );
	for( int i = 0; i < s_tableCount; ++i )
	{
		const ScaleEntry *e = &s_table[i];
		wsLog( "%-8s %-24s %-32s hits=%u", e->group == G_COLLISION ? "COLL" : "RANGE", e->ctx ? e->ctx : "*", e->field, s_hits[i] );
	}
}
