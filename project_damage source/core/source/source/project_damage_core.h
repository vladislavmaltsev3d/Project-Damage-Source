// общие объявления: собираем повреждения и возвращаем итоговую геометрию
// ProjectDamage - worn edge damage cut into mesh objects.
// Copyright (C) 2026 Cornelius Dammrich
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// This file is also conveyed by its author, the sole copyright holder,
// under a separate proprietary licence as part of the ProjectDamage plugin for
// Cinema 4D. That does not affect your rights under the GPL above.
// Modified 2026-10-15: comments removed, chip/crack/crater generation and cutter construction reworked.

#pragma once

#include "fabric_tears_params.h"
#include "concrete_damage_params.h"
#include "concrete_hit_params.h"
#include "plaster_displace_params.h"
#include "concrete_pits_params.h"
#include "metal_tear_params.h"
#include "clay_params.h"
#include "edge_bevel_params.h"
#include "damaged_bevel_params.h"
#include "scratches_draw_params.h"
#include "metal_deform_params.h"
#include "wood_damage_params.h"
#include "lath_damage_params.h"
#include "damage_debris_params.h"
#include "rb_fracture_params.h"
#include "cell_fracture_params.h"
#include "hang_hole_params.h"
#include "wall_hole_params.h"
#include "impact_deform_params.h"
#include "wallpaper_params.h"
#include "break_rebar_params.h"
#include "bullet_impacts_params.h"
#include "building_destruction_params.h"
#include "ground_cracks_params.h"
#include "brick_damage_params.h"
#include "broken_glass_params.h"
#include "ground_fissure_params.h"
#include "rusty_holes_params.h"
#include "voxel_erosion_params.h"
#include "project_damage_types.h"
#include "project_damage_host.h"
#ifndef PROJECT_DAMAGE_STANDALONE
#include "c4d.h"
#endif

#include <memory>
#include <string>
#include <vector>

#ifdef PROJECT_DAMAGE_STANDALONE
    #define PROJECT_DAMAGE_NS_MACRO_BEGIN
    #define PROJECT_DAMAGE_NS_MACRO_END
#elif API_VERSION >= 2024900
    #define PROJECT_DAMAGE_NS_MACRO_BEGIN namespace cinema {
    #define PROJECT_DAMAGE_NS_MACRO_END }
#else
    #define PROJECT_DAMAGE_NS_MACRO_BEGIN
    #define PROJECT_DAMAGE_NS_MACRO_END
#endif

#ifdef PROJECT_DAMAGE_STANDALONE
#include <cstdint>
#include <cmath>
#include <limits>
using Int32 = int32_t;  using UInt32 = uint32_t;
using Int64 = int64_t;  using UInt64 = uint64_t;
using Float = double;   using Bool  = bool;
static constexpr Int32 NOTOK = -1;
template <typename T> struct LIMIT {
    static constexpr T MAX = std::numeric_limits<T>::max();
    static constexpr T MIN = std::numeric_limits<T>::lowest();
};
inline Float Abs(Float v)          { return std::fabs(v); }
inline Float FMin(Float a, Float b) { return a < b ? a : b; }
inline Float FMax(Float a, Float b) { return a < b ? b : a; }
inline Float Clamp01(Float a)      { const Float t = a < 0.0 ? 0.0 : a; return t > 1.0 ? 1.0 : t; }
#endif

PROJECT_DAMAGE_NS_MACRO_BEGIN

#ifndef PROJECT_DAMAGE_STANDALONE
inline EMat EM(const Matrix& mat)
{
	return EMat(m::vec3(mat.sqmat.v1.x, mat.sqmat.v1.y, mat.sqmat.v1.z),
		m::vec3(mat.sqmat.v2.x, mat.sqmat.v2.y, mat.sqmat.v2.z),
		m::vec3(mat.sqmat.v3.x, mat.sqmat.v3.y, mat.sqmat.v3.z),
		m::vec3(mat.off.x, mat.off.y, mat.off.z));
}
#endif

constexpr Int32 PROJECT_DAMAGE_SEL_COUNT = 8;
constexpr Int32 PROJECT_DAMAGE_SELE_COUNT = 7;

struct CustomShape
{
	std::vector<EVec> pts;
	std::vector<Int32>  tris;
	Float  size = 0.0;
	UInt64 hash = 0;
};

