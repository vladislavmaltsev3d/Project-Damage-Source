// номера полей старого описания эффекта
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

#ifndef OPROJECT_DAMAGE_H__
#define OPROJECT_DAMAGE_H__

enum
{
  PROJECT_DAMAGE_AMOUNT             = 1000,
  PROJECT_DAMAGE_SIZE_MIN           = 1001,
  PROJECT_DAMAGE_SIZE_MAX           = 1002,
  PROJECT_DAMAGE_DEPTH_MIN          = 1003,
  PROJECT_DAMAGE_DEPTH_MAX          = 1004,
  PROJECT_DAMAGE_RANDOM_OFFSET      = 1005,
  PROJECT_DAMAGE_SEED               = 1007,
  PROJECT_DAMAGE_EDGE_THRESHOLD     = 1008,
                                                                                                                             
  PROJECT_DAMAGE_OFFSET             = 1010,
  PROJECT_DAMAGE_DETAIL             = 1011,
  PROJECT_DAMAGE_NOISE_STRENGTH     = 1012,
  PROJECT_DAMAGE_NOISE_SCALE        = 1013,
  PROJECT_DAMAGE_AXIS_X             = 1014,
  PROJECT_DAMAGE_AXIS_Y             = 1015,
  PROJECT_DAMAGE_AXIS_Z             = 1016,
                                                                   
  PROJECT_DAMAGE_TOPOLOGY           = 1018,
  PROJECT_DAMAGE_SEC_AMOUNT         = 1019,
  PROJECT_DAMAGE_SEC_SIZE           = 1020,
  PROJECT_DAMAGE_SEC_DEPTH          = 1021,
  PROJECT_DAMAGE_CORNER_PROB        = 1022,
  PROJECT_DAMAGE_CORNER_MINEDGES    = 1023,
  PROJECT_DAMAGE_CORNER_SIZE_MIN    = 1024,
  PROJECT_DAMAGE_CORNER_SIZE_MAX    = 1025,
  PROJECT_DAMAGE_CORNER_DEPTH_MIN   = 1026,
  PROJECT_DAMAGE_CORNER_DEPTH_MAX   = 1027,
  PROJECT_DAMAGE_NORM_ENABLE        = 1028,
  PROJECT_DAMAGE_NORM_MAXSIZE       = 1029,
  PROJECT_DAMAGE_STRETCH            = 1030,
                                                                                                                                                        
                                                                         
                                                                                     
                                                                         
  PROJECT_DAMAGE_DBG_BUILDCOUNT     = 1033,
  PROJECT_DAMAGE_DBG_FPCOUNT        = 1034,
                                                                        
                                                                 
  PROJECT_DAMAGE_SURF_AMOUNT        = 1036,
  PROJECT_DAMAGE_SURF_SIZE_MIN      = 1037,
  PROJECT_DAMAGE_SURF_SIZE_MAX      = 1038,
  PROJECT_DAMAGE_SURF_DEPTH_MIN     = 1039,
  PROJECT_DAMAGE_SURF_DEPTH_MAX     = 1040,
  PROJECT_DAMAGE_SURF_NOISE         = 1041,
  PROJECT_DAMAGE_SURF_NOISE_IN      = 1042,
  PROJECT_DAMAGE_SURF_DETAIL        = 1043,
  PROJECT_DAMAGE_CORNER_SEC_AMOUNT  = 1044,
  PROJECT_DAMAGE_CORNER_SEC_SIZE    = 1045,
  PROJECT_DAMAGE_CORNER_SEC_DEPTH   = 1046,
  PROJECT_DAMAGE_SURF_SEC_AMOUNT    = 1047,
  PROJECT_DAMAGE_SURF_SEC_SIZE      = 1048,
  PROJECT_DAMAGE_SURF_SEC_DEPTH     = 1049,
                                                                          
