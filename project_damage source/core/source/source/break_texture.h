// общие объявления: переносим шум или текстуру на поверхность каттера и среза
// ProjectDamage break texture surfaces (EXPERIMENTAL) — GPL-3.0-or-later; see LICENSE.
#pragma once
#include "project_damage_core.h"
#include "project_damage_bvh.h"
#include <functional>
#include <memory>
#include <string>

PROJECT_DAMAGE_NS_MACRO_BEGIN
                                                                           
                                                                                 
struct DispImage { int w = 0, h = 0; std::vector<float> v; bool ok = false; };
std::shared_ptr<const DispImage> LoadDispImage(const std::string& path);
                                                                             
                                                                 
double SampleTriplanar(const DispImage& im, const EVec& p, const EVec& n, const EVec& offset, double scale);
                                                                         
                                                                           
                                                                         
m::Manifold BuildTextureBreakCutter(const BreakerSpec& s, Float ampScale, const EVec& bbMin, const EVec& bbMax,
	const std::function<bool(const EVec&)>& insideMesh, std::string* log = nullptr);
                                                                      
                                                                         
struct TexSpec
{
	std::string path;
	double scale = 30.0, depth = 0.0;
	double offX = 0.0, offY = 0.0, offZ = 0.0;
	double edge = 0.0, exit = 1.0;
	                                                                    
	                                                         
	bool proc = false;
	double detail = 4.0, roughness = 0.5, distortion = 0.0;
};
                                                                               
                                                                       
                                                                            
                                                                         
manifold::Manifold TextureDisplaceSolid(const manifold::Manifold& solid, const TexSpec& t, std::string* log = nullptr,
	const std::function<bool(const EVec&)>* inside = nullptr);
                                                                       
std::function<bool(const EVec&)> MakeInsideTest(const MeshView& view, std::shared_ptr<ProjectDamageBVH>& bvh);
TexSpec TexSpecOf(const BreakerSpec& s, Float ampScale);
                                                                             
                                                                          
                                                                          
manifold::Manifold FlareBreakCutter(const manifold::Manifold& solid, const MeshView& view, double W, double B, std::string* log = nullptr);
manifold::Manifold TextureDisplaceClassFaces(const manifold::Manifold& result, const std::vector<Int32>& ids,
	const TexSpec& t, std::string* log = nullptr);
manifold::Manifold TextureDisplaceCutter(const manifold::Manifold& cutter, const BreakerSpec& s, Float ampScale, std::string* log = nullptr);
PROJECT_DAMAGE_NS_MACRO_END