#ifndef PROJECT_DAMAGE_STANDALONE
const CustomShape* BreakerCustomShape(BaseObject* br);

Int32 EffectiveBreakerMode(const BaseObject* br);
#endif

struct FractProximity
{
	Float x = 0.0, y = 0.0, z = 0.0;
	Float radius = 0.0;
	Float strength = 1.0;
};

struct BreakerSpec
{
	Int32  mode       = 0;
	Bool   flip       = false;
	Int32  seed       = 12345;
	Float  length     = 120.0;
	Float  width      = 120.0;
	Float  radius     = 50.0;
	Float  thickness  = 10.0;
	Float  resolution = 1.0;
	Float  strength   = 1.0;
	Float  macroH     = 0.08;
	Float  turbH      = 0.07;
	Float  fineH      = 0.03;
	Float  noiseScale = 1.0;
	Float  rimAmp     = 0.13;
	Float  rimScale   = 1.0;
	EMat   rel = EMat(m::la::identity);
	std::shared_ptr<const CustomShape> custom;
	UInt64 customHash = 0;
    Int32 customSheet = 0;
    Bool rebar = true;
	                                                                   
	                                                                          
	                                                                       
	                                                                     
	                                                                 
	Int32  surfMode    = 0;
	std::string texPath;
	Float  texOffX = 0.0, texOffY = 0.0, texOffZ = 0.0;                          
	Float  texScale    = 30.0;                                  
	Float  texStrength = 1.5;                                      
	Float  texEdge     = 0.0;                                                     
	Float  texExit     = 2.0;                                                              
	Float  texMacro    = 1.0;                                                                 
	                                                                         
	                                                                        
	                                                                      
	                                        
	Bool   smoothSlope = false;
	Float  smoothWidth = 0.35;
	                                                                       
	                                                                       
	                                  
	Float  surfSmooth = 0.0;                                                    
	Float  surfDetail = 4.0;                                                    
	Float  surfRough  = 0.5;                                             
	Float  surfCrease = 1.0;                                                  
	Float  surfWarp   = 0.0;                                                   
	Float  macroScale = 1.0;                                                
	Float  turbScale  = 1.0;
	Float  fineScale  = 1.0;
	// Wood looks; 0 leaves the surface exactly as without them.
	Float  surfAniso  = 0.0;     // 0..1: noise stretched along surfAnisoAxis
	Int32  surfAnisoAxis = 0;    // breaker-local X, Y, Z (a Plane has no Y: Y acts as Z)
	Float  surfTerrace = 0.0;    // 0 = off, else about this many flat steps
};

#ifndef PROJECT_DAMAGE_STANDALONE
BreakerSpec ReadBreakerSpec(const BaseContainer* bc, Int32 mode);
#endif

struct ProjectDamageParams
{
    std::vector<ProjectDamageParams> fractureLayers;
    std::vector<Float> fractureGuidePoints;
    std::vector<Int32> fractureGuideCounts, fractureGuideLevels;
    Int32 fractureEffectId = 4;
                                                                                   
    std::vector<UInt64> incrementalKeys;