  PROJECT_DAMAGE_FRACT_L1_AMOUNT      = 1050,
  PROJECT_DAMAGE_FRACT_L1_WIDTH       = 1051,
  PROJECT_DAMAGE_FRACT_L1_DEPTH       = 1052,
  PROJECT_DAMAGE_FRACT_L1_BIAS        = 1053,
  PROJECT_DAMAGE_FRACT_L1_CHIPS       = 1054,
  PROJECT_DAMAGE_FRACT_L1_CHIP_AMOUNT = 1055,
  PROJECT_DAMAGE_FRACT_L1_CHIP_SIZE   = 1056,
  PROJECT_DAMAGE_FRACT_L1_CHIP_DEPTH  = 1057,
  PROJECT_DAMAGE_FRACT_L2_AMOUNT      = 1058,
  PROJECT_DAMAGE_FRACT_L2_WIDTH       = 1059,
  PROJECT_DAMAGE_FRACT_L2_DEPTH       = 1060,
  PROJECT_DAMAGE_FRACT_L2_BIAS        = 1061,
  PROJECT_DAMAGE_FRACT_L2_CHIPS       = 1062,
  PROJECT_DAMAGE_FRACT_L2_CHIP_AMOUNT = 1063,
  PROJECT_DAMAGE_FRACT_L2_CHIP_SIZE   = 1064,
  PROJECT_DAMAGE_FRACT_L2_CHIP_DEPTH  = 1065,
  PROJECT_DAMAGE_FRACT_L3_AMOUNT      = 1066,
  PROJECT_DAMAGE_FRACT_L3_WIDTH       = 1067,
  PROJECT_DAMAGE_FRACT_L3_DEPTH       = 1068,
  PROJECT_DAMAGE_FRACT_L3_BIAS        = 1069,
  PROJECT_DAMAGE_FRACT_L3_CHIPS       = 1070,
  PROJECT_DAMAGE_FRACT_L3_CHIP_AMOUNT = 1071,
  PROJECT_DAMAGE_FRACT_L3_CHIP_SIZE   = 1072,
  PROJECT_DAMAGE_FRACT_L3_CHIP_DEPTH  = 1073,
  PROJECT_DAMAGE_FRACT_DISP_STRENGTH  = 1074,
  PROJECT_DAMAGE_FRACT_DISP_SCALE     = 1075,
                                                                         
  PROJECT_DAMAGE_SEL_CHIPS            = 1076,
  PROJECT_DAMAGE_SEL_CRACK1           = 1077,
  PROJECT_DAMAGE_SEL_CRACK2           = 1078,
  PROJECT_DAMAGE_SEL_CRACK3           = 1079,
  PROJECT_DAMAGE_SEL_HOLES            = 1080,
  PROJECT_DAMAGE_FRACT_DEPTH_MULT     = 1081,                                           
                                                                                       
                                                                             
                                                                            
  PROJECT_DAMAGE_FRACT_AVOID          = 1082,

  PROJECT_DAMAGE_SEL_BUILDUP          = 1083,                              
                                                                      
                                                            
  PROJECT_DAMAGE_BUILD_COVERAGE       = 1084,
  PROJECT_DAMAGE_BUILD_WIDTH          = 1085,
  PROJECT_DAMAGE_BUILD_HEIGHT         = 1086,
  PROJECT_DAMAGE_BUILD_IRREG          = 1087,
                                                                  
  PROJECT_DAMAGE_BUILD_CORNER_P       = 1091,
  PROJECT_DAMAGE_BUILD_CORNER_S       = 1092,
  PROJECT_DAMAGE_BUILD_BLEND          = 1093,                                                           
                                                

                                                                         
                                                          
  PROJECT_DAMAGE_CHIP_SCALE           = 1106,                                      
  PROJECT_DAMAGE_CORNER_SCALE         = 1107,                 
  PROJECT_DAMAGE_SURF_SCALE           = 1108,                 
  PROJECT_DAMAGE_BUILD_SCALE          = 1109,                 

