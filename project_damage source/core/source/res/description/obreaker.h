// номера полей старого описания каттера
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

#ifndef OBREAKER_H__
#define OBREAKER_H__

                                                                                                    
                                                                                                 
                                                                              
                                                                                         
                                                                               
                                                                              
                                                                         
                                                                                                  
                                        
#define PLUGIN_ID_BREAKER 1069670

enum
{
  BREAKER_MODE           = 1000,
  BREAKER_SEED           = 1001,
  BREAKER_LENGTH         = 1002,
  BREAKER_WIDTH          = 1003,
  BREAKER_RADIUS         = 1004,
  BREAKER_RESOLUTION     = 1005,
  BREAKER_STRENGTH       = 1006,
  BREAKER_MACRO_H        = 1007,
  BREAKER_TURB_H         = 1008,
  BREAKER_FINE_H         = 1009,
  BREAKER_NOISE_SCALE    = 1010,
  BREAKER_RIM_AMP        = 1011,
  BREAKER_RIM_SCALE      = 1012,
  BREAKER_FLIP           = 1013,
  BREAKER_SHOW_SURFACE   = 1014,
  BREAKER_GIZMO          = 1015,
  BREAKER_GIZMO_WIDTH    = 1016,
                                                                            
                                                                             
                                                                               
                                             
  BREAKER_GIZMO_ROUND    = 1017,
  BREAKER_HATCH          = 1018,
  BREAKER_HATCH_COLOR    = 1019,
  BREAKER_HATCH_WIDTH    = 1020,
                                                                            
                                                                            
                                                       
  BREAKER_THICKNESS      = 1021,
                                                                               
                                                                                         
                                                                                      
                                                                                                          
                                                                       
                                                         
  BREAKER_MANUAL         = 1022,
  BREAKER_MANUAL_VIEW    = 1023,

  BREAKER_GROUP_SIZE     = 2001,
  BREAKER_GROUP_NOISE    = 2002,
  BREAKER_GROUP_EDGE     = 2003,
  BREAKER_GROUP_VIEW     = 2004,

                                  
  BREAKER_MODE_PLANE     = 0,
  BREAKER_MODE_SPHERE    = 1,
  BREAKER_MODE_STRIP     = 2,
                                                                              
                                                                               
                                                                              
                                                                               
                                                                         
  BREAKER_MODE_CUSTOM    = 3,
};

#endif