	Int32 amount        = 120;
	Float sizeMin       = 6.0;
	Float sizeMax       = 11.0;
	Float depthMin      = 4.0;
	Float depthMax      = 8.0;
	Float chipRimSoft   = 0.0;
	Float offset        = 0.0;
	Float randomOffset  = 0.5;
	Float detail        = 1.0;
	Float noiseStrength = 0.0;
	Float noiseScale    = 8.0;
	Float noiseRoughness = 0.0;
	Float noiseDistortion = 0.0;
	Int32 seed          = 5;
	Float thresholdDeg  = 60.0;
	Bool  axisX         = true;
	Bool  axisY         = true;
	Bool  axisZ         = true;
	Int32 secAmount     = 0;
	Float secSize       = 0.5;
	Float secDepth      = 0.15;
	Float cornerProb     = 0.0;
	Int32 cornerMinEdges = 3;
	Float cornerSizeMin  = 6.0;
	Float cornerSizeMax  = 12.0;
	Float cornerDepthMin = 3.0;
	Float cornerDepthMax = 6.0;
	Float cornerRimSoft  = 0.0;
	Float cornerDetail   = 1.0;
	Int32 cornerSecAmount = 0;
	Float cornerSecSize   = 0.5;
	Float cornerSecDepth  = 0.15;
	                                                                          
	                                                                       
	                                                                   
	                                                          
	Int32 cornerStyle = 0;
	Float cornerRough = 0.5;
	Float cornerTilt  = 0.5;
	Float cornerFlakes = 0.5;
	                                                                       
	                                                     
	Bool  cornerPaintOnly = false;
	Float cornerPaintRadius = 8.0;
	std::vector<double> cornerPaintPts;
	std::vector<Int32>  cornerPaintCount;
	Bool  normEnable  = false;
	Float normMaxSize = 0.3;
	Float stretch     = 1.0;
	                                                                   
	                                                                       
	                                                                         
	                                                                      
	                                                                          
	                                                                     
	                                                                  
	                                                              
	Int32 chipSeed = 0;
	Float chipCluster = 0.0;
	Float chipFine    = 0.0;
	Float chipProfile = 0.0;
	Float chipAsym    = 0.0;
	Float chipGuard   = 0.0;
	Int32 surfAmount   = 0;
	Int32 surfSeed     = 0;
	Float surfSizeMin  = 3.0;
	Float surfSizeMax  = 9.0;
	Float surfDepthMin = 0.3;
	Float surfDepthMax = 1.5;
	Float surfNoise    = 1.0;
	Float surfNoiseIn  = 0.4;
	Bool surfPaintOnly = false;
	Float surfPaintSnap = 5.0;
	Float surfDetail   = 1.0;
	Int32 surfSecAmount = 0;
	Float surfSecSize   = 0.5;
	Float surfSecDepth  = 0.15;
	                                                                     
	                                                                    
	                                                                         
	                                                                     
	                                                                   
	Float surfCluster = 0.0;
	Float surfSizeBias = 0.0;
	Float surfDepthFollow = 0.0;
	Float surfGuard = 0.0;
	                                                                    
	                                                                   
	                                                                         
	                                                                        
	Int32 surfType = 0;
	Float surfGrainX = 0.0, surfGrainY = 0.0, surfGrainZ = 1.0;                              
	Float surfKnifeJitter = 10.0;                                               
	Float surfKnifeBack = 0.15;                                               
	struct FractLevel
	{
		Int32 amount     = 0;
		Float width      = 2.0;
		Float depth      = 6.0;
		Float bias       = 0.0;
		Bool  chips      = true;
		Float chipAmount = 4.0;
		Float chipSize   = 2.5;
		Float chipDepth  = 1.0;
		Int32 subdiv = 0;
		Float paintRadius = 3.0;
		Float length = 0.0;
		                                                                    
		                                                                 
		                                                                     
		                                                                      
		Float branches = 0.0;
		Float branchLength = 0.35;
                                                                             
                                                                       
        Float paintLengthMin = -1.0;
        Float paintDistortion = -1.0;
        Float paintDistortionScale = 20.0;
	};
	FractLevel fract[3];
	Bool fractPaintOnly = false;
	Int32 fractRecipe = 2;
	Float fractThicknessCap = 0.45;
	Bool fractThrough = false;
	Float fractThroughMinPiece = 0.001;
	                                                                            
	Bool fractThroughFill = false;
	Float fractThroughFillSize = 0.0;
	Float fractThroughRough = 0.5;
	                                                                         
	                                                                          
	                                                                            
	                                                                          
	                                                                          
	                                                    
	Bool fractPhysics = false;
	Float fractPhysicsDepth = 0.0;
	Int32 fractPhysicsClose = 6;                                                   
	                                                                            
	                                                                           
	                                                                           
	                                                                   
	Bool fractRounded = false;
	                                                                                               
	Bool fractSurfaceProxy = false;
	Bool surfaceGuide = false;                                                   
	                                                                           
	                                                                         
	                                                                          
	                                                                
	Bool damageShading = true;
	Float damageSmoothAngle = 80.0;
	Float baseSmoothAngle = 30.0;
	                                                                        
	                                                                       
	                                                                         
	                                                                      
	                                                                  