  PROJECT_DAMAGE_FRACT_DETAIL         = 1110,                                                                  

                                                                           
                                                                         
                                                                            
  PROJECT_DAMAGE_FRACT_L1_SUBDIV      = 1111,
  PROJECT_DAMAGE_FRACT_L2_SUBDIV      = 1112,
  PROJECT_DAMAGE_FRACT_L3_SUBDIV      = 1113,

                                                                            
                                                                         
                                                                           
  PROJECT_DAMAGE_SEL_BUNDLE_CHIPS     = 1114,
  PROJECT_DAMAGE_SEL_BUNDLE_CRACK1    = 1115,
  PROJECT_DAMAGE_SEL_BUNDLE_CRACK2    = 1116,
  PROJECT_DAMAGE_SEL_BUNDLE_CRACK3    = 1117,
  PROJECT_DAMAGE_SEL_BUNDLE_HOLES     = 1118,
  PROJECT_DAMAGE_SEL_BUNDLE_BUILDUP   = 1119,
  PROJECT_DAMAGE_SEL_BUNDLE           = 1120,
                                                                          
                                                                      
  PROJECT_DAMAGE_SEL_BASE             = 1121,
  PROJECT_DAMAGE_SEL_BUNDLE_BASE      = 1122,

                                                                       
                                                                     
  PROJECT_DAMAGE_CORNER_DETAIL        = 1123,

                                                                            
                                                                              
                                           
  PROJECT_DAMAGE_GLOBAL_SCALE         = 1124,

                                                                          
                                                                         
                                                                              
                                                                             
                                        
  PROJECT_DAMAGE_SELE_CHIPS           = 1125,
  PROJECT_DAMAGE_SELE_CRACK1          = 1126,
  PROJECT_DAMAGE_SELE_CRACK2          = 1127,
  PROJECT_DAMAGE_SELE_CRACK3          = 1128,
  PROJECT_DAMAGE_SELE_HOLES           = 1129,
  PROJECT_DAMAGE_SELE_BUILDUP         = 1130,
  PROJECT_DAMAGE_SELE_BUNDLE_CHIPS    = 1131,
  PROJECT_DAMAGE_SELE_BUNDLE_CRACK1   = 1132,
  PROJECT_DAMAGE_SELE_BUNDLE_CRACK2   = 1133,
  PROJECT_DAMAGE_SELE_BUNDLE_CRACK3   = 1134,
  PROJECT_DAMAGE_SELE_BUNDLE_HOLES    = 1135,
  PROJECT_DAMAGE_SELE_BUNDLE_BUILDUP  = 1136,
  PROJECT_DAMAGE_SELE_BUNDLE          = 1137,

                                                                      
                                                                           
                                                                              
                              
                                                                          
                                                
                                                                              
                                                                       
                                                                              
                                                               
                                                                             
                                                                         
                                   
  PROJECT_DAMAGE_SDS_WEIGHT_RIM       = 1138,
  PROJECT_DAMAGE_SDS_WEIGHT_BASE      = 1139,
                                                                             
                                                                          
                                                                            
                                           
  PROJECT_DAMAGE_SDS_SUBDIV           = 1140,

                                                                          
                                                                                      
                                                                      
                                                                                    
                                                                            
                                                                   
                                                                     
                          
  PROJECT_DAMAGE_MANUAL_GLOBAL        = 1141,
  PROJECT_DAMAGE_MANUAL_CHIPS         = 1142,
  PROJECT_DAMAGE_MANUAL_SURFACE       = 1143,
  PROJECT_DAMAGE_MANUAL_BUILDUP       = 1144,
  PROJECT_DAMAGE_MANUAL_FRACTURE      = 1145,
  PROJECT_DAMAGE_MANUAL_CORNER        = 1146,

                                                                          
                                                                           
                                                                         
                                                                
  PROJECT_DAMAGE_FRACT_SCALE          = 1147,

                                                                            
                                                                       
                                                                          
                                                                         
                                                                
  PROJECT_DAMAGE_BUILD_DETAIL         = 1148,

                                                                                 
                                                                                    
                                                                                      
                                                                                        
                                                                             
                                                                              
                                                                                      
                                                                
  PROJECT_DAMAGE_BREAK_LIST           = 1149,                                                            
  PROJECT_DAMAGE_BREAK_ADD            = 1150,                                     
  PROJECT_DAMAGE_BREAK_SCALE          = 1151,                                                          
  PROJECT_DAMAGE_BREAK_SURFDMG        = 1152,                                                   
  PROJECT_DAMAGE_BREAK_MANUAL         = 1153,                                                               

                                                                              
                                                                 