	std::vector<std::vector<EVec>> fractHelperStrokes;
	Float fractHelperSnap = 0.0;                                        
	                                                                    
	                                                                        
	                                                                  
	                                                                            
	                                                           
	                                                                      
	                                                                      
	                                                       
	                                                                        
	                                                                   
	Int32 fractType = 0;
	Int32 fractImpacts = 1;                                                            
	Float fractGrainX = 0.0, fractGrainY = 0.0, fractGrainZ = 1.0;                  
	Float fractGrainPull = 0.8;                                              
	Int32 fractProxMode = 0;
	Float fractProxRadius = 40.0;
	Float fractProxStrength = 0.6;
	Int32 fractRadialSpokes = 7;
	Int32 fractRadialRings = 3;
	Float fractRadialRingFill = 0.8;
	Float fractRadialCrush = 0.12;
	Int32 fractGlassSpokes = 14;
	Int32 fractGlassRings = 7;
	Float fractGlassBranch = 0.5;
	Float fractGlassRingFill = 0.75;
	Float fractGlassWidth = 0.25;
	Float fractGlassDepth = 0.4;
	Float fractGlassCrush = 0.05;
	std::vector<FractProximity> fractProximity;
	Float fractDispStrength = 1.0;
	Float fractDispScale    = 1.0;
	Float fractDetail       = 1.0;
	Float fractRimSoft      = 0.0;
	Float fractDepthMult    = 1.0;
	Float fractTileOffset   = 0.0;
	Int32 fractTileSeed     = 0;
	                                                                          
	                                                                       
	                                                                       
	                                           
	Float fractRimJag = 0.0;
	Float fractFlare  = 0.0;
	                                                                    
	                                                                        
	                                                                       
	                                                                 
	                                                                     
	                                                                      
	                                                                        
	                                                           
	Bool  fractShardEnable = false;
	Int32 fractShardDir = 0;
	Float fractShardAmount = 1.5;
	Float fractShardRadius = 40.0;
	Float fractShardFalloff = 1.0;
	Float fractShardTilt = 0.5;
	Float fractShardRandom = 0.35;
	Int32 fractShardSeed = 0;
	std::vector<FractProximity> fractShardPoints;
	Float buildCoverage   = 0.0;
	Float buildWidth      = 3.0;
	Float buildHeight     = 0.4;
	Float buildIrreg      = 2.33;
	Float buildCornerProb = 0.0;
	Float buildCornerSize = 3.0;
	Float buildDetail     = 1.0;
	Float buildBlend      = 0.0;
	                                                                       
	                                                                    
	                                                                  
	                                                     
	Int32 buildShape = 0;
	Float buildLumps = 0.0;
	                                                                          
	                                                                 
	Bool  buildPaintOnly = false;
	Float buildPaintRadius = 5.0;
	std::vector<double> buildPaintPts;
	std::vector<Int32>  buildPaintCount;
	                                                                          
	                                                                        
	                                                         
	Float pileHeight = 0.6;
	Float pileLumps = 0.0;
	Float pileDetail = 1.0;
	Int32 depositAmount = 0;
	Int32 depositStyle = 0;
	Float depositSizeMin = 3.0;
	Float depositSizeMax = 7.0;
	Float depositHeight = 1.0;
	Float depositGravity = 0.8;
	Float depositGravityX = 0.0, depositGravityY = 0.0, depositGravityZ = -1.0;
	Float depositShelter = 0.0;
	Float depositShelterRadius = 15.0;
	Float depositEmbed = 0.3;
	Int32 depositLayers = 1;
	Float depositPorosity = 0.0;
	Bool depositSeparate = false;
	Float depositDetail = 0.0;                                 
	FabricTearParams fabric;
                                                                               
    Int32 profileCore = -1;                                             
	ConcreteDamageParams concrete;
	ConcreteHitParams hit;
	PlasterDisplaceParams plaster;
	ConcretePitsParams pits;
    MetalTearParams metalTear;
    ClayParams clay;                                                 
    EdgeBevelParams bevel;
    DamagedBevelParams damagedBevel;                              
    ScratchDrawParams scratch;
    MetalDeformParams metal;
	WoodDamageParams wood;
	LathDamageParams lath;
	DamageDebrisParams debris;
	RBFractureParams rbf;
	CellFractureParams cellf;                                                               
	WallHoleParams wallHole;
	HangHoleParams hang;
	ImpactDeformParams deform;                                                                                                                                                                                                                                                                 
	WallpaperParams wallpaper;
    BulletImpactParams bullet;
    BuildingDestructionParams blast;
    GroundCracksParams ground;
    BrickDamageParams brick;
    BrokenGlassParams glass;
    GroundFissureParams fissure;
    RustyHoleParams rusty;
    VoxelErosionParams erosion;
	Bool rustUsePaint = false;
	Bool rustEnable = false;
	Int32 rustSeed = 0;
	Float rustIntensity = 1.0;
	Float rustEdgeBias = 0.0;
	Float rustBottomBias = 0.0;
	Float rustNoise = 0.6;
	Float rustNoiseScale = 6.0;
	Float rustDetail = 0.6;
	Float rustCrustStart = 0.45;
	Float rustCrustHeight = 0.25;
	Float rustCrustRough = 0.6;
	Int32 rustPits = 300;
	Float rustPitStart = 0.25;
	Float rustPitSize = 0.4;
	Float rustPitDepth = 0.25;
	Float rustHoleStart = 0.9;
	Float rustHoleDepth = 3.0;
	Float rustFlakes = 0.6;
	Float rustFlakeSize = 1.5;
	Bool rustPaint = true;
	Float rustPaintStart = 0.3;
	Float rustBand = 0.2;
	Float rustPaintFlakes = 0.5;
	Float rustCraze = 0.6;
	Float rustCrazeCell = 0.8;
	Int32 rustBlisters = 150;
	Float rustBlisterSize = 0.6;
	Float rustStreak = 0.8;
	Float rustStreakLength = 15.0;
	Float rustGravityX = 0.0, rustGravityY = 0.0, rustGravityZ = -1.0;
	Bool heatEnable = false;
	Int32 heatSeed = 0;
	Float heatIntensity = 0.8;
	Bool heatDiffuse = true;
	Float heatSpread = 5.0;
	Float heatDuration = 1.0;
	Int32 heatSteps = 8;
	Float heatCharStart = 0.45;
	Float heatShrinkDepth = 0.8;
	Float heatDetail = 8.0;
	Float heatBlockRound = 0.5;
	Float heatCellMin = 5.0;
	Float heatCellMax = 15.0;
	Int32 heatCrackCells = 80;
	Float heatCrackWidth = 0.25;
	Float heatCrackDepth = 1.0;
	Int32 heatBlisters = 30;
	Float heatBlisterMin = 1.0;
	Float heatBlisterMax = 3.0;
	Float heatBlisterLow = 0.2;
	Float heatBlisterHigh = 0.75;
	Float heatBlisterBurst = 0.35;
	Float heatCharLayer = 0.1;

	Bool coatingZoneEnable = false;
	Float coatingZoneThreshold = 0.5;
	Float coatingZoneSmooth = 1.0;
	Float coatingZoneInset = 0.5;
	Float coatingZoneDetail = 2.0;
	Int32 coatingFlakeSide = 0;
	Float coatingFlakeSideX = 0.0;
	Float coatingFlakeSideY = 0.0;
	Float coatingFlakeSideZ = 1.0;
	Bool coatingEnable = false;
	Int32 coatingAmount = 24;
	Float coatingThickness = 0.08;
	Float coatingPatchMin = 2.0, coatingPatchMax = 8.0;
	Float coatingJagged = 0.65, coatingFlakes = 0.5;
	Float coatingCurl = 0.6, coatingCracks = 0.3;
	Int32 coatingSeed = 0;
	Float coatingSatellites = 0.65;
	Float coatingStretch = 0.0;
	Float coatingDirX = 1.0;
	Float coatingDirY = 0.0;
	Float coatingDirZ = 0.0;
	Float coatingEdgeWrap = 1.0;
	Bool coatingFacingOnly = false;
	Float coatingFacingX = 0.0;
	Float coatingFacingY = 0.0;
	Float coatingFacingZ = 1.0;
	Float coatingFacingAngle = 90.0;
	Float coatingFlakeSize = 1.0;
	Float coatingCrackLength = 1.0;
	Int32 topology    = 0;
	Bool  baseMap     = false;
    Bool preserveSurfaceShells = false;
	const IFieldMask* mask = nullptr;
	const IFieldMask* plasterZone = nullptr;
	std::vector<BreakerSpec> breakers;
	Bool  breakSurfDmg = false;
	Int32 breakRimAmount = 40;
	Float breakRimSize   = 1.0;
	Float breakRimDepth  = 1.0;
    BreakRebarParams breakRebar;
	Bool  avoidChips  = true;
	Bool  avoidSurf   = true;
	Bool  avoidBuild  = true;
	Bool  avoidFract  = true;
	Bool  avoidCorner = true;
};