  PROJECT_DAMAGE_SEL_BREAK            = 1154,
  PROJECT_DAMAGE_SEL_BREAK_BUNDLE     = 1155,
  PROJECT_DAMAGE_SELE_BREAK           = 1156,
  PROJECT_DAMAGE_SELE_BREAK_BUNDLE    = 1157,

                                                                           
                                                                         
                                                                            
                                                                           
                                                                            
                                                   
  PROJECT_DAMAGE_CHIPS_ENABLE        = 1158,
  PROJECT_DAMAGE_SURF_ENABLE         = 1159,
  PROJECT_DAMAGE_BUILD_ENABLE        = 1160,
  PROJECT_DAMAGE_FRACT_ENABLE        = 1161,
  PROJECT_DAMAGE_CORNER_ENABLE       = 1162,

                                                                            
                                                                             
                                                                          
                                                              
  PROJECT_DAMAGE_BREAK_RIM_AMOUNT    = 1163,
  PROJECT_DAMAGE_BREAK_RIM_SIZE      = 1164,
  PROJECT_DAMAGE_BREAK_RIM_DEPTH     = 1165,
                                                                             
                                                                          
                                                                         
                                                                             
                                                                        
  PROJECT_DAMAGE_BREAK_RIM_ENABLE    = 1166,

                                                                          
                                                                           
                                                                              
                                                                      
  PROJECT_DAMAGE_ALL_OFF             = 1167,

                                                                            
                                                                              
                                                                              
                                                                             
                                                                        
                                                                        
  PROJECT_DAMAGE_KEEP_UVS            = 1168,

                                                                    
                                                                            
                                                                            
                                                                     
  PROJECT_DAMAGE_OPT_ENABLE          = 1169,
  PROJECT_DAMAGE_OPT_TOLERANCE       = 1170,

                                                                           
                                                                              
                                                                             
                                                                             
                                                                            
                                                 
                                                                             
                                                                           
                                                                             
                                                    
                                                                            
  PROJECT_DAMAGE_CHIP_RIMSOFT        = 1171,
  PROJECT_DAMAGE_CORNER_RIMSOFT      = 1172,
  PROJECT_DAMAGE_FRACT_RIMSOFT       = 1173,

                                                                              
                                                                              
                                                                                      
                                                                          
                                                                           
                                                  
                                                                       
                                                                           
                                                                           
                                                
                                                                             
                                                                             
                                                     
  PROJECT_DAMAGE_GLOBAL_DETAIL       = 1174,

                                                                       
                                                                             
                                                                                  
                                                                          
                                                           
                                                                                     
                                                                  
                                                                           
                                                 
  PROJECT_DAMAGE_DEFER_MS            = 1175,

                                              
                                                                            
                                                                          
                                                                           
                                                                            
                                                                              
                                                                        
  PROJECT_DAMAGE_FIELD               = 1176,
                                                                            
                                                                              
                                                                         
  PROJECT_DAMAGE_FIELD_AMOUNT        = 1177,
                                                                       
                                                                           
  PROJECT_DAMAGE_FIELD_SIZE          = 1178,

  PROJECT_DAMAGE_CLEAN_ENABLE        = 1179,
  PROJECT_DAMAGE_CLEAN_TOLERANCE     = 1180,

                                                                             
                                                                           
                                                                             
                                                                             

                                                                         
                                                                         
                                                                         
                                                                           
                                                                       
  PROJECT_DAMAGE_AVOID_SEL           = 1184,
  PROJECT_DAMAGE_AVOID_CHIPS         = 1185,
  PROJECT_DAMAGE_AVOID_SURF          = 1186,
  PROJECT_DAMAGE_AVOID_BUILD         = 1187,
  PROJECT_DAMAGE_AVOID_FRACT         = 1188,
  PROJECT_DAMAGE_AVOID_CORNER        = 1189,

                                                                              
                                                           
                                                                          
                                                                   
  PROJECT_DAMAGE_KEEP_SEL            = 1190,

                                                                        
                                                                            
                                                                             
                                                                               
                                                                               
                                                                               
                                                            
                                        
  PROJECT_DAMAGE_FRACT_GUIDE         = 1191,
                                                                           
                                                                       
                                                                         
                                                                       
                                                                                      
                                      
  PROJECT_DAMAGE_FRACT_TILE_OFFSET   = 1192,
  PROJECT_DAMAGE_FRACT_TILE_SEED     = 1193,

                                                                               
                                                                          
                                                                                        
                                                                                      
                                                                            
                                                                             
                                                                           
                                                                         
                                                                            
                                                                           
  PROJECT_DAMAGE_PRESET_LIST         = 1194,
  PROJECT_DAMAGE_PRESET_LOAD         = 1195,
  PROJECT_DAMAGE_PRESET_SAVE         = 1196,
  PROJECT_DAMAGE_PRESET_DELETE       = 1197,

  PROJECT_DAMAGE_GROUP_DISTRIBUTION = 2000,
  PROJECT_DAMAGE_GROUP_CHIPS        = 2001,
  PROJECT_DAMAGE_GROUP_EDGES        = 2002,
  PROJECT_DAMAGE_GROUP_MESH         = 2003,
  PROJECT_DAMAGE_GROUP_SECONDARY    = 2004,
  PROJECT_DAMAGE_GROUP_CORNER       = 2005,
  PROJECT_DAMAGE_GROUP_DETAIL       = 2006,
  PROJECT_DAMAGE_GROUP_SURFACE      = 2008,
  PROJECT_DAMAGE_GROUP_CORNER_SEC   = 2009,
  PROJECT_DAMAGE_GROUP_SURFACE_SEC  = 2010,
  PROJECT_DAMAGE_GROUP_MAIN         = 2007,
  PROJECT_DAMAGE_GROUP_FRACTURE     = 2011,
  PROJECT_DAMAGE_GROUP_FRACT_L1     = 2012,
  PROJECT_DAMAGE_GROUP_FRACT_L2     = 2013,
  PROJECT_DAMAGE_GROUP_FRACT_L3     = 2014,
  PROJECT_DAMAGE_GROUP_FRACT_DISP   = 2015,
  PROJECT_DAMAGE_GROUP_SELECTIONS   = 2016,
  PROJECT_DAMAGE_GROUP_BUILDUP      = 2017,
                                                                   
  PROJECT_DAMAGE_GROUP_BUILD_CORNER = 2019,
                                                 
  PROJECT_DAMAGE_GROUP_GLOBAL       = 2021,                                               
                                                                               
                                                                                     
  PROJECT_DAMAGE_GROUP_FRACT_OPTS   = 2023,                                                      
  PROJECT_DAMAGE_GROUP_SEL_POLY     = 2024,                                        
  PROJECT_DAMAGE_GROUP_SEL_EDGE     = 2025,                                     
                                                                         
                                                                
                                                                          
  PROJECT_DAMAGE_GROUP_SURF_SHAPE   = 2026,                                 
  PROJECT_DAMAGE_GROUP_SURF_NOISE   = 2027,                                  
  PROJECT_DAMAGE_GROUP_CORNER_SHAPE = 2028,                                 
  PROJECT_DAMAGE_GROUP_BUILD_BEAD   = 2029,                                        
  PROJECT_DAMAGE_GROUP_BREAK        = 2030,                                                                                                                              
  PROJECT_DAMAGE_GROUP_BREAK_RIM    = 2031,                                                   
  PROJECT_DAMAGE_GROUP_OPTIMIZE     = 2032,                                                 
  PROJECT_DAMAGE_GROUP_FIELD        = 2033,                                                    
  PROJECT_DAMAGE_GROUP_CLEANUP      = 2034,                                                
  PROJECT_DAMAGE_GROUP_PRESETS      = 2035,                                                            
  PROJECT_DAMAGE_GROUP_AVOID        = 2036,                                              
  PROJECT_DAMAGE_GROUP_FRACT_TILE   = 2037,                                              

                                                              
  PROJECT_DAMAGE_AXIS_OFF           = 0,
  PROJECT_DAMAGE_AXIS_ON            = 1,

                                             
  PROJECT_DAMAGE_TOPO_TRIANGLES     = 0,
  PROJECT_DAMAGE_TOPO_QUADS         = 1,
  PROJECT_DAMAGE_TOPO_NGONS         = 2,
  PROJECT_DAMAGE_TOPO_KEEP          = 3,
};

#endif