constexpr Int32 PROJECT_DAMAGE_SEL_SLOT_ID = 1069343;

class ProjectDamageDamageCache;
std::vector<Int32> DamageCacheIncrementalBuilt(ProjectDamageDamageCache*);
std::vector<Int32> DamageCacheIncrementalReused(ProjectDamageDamageCache*);
ProjectDamageDamageCache* DamageCacheAlloc();
void DamageCacheFree(ProjectDamageDamageCache*& cache);

std::vector<uint8_t> DamageCacheSerialize(const ProjectDamageDamageCache* cache);
Bool DamageCacheDeserialize(ProjectDamageDamageCache* cache, const uint8_t* data, size_t len);

struct SurfacePaintRegion
{
	std::vector<EVec> points;
	EVec normal = EVec(0.0, 0.0, 1.0);
};

#include "project_damage_draw_mask.h"

struct CoreInput
{
	MeshView view = MeshView{ nullptr, 0, nullptr, 0 };
	const m::Manifold* box = nullptr;
	Int32 baseId = 0;
	                                                                          
	                                                                              
	const std::vector<double>* cornerUvs = nullptr;
	EVec bbMp = EVec(0.0);
	EVec bbRad = EVec(0.0);
	const std::vector<bool>* avoidPoly = nullptr;
	const std::vector<double>* rustWeights = nullptr;
	const std::vector<double>* fabricWeights = nullptr;
	const std::vector<bool>* fabricAvoidPoly = nullptr;
	const std::vector<std::vector<EVec>>* concreteStrokes = nullptr;
	const std::vector<double>* heatWeights = nullptr;
	const std::vector<double>* coatingZoneWeights = nullptr;
	const std::vector<std::vector<EVec>>* guideSplines = nullptr;
	                                                                           
	                                                          
	                                                                           
	                                                                    
	                                                                         
	const std::vector<double>* srcCornerNormals = nullptr;
	const std::vector<std::vector<EVec>>* rbfCutStrokes = nullptr;
	const std::vector<std::vector<EVec>>* rbfImpactStrokes = nullptr;
	const std::vector<std::vector<EVec>>* cellfStrokes = nullptr;                                         
	const std::vector<Int32>* guideLevels = nullptr;
	const std::vector<SurfacePaintRegion>* surfacePaint = nullptr;
	const std::vector<SurfacePaintRegion>* coatingPaint = nullptr;
	const std::vector<SurfacePaintRegion>* rustPaint = nullptr;
    const std::vector<std::vector<EVec>>* woodPaint = nullptr;
    const std::vector<std::vector<EVec>>* lathPaint = nullptr;
    const std::vector<SurfacePaintRegion>* wallpaperPaint = nullptr;
    const std::vector<std::vector<EVec>>* bulletPaint = nullptr;
    const std::vector<std::vector<EVec>>* groundPaint = nullptr;
    const std::vector<std::vector<EVec>>* brickPaint = nullptr;
    const std::vector<std::vector<EVec>>* bevelPaint = nullptr;
                                                                      
    const std::vector<std::vector<EVec>>* damagedBevelPaint = nullptr;
    const std::vector<std::vector<EVec>>* scratchStrokes = nullptr;
    const std::vector<std::vector<EVec>>* chipPaint = nullptr;
    double chipPaintRadius = 0.0;                                       
    const std::vector<std::vector<EVec>>* fissurePaint = nullptr;
                                                                      
    const std::vector<std::vector<EVec>>* rustyPaint = nullptr;
    const std::vector<SurfacePaintRegion>* glassHolePaint = nullptr;
    const std::vector<SurfacePaintRegion>* glassImpactPaint = nullptr;
    const std::vector<SurfacePaintRegion>* fabricHoles = nullptr;
    const std::vector<SurfacePaintRegion>* fabricEdges = nullptr;
    const DrawMaskInput* drawMask = nullptr;
};

m::Manifold BuildInputManifold(const double* xyz, size_t nv, const int32_t* abcd, size_t np,
	uint32_t originalId, bool stampFaceIds, const double* uvs);

struct CoreResult
{
	Bool inputValid = false;
	std::vector<EVec> beadPoints, pilePoints;
	std::vector<ProjectDamagePoly> beadPolys, pilePolys;
	std::vector<EVec> depositPoints;
	std::vector<ProjectDamagePoly> depositPolys;
	std::vector<float> rustSource;
	std::vector<float> rustPoint, rustStreakPoint, coatingRust, coatingStreak;
	std::vector<float> heatExposure, heatCharAmount, heatCharDepth;
	std::vector<EVec> brickMeshPoints;
    std::vector<ProjectDamagePoly> brickMeshPolys;
    std::vector<Int32> brickPiece;
    Int32 brickCount=0;
    std::vector<EVec> lathPoints;
    std::vector<ProjectDamagePoly> lathPolys;
                                                                         
                                                                     
    std::vector<EVec> debrisPoints;
    std::vector<ProjectDamagePoly> debrisPolys;
    std::vector<Int32> debrisPiece;
                                                                             
                                                                            
                                                                           
    std::vector<Int32> debrisBasePoly;
    std::vector<double> debrisUvs;
    std::vector<double> debrisNormals;                                                     
                                                                            
                                                                            
                                                                 
    std::vector<EVec> rbfPoints;
    std::vector<ProjectDamagePoly> rbfPolys;
    std::vector<Int32> rbfPiece, rbfBasePoly;
    std::vector<double> rbfUvs, rbfNormals, rbfPieceVolume, rbfPieceCenter;
                                                                                
    std::vector<EVec> cellfPoints;
    std::vector<ProjectDamagePoly> cellfPolys;
    std::vector<Int32> cellfPiece, cellfBasePoly;
    std::vector<double> cellfUvs, cellfNormals, cellfPieceVolume, cellfPieceCenter;
    Bool debrisSourceUvs = false;
    std::vector<Int32> lathPiece;
	std::vector<EVec> coatingPoints;
	std::vector<ProjectDamagePoly> coatingPolys;
	Bool passthrough = false;
	std::vector<EVec>     points;
	std::vector<ProjectDamagePoly> polys;
	m::MeshGL mesh;
	std::vector<Int32> propCorner;
	std::vector<Int32> basePolyOf;
    std::vector<Int32> effectIds;
	std::vector<Int32> chipPolys;
	std::vector<Int32> crackPolys[3];
	std::vector<Int32> holePolys;
    std::vector<Int32> wallpaperPolys[3];
    std::vector<Int32> wallpaperBasePolys;
                                                                              
                                                                      
    std::vector<Int32> wallpaperFlapPolys;
    std::vector<Int32> brickPolys[5];
    std::vector<Int32> extraPolys[4];
    std::vector<float> edgeWear, scratchMask, metalDent, metalCrease, metalContact;
    std::vector<float> bevelDamage;                                                               
    std::vector<double> bevelSegPts; std::vector<int32_t> bevelSegTris;                                       
	std::vector<Int32> buildPolys;
	std::vector<Int32> breakPolys;
                                                                               
    std::vector<double> cornerNormals;
    std::vector<EVec> concreteRebarPoints;
    std::vector<ProjectDamagePoly> concreteRebarPolys;
    std::vector<EVec> rebarPoints;
    std::vector<double> rebarBrickFrame, rebarBrickCut;
    std::vector<Int32> rebarBrickKey;
                                                                  
    std::vector<double> wallHoleRimFrame, wallHoleRimCut, wallHoleRubbleFrame;
    std::vector<Int32> wallHoleRimKey, wallHoleRubbleKey;
    std::vector<ProjectDamagePoly> rebarPolys;
    std::vector<Int32> rebarPolyBar;
    std::vector<Int32> rebarPolyPart;
    std::vector<float> rebarExposure;
    std::vector<float> rebarGrain;                                                                       
    Int32 rebarBars = 0;
};

CoreResult BuildWornCore(const CoreInput& in, const ProjectDamageParams& params,
	const ProjectDamageHost& host, ProjectDamageDamageCache* cache = nullptr,
	UInt64 cacheFingerprint = 0);

#ifndef PROJECT_DAMAGE_STANDALONE
Bool RegisterProjectDamageBreaker();
#endif

PROJECT_DAMAGE_NS_MACRO_END
