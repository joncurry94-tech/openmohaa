/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_light.c

#include "tr_local.h"

#define	DLIGHT_AT_RADIUS		16
// at the edge of a dlight's influence, this amount of light will be added

#define	DLIGHT_MINIMUM_RADIUS	16		
// never calculate a range less than this to prevent huge light numbers


/*
===============
R_TransformDlights

Transforms the origins of an array of dlights.
Used by both the front end (for DlightBmodel) and
the back end (before doing the lighting calculation)
===============
*/
void R_TransformDlights( int count, dlight_t *dl, orientationr_t *or) {
	int		i;
	vec3_t	temp;

	for ( i = 0 ; i < count ; i++, dl++ ) {
		VectorSubtract( dl->origin, or->origin, temp );
		dl->transformed[0] = DotProduct( temp, or->axis[0] );
		dl->transformed[1] = DotProduct( temp, or->axis[1] );
		dl->transformed[2] = DotProduct( temp, or->axis[2] );
		// HZM gl2 [2026-09-26] Phase S1: the cone axis in the same frame (ProjectDlightTexture's u_HzmLightSpot and
		// the bmodel cull use it beside `transformed`). Re-normalised: a scaled entity axis would otherwise change the
		// length the shader reads k back from.
		if ( dl->hzmSpot == HZM_SPOT_READY ) {
			dl->hzmAxisLocal[0] = DotProduct( dl->hzmAxis, or->axis[0] );
			dl->hzmAxisLocal[1] = DotProduct( dl->hzmAxis, or->axis[1] );
			dl->hzmAxisLocal[2] = DotProduct( dl->hzmAxis, or->axis[2] );
			if ( VectorNormalize( dl->hzmAxisLocal ) <= 0.0f ) {
				VectorCopy( dl->hzmAxis, dl->hzmAxisLocal );
			}
		}
	}
}

/*
=============
R_DlightBmodel

Determine which dynamic lights may effect this bmodel
=============
*/
void R_DlightBmodel( bmodel_t *bmodel ) {
	int			i, j;
	dlight_t	*dl;
	int			mask;
	msurface_t	*surf;

	// transform all the lights
	R_TransformDlights( tr.refdef.num_dlights, tr.refdef.dlights, &tr.or );

	mask = 0;
	for ( i=0 ; i<tr.refdef.num_dlights ; i++ ) {
		dl = &tr.refdef.dlights[i];

		// see if the point is close enough to the bounds to matter
		for ( j = 0 ; j < 3 ; j++ ) {
			if ( dl->transformed[j] - bmodel->bounds[1][j] > dl->radius ) {
				break;
			}
			if ( bmodel->bounds[0][j] - dl->transformed[j] > dl->radius ) {
				break;
			}
		}
		if ( j < 3 ) {
			continue;
		}

		// HZM gl2 [2026-09-26] Phase S1: a cone reaches far less than its sphere (a 28 deg / 1100 u cone is ~9% of
		// the volume) - test the bmodel's bounding sphere against it, in the same local frame. Spots only.
		if ( dl->hzmSpot == HZM_SPOT_READY ) {
			vec3_t	bc, bh;

			VectorAdd( bmodel->bounds[0], bmodel->bounds[1], bc );
			VectorScale( bc, 0.5f, bc );
			VectorSubtract( bmodel->bounds[1], bc, bh );
			if ( !R_HZM_SpotSphereTouches( dl->transformed, dl->hzmAxisLocal, dl->hzmCosOuter, bc, VectorLength( bh ) ) ) {
				continue;
			}
		}

		// we need to check this light
		mask |= 1 << i;
	}

	tr.currentEntity->needDlights = (mask != 0);

	// set the dlight bits in all the surfaces
	for ( i = 0 ; i < bmodel->numSurfaces ; i++ ) {
		surf = tr.world->surfaces + bmodel->firstSurface + i;

		switch(*surf->data)
		{
			case SF_FACE:
			case SF_GRID:
			case SF_TRIANGLES:
				((srfBspSurface_t *)surf->data)->dlightBits = mask;
				break;

			default:
				break;
		}
	}
}


/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

extern	cvar_t	*r_ambientScale;
extern	cvar_t	*r_directedScale;
extern	cvar_t	*r_debugLight;

/*
=================
R_SetupEntityLightingGrid

=================
*/
#if 0
static void R_SetupEntityLightingGrid( trRefEntity_t *ent, world_t *world ) {
	vec3_t	lightOrigin;
	int		pos[3];
	int		i, j;
	byte	*gridData;
	float	frac[3];
	int		gridStep[3];
	vec3_t	direction;
	float	totalFactor;

	if ( ent->e.renderfx & RF_LIGHTING_ORIGIN ) {
		// separate lightOrigins are needed so an object that is
		// sinking into the ground can still be lit, and so
		// multi-part models can be lit identically
		VectorCopy( ent->e.lightingOrigin, lightOrigin );
	} else {
		VectorCopy( ent->e.origin, lightOrigin );
	}

	VectorSubtract( lightOrigin, world->lightGridOrigin, lightOrigin );
	for ( i = 0 ; i < 3 ; i++ ) {
		float	v;

		v = lightOrigin[i]*world->lightGridInverseSize[i];
		pos[i] = floor( v );
		frac[i] = v - pos[i];
		if ( pos[i] < 0 ) {
			pos[i] = 0;
		} else if ( pos[i] > world->lightGridBounds[i] - 1 ) {
			pos[i] = world->lightGridBounds[i] - 1;
		}
	}

	VectorClear( ent->ambientLight );
	VectorClear( ent->directedLight );
	VectorClear( direction );

	assert( world->lightGridData ); // NULL with -nolight maps

	// trilerp the light value
	gridStep[0] = 8;
	gridStep[1] = 8 * world->lightGridBounds[0];
	gridStep[2] = 8 * world->lightGridBounds[0] * world->lightGridBounds[1];
	gridData = world->lightGridData + pos[0] * gridStep[0]
		+ pos[1] * gridStep[1] + pos[2] * gridStep[2];

	totalFactor = 0;
	for ( i = 0 ; i < 8 ; i++ ) {
		float	factor;
		byte	*data;
		int		lat, lng;
		vec3_t	normal;
		#if idppc
		float d0, d1, d2, d3, d4, d5;
		#endif
		factor = 1.0;
		data = gridData;
		for ( j = 0 ; j < 3 ; j++ ) {
			if ( i & (1<<j) ) {
				if ( pos[j] + 1 > world->lightGridBounds[j] - 1 ) {
					break; // ignore values outside lightgrid
				}
				factor *= frac[j];
				data += gridStep[j];
			} else {
				factor *= (1.0f - frac[j]);
			}
		}

		if ( j != 3 ) {
			continue;
		}

		if (world->lightGrid16)
		{
			uint16_t *data16 = world->lightGrid16 + (int)(data - world->lightGridData) / 8 * 6;
			if (!(data16[0]+data16[1]+data16[2]+data16[3]+data16[4]+data16[5])) {
				continue;	// ignore samples in walls
			}
		}
		else
		{
			if (!(data[0]+data[1]+data[2]+data[3]+data[4]+data[5]) ) {
				continue;	// ignore samples in walls
			}
		}
		totalFactor += factor;
		#if idppc
		d0 = data[0]; d1 = data[1]; d2 = data[2];
		d3 = data[3]; d4 = data[4]; d5 = data[5];

		ent->ambientLight[0] += factor * d0;
		ent->ambientLight[1] += factor * d1;
		ent->ambientLight[2] += factor * d2;

		ent->directedLight[0] += factor * d3;
		ent->directedLight[1] += factor * d4;
		ent->directedLight[2] += factor * d5;
		#else
		if (world->lightGrid16)
		{
			// FIXME: this is hideous
			uint16_t *data16 = world->lightGrid16 + (int)(data - world->lightGridData) / 8 * 6;

			ent->ambientLight[0] += factor * data16[0] / 257.0f;
			ent->ambientLight[1] += factor * data16[1] / 257.0f;
			ent->ambientLight[2] += factor * data16[2] / 257.0f;

			ent->directedLight[0] += factor * data16[3] / 257.0f;
			ent->directedLight[1] += factor * data16[4] / 257.0f;
			ent->directedLight[2] += factor * data16[5] / 257.0f;
		}
		else
		{
			ent->ambientLight[0] += factor * data[0];
			ent->ambientLight[1] += factor * data[1];
			ent->ambientLight[2] += factor * data[2];

			ent->directedLight[0] += factor * data[3];
			ent->directedLight[1] += factor * data[4];
			ent->directedLight[2] += factor * data[5];
		}
		#endif
		lat = data[7];
		lng = data[6];
		lat *= (FUNCTABLE_SIZE/256);
		lng *= (FUNCTABLE_SIZE/256);

		// decode X as cos( lat ) * sin( long )
		// decode Y as sin( lat ) * sin( long )
		// decode Z as cos( long )

		normal[0] = tr.sinTable[(lat+(FUNCTABLE_SIZE/4))&FUNCTABLE_MASK] * tr.sinTable[lng];
		normal[1] = tr.sinTable[lat] * tr.sinTable[lng];
		normal[2] = tr.sinTable[(lng+(FUNCTABLE_SIZE/4))&FUNCTABLE_MASK];

		VectorMA( direction, factor, normal, direction );
	}

	if ( totalFactor > 0 && totalFactor < 0.99 ) {
		totalFactor = 1.0f / totalFactor;
		VectorScale( ent->ambientLight, totalFactor, ent->ambientLight );
		VectorScale( ent->directedLight, totalFactor, ent->directedLight );
	}

	VectorScale( ent->ambientLight, r_ambientScale->value, ent->ambientLight );
	VectorScale( ent->directedLight, r_directedScale->value, ent->directedLight );

	VectorNormalize2( direction, ent->lightDir );
}
#endif

static void R_SetupEntityLightingGrid(trRefEntity_t* ent, world_t* world);


/*
===============
LogLight
===============
*/
static void LogLight( trRefEntity_t *ent ) {
	int	max1, max2;

	if ( !(ent->e.renderfx & RF_FIRST_PERSON ) ) {
		return;
	}

	max1 = ent->ambientLight[0];
	if ( ent->ambientLight[1] > max1 ) {
		max1 = ent->ambientLight[1];
	} else if ( ent->ambientLight[2] > max1 ) {
		max1 = ent->ambientLight[2];
	}

	max2 = ent->directedLight[0];
	if ( ent->directedLight[1] > max2 ) {
		max2 = ent->directedLight[1];
	} else if ( ent->directedLight[2] > max2 ) {
		max2 = ent->directedLight[2];
	}

	ri.Printf( PRINT_ALL, "amb:%i  dir:%i\n", max1, max2 );
}

/*
=================
R_SetupEntityLighting

Calculates all the lighting values that will be used
by the Calc_* functions
=================
*/
void R_SetupEntityLighting( const trRefdef_t *refdef, trRefEntity_t *ent ) {
	int				i;
	dlight_t		*dl;
	float			power;
	vec3_t			dir;
	float			d;
	vec3_t			lightDir;
	vec3_t			lightOrigin;

	// lighting calculations 
	if ( ent->lightingCalculated ) {
		return;
	}
	ent->lightingCalculated = qtrue;

	//
	// trace a sample point down to find ambient light
	//
	if ( ent->e.renderfx & RF_LIGHTING_ORIGIN ) {
		// separate lightOrigins are needed so an object that is
		// sinking into the ground can still be lit, and so
		// multi-part models can be lit identically
		VectorCopy( ent->e.lightingOrigin, lightOrigin );
	} else {
		VectorCopy( ent->e.origin, lightOrigin );
	}

	// if NOWORLDMODEL, only use dynamic lights (menu system, etc)
	if ( !(refdef->rdflags & RDF_NOWORLDMODEL ) 
		&& tr.world->lightGridData ) {
		R_SetupEntityLightingGrid( ent, tr.world );
	} else {
		ent->ambientLight[0] = ent->ambientLight[1] = 
			ent->ambientLight[2] = tr.identityLight * 150;
		ent->directedLight[0] = ent->directedLight[1] = 
			ent->directedLight[2] = tr.identityLight * 150;
		VectorCopy( tr.sunDirection, ent->lightDir );
	}

	// bonus items and view weapons have a fixed minimum add
	if ( 1 /* ent->e.renderfx & RF_MINLIGHT */ ) {
		// give everything a minimum light add
		ent->ambientLight[0] += tr.identityLight * 32;
		ent->ambientLight[1] += tr.identityLight * 32;
		ent->ambientLight[2] += tr.identityLight * 32;
	}

	//
	// modify the light by dynamic lights
	//
	d = VectorLength( ent->directedLight );
	VectorScale( ent->lightDir, d, lightDir );

	for ( i = 0 ; i < refdef->num_dlights ; i++ ) {
		dl = &refdef->dlights[i];
		VectorSubtract( dl->origin, lightOrigin, dir );
		d = VectorNormalize( dir );

		power = DLIGHT_AT_RADIUS * ( dl->radius * dl->radius );
		if ( d < DLIGHT_MINIMUM_RADIUS ) {
			d = DLIGHT_MINIMUM_RADIUS;
		}
		d = power / ( d * d );
		// HZM gl2 [2026-09-26] Phase S1: a spot adds only inside its cone and range (every other light untouched)
		if ( dl->hzmSpot == HZM_SPOT_READY ) {
			d *= R_HZM_DlightFactorAt( dl, lightOrigin, 0.0f );
		}

		VectorMA( ent->directedLight, d, dl->color, ent->directedLight );
		VectorMA( lightDir, d, dir, lightDir );
	}

	// clamp lights
	// FIXME: old renderer clamps (ambient + NL * directed) per vertex
	//        check if that's worth implementing
	{
		float r, g, b, max;

		r = ent->ambientLight[0];
		g = ent->ambientLight[1];
		b = ent->ambientLight[2];

		max = MAX(MAX(r, g), b);

		if (max > 255.0f)
		{
			max = 255.0f / max;
			ent->ambientLight[0] *= max;
			ent->ambientLight[1] *= max;
			ent->ambientLight[2] *= max;
		}

		r = ent->directedLight[0];
		g = ent->directedLight[1];
		b = ent->directedLight[2];

		max = MAX(MAX(r, g), b);

		if (max > 255.0f)
		{
			max = 255.0f / max;
			ent->directedLight[0] *= max;
			ent->directedLight[1] *= max;
			ent->directedLight[2] *= max;
		}
	}


	if ( r_debugLight->integer ) {
		LogLight( ent );
	}

	// save out the byte packet version
	((byte *)&ent->ambientLightInt)[0] = ri.ftol(ent->ambientLight[0]);
	((byte *)&ent->ambientLightInt)[1] = ri.ftol(ent->ambientLight[1]);
	((byte *)&ent->ambientLightInt)[2] = ri.ftol(ent->ambientLight[2]);
	((byte *)&ent->ambientLightInt)[3] = 0xff;
	
	// transform the direction to local space
	VectorNormalize( lightDir );
	ent->modelLightDir[0] = DotProduct( lightDir, ent->e.axis[0] );
	ent->modelLightDir[1] = DotProduct( lightDir, ent->e.axis[1] );
	ent->modelLightDir[2] = DotProduct( lightDir, ent->e.axis[2] );

	VectorCopy(lightDir, ent->lightDir);
}

/*
=================
R_LightForPoint
=================
*/
int R_LightForPoint( vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir )
{
	trRefEntity_t ent;
	
	if ( tr.world->lightGridData == NULL )
	  return qfalse;

	Com_Memset(&ent, 0, sizeof(ent));
	VectorCopy( point, ent.e.origin );
	R_SetupEntityLightingGrid( &ent, tr.world );
	VectorCopy(ent.ambientLight, ambientLight);
	VectorCopy(ent.directedLight, directedLight);
	VectorCopy(ent.lightDir, lightDir);

	return qtrue;
}

#if 0
int R_LightDirForPoint( vec3_t point, vec3_t lightDir, vec3_t normal, world_t *world )
{
	trRefEntity_t ent;
	
	if ( world->lightGridData == NULL )
	  return qfalse;

	Com_Memset(&ent, 0, sizeof(ent));
	VectorCopy( point, ent.e.origin );
	R_SetupEntityLightingGrid( &ent, world );

	if (DotProduct(ent.lightDir, normal) > 0.2f)
		VectorCopy(ent.lightDir, lightDir);
	else
		VectorCopy(normal, lightDir);

	return qtrue;
}
#endif

int R_CubemapForPoint( vec3_t point )
{
	int cubemapIndex = -1;

	if (r_cubeMapping->integer && tr.numCubemaps)
	{
		int i;
		vec_t shortest = (float)WORLD_SIZE * (float)WORLD_SIZE;

		for (i = 0; i < tr.numCubemaps; i++)
		{
			vec3_t diff;
			vec_t length;

			VectorSubtract(point, tr.cubemaps[i].origin, diff);
			length = DotProduct(diff, diff);

			if (shortest > length)
			{
				shortest = length;
				cubemapIndex = i;
			}
		}
	}

	return cubemapIndex + 1;
}

//
// OPENMOHAA-specific stuff
//

/*
===============
R_CoopSmoothEntityLight / R_CoopResetEntityLightSmoothing

HZM gl2 [user 2026-09-09, bug-2554] "when I am in the truck or walking and go under a light, it just
kinda flashes bright and then when I leave it gets dark, it all seems instant".

It IS instant. Neither renderer damps entity lighting between frames: tr_scene.c:254-255 clears both
bLightGridCalculated and sphereCalculated as every entity is added to the scene, and the light sphere
is rebuilt per draw-surf list. So the frame the sphere stops selecting a light - a spot cone losing
the entity, a light falling past the r_entlight_maxcalc cap, a classification flip - the new colour
is simply the colour. This eases it instead.

KEYED ON THE GAME ENTITY NUMBER (refEntity_t::entityNumber, tr_types.h:141), never on the refdef
slot: backEndData->entities[] is refilled every frame, so a slot key would blend one object's light
into an unrelated one's.

ADVANCES AT MOST ONCE PER ENTITY PER FRAME. RB_SetupCharLighting runs per DRAW BATCH, so without the
timestamp test the blend would advance once per batch and the time constant would depend on how many
surfaces a model happens to have.

SNAPS ACROSS A DISCONTINUITY. Unseen for more than half a second - spawned, teleported, respawned, or
the entity number recycled onto a different object - and there is nothing meaningful to blend from;
so restart at the new value rather than drag a stale colour across the gap. Time running backwards
means a new map, same treatment, and RE_LoadWorldMap clears the table outright for the case where a
reload lands inside the 500 ms window.
===============
*/
typedef struct {
	vec3_t   rgb;
	vec3_t   dir;
	int      lastTime;
	qboolean valid;
} coopEntLightSmooth_t;

// entity numbers are ENTBITS-wide game slots (12 bits on this build); anything outside is rejected
#define COOP_ENTLIGHT_MAX 4096
static coopEntLightSmooth_t s_coopEntLight[COOP_ENTLIGHT_MAX];

void R_CoopResetEntityLightSmoothing( void )
{
	Com_Memset( s_coopEntLight, 0, sizeof( s_coopEntLight ) );
}

void R_CoopSmoothEntityLight( int entityNumber, vec3_t rgb, vec3_t dir )
{
	coopEntLightSmooth_t *st;
	int                   dtms;
	float                 tau, a, len;
	int                   i;

	if ( !r_entLightSmooth || r_entLightSmooth->value <= 0.0f ) {
		return;
	}
	if ( entityNumber < 0 || entityNumber >= COOP_ENTLIGHT_MAX ) {
		return;
	}

	st  = &s_coopEntLight[entityNumber];
	tau = r_entLightSmooth->value;

	if ( st->valid ) {
		dtms = backEnd.refdef.time - st->lastTime;

		if ( dtms == 0 ) {
			// already advanced this frame (char lighting runs once per draw batch) - hand back the
			// value we settled on, so every batch of one model agrees
			VectorCopy( st->rgb, rgb );
			if ( dir ) {
				VectorCopy( st->dir, dir );
			}
			return;
		}

		if ( dtms > 0 && dtms <= 500 ) {
			a = 1.0f - exp( -( (float)dtms * 0.001f ) / tau );
			if ( a < 0.0f ) { a = 0.0f; } else if ( a > 1.0f ) { a = 1.0f; }

			for ( i = 0; i < 3; i++ ) {
				rgb[i] = st->rgb[i] + ( rgb[i] - st->rgb[i] ) * a;
			}
			if ( dir ) {
				for ( i = 0; i < 3; i++ ) {
					dir[i] = st->dir[i] + ( dir[i] - st->dir[i] ) * a;
				}
				// lerping two unit vectors shortens the result; lightall_fp normalises L anyway,
				// but the CPU consumers do not, so restore it here
				len = VectorLength( dir );
				if ( len > 0.0001f ) {
					VectorScale( dir, 1.0f / len, dir );
				} else {
					VectorCopy( st->dir, dir );
				}
			}
		}
		// dtms < 0 (new map) or > 500 (gone and back) falls through and snaps
	}

	VectorCopy( rgb, st->rgb );
	if ( dir ) {
		VectorCopy( dir, st->dir );
	} else {
		VectorClear( st->dir );
	}
	st->lastTime = backEnd.refdef.time;
	st->valid    = qtrue;
}

/*
===============
RB_GetEntityGridLighting

HZM gl2 re-port (bug-gl2-modellight): gl1 tr_light.c RB_GetEntityGridLighting -
flat grid sample at the entity's trace origin plus dlight falloff, packed to a
byte color for RB_CalcLightGridColor.
===============
*/
static int RB_GetEntityGridLighting()
{
    int       iColor;
    int       i;
    dlight_t *dl;
    float     power;
    vec3_t    vLight;
    vec3_t    dir;
    float     d;
    float    *lightOrigin;

    lightOrigin = backEnd.currentSphere->traceOrigin;
    if (!(backEnd.refdef.rdflags & RDF_NOWORLDMODEL) && tr.world && tr.world->lightGridData) {
        R_GetLightingGridValueSingle(tr.world, backEnd.currentSphere->traceOrigin, vLight);
    } else {
        vLight[0] = vLight[1] = vLight[2] = tr.identityLight * 150.0;
    }

    for (i = 0; i < backEnd.refdef.num_dlights; i++) {
        dl = &backEnd.refdef.dlights[i];
        VectorSubtract(dl->origin, lightOrigin, dir);
        d = VectorLengthSquared(dir);

        power = dl->radius * dl->radius;
        if (power >= d) {
            d = dl->radius * 7500.0 / d;
            // HZM gl2 [2026-09-26] Phase S1: a spot adds only inside its cone and range (others untouched)
            if (dl->hzmSpot == HZM_SPOT_READY) {
                d *= R_HZM_DlightFactorAt(dl, lightOrigin, backEnd.currentSphere->radius);
            }
            VectorMA(vLight, d, dl->color, vLight);
        }
    }

    if (tr.overbrightShift) {
        vLight[0] = tr.overbrightMult * vLight[0];
        vLight[1] = tr.overbrightMult * vLight[1];
        vLight[2] = tr.overbrightMult * vLight[2];
    }

    // normalize
    if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
        float scale = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
        VectorScale(vLight, scale, vLight);
    }

    // clamp ambient
    for (i = 0; i < 3; i++) {
        if (vLight[i] > tr.identityLightByte) {
            vLight[i] = tr.identityLightByte;
        }
    }

    // save out the byte packet version
    ((byte *)&iColor)[0] = (byte)(int)vLight[0];
    ((byte *)&iColor)[1] = (byte)(int)vLight[1];
    ((byte *)&iColor)[2] = (byte)(int)vLight[2];
    ((byte *)&iColor)[3] = 0xff;

    return iColor;
}

/*
===============
RB_SetupEntityGridLighting

HZM gl2 re-port (bug-gl2-modellight): gl1 tr_light.c:1315-1358 - computes the
flat grid color once per entity per frame and propagates it up the
parentEntity chain.
===============
*/
void RB_SetupEntityGridLighting()
{
    trRefEntity_t *ent;
    int            iColor;

    if (backEnd.currentEntity->bLightGridCalculated) {
        return;
    }

    // HZM gl2 (bug-1131): parentEntity is now a refdef-relative slot or ENTITYNUM_NONE
    // (translated in RE_AddRefEntityToScene2). Belt-and-braces: any out-of-range index is
    // treated as no-parent so we can never chase uninitialized refdef slots again.
    iColor = -1;
    for (ent = backEnd.currentEntity;
         ent->e.parentEntity != ENTITYNUM_NONE && ent->e.parentEntity >= 0
         && ent->e.parentEntity < backEnd.refdef.num_entities;
         ent = &backEnd.refdef.entities[ent->e.parentEntity]) {
        trRefEntity_t *newref = &backEnd.refdef.entities[ent->e.parentEntity];
        if (newref == ent) {
            // parentEntity refers to itself
            iColor = newref->iGridLighting;
            break;
        }

        if (newref->bLightGridCalculated) {
            iColor = newref->iGridLighting;
            break;
        }
    }

    if (iColor == -1) {
        // no valid pre-computed ancestor color -> compute from this entity (gl1-equivalent)
        iColor = RB_GetEntityGridLighting();

        // HZM gl2 [user 2026-09-09, bug-2554] ease it toward the previous frame's value instead of
        // replacing it. ONLY in this branch: an entity that took its colour from an ancestor above
        // has taken an ALREADY-smoothed value, and smoothing it again would apply the filter twice
        // down a parent chain.
        if (r_entLightSmooth && r_entLightSmooth->value > 0.0f) {
            vec3_t vSm;
            vSm[0] = (float)((byte *)&iColor)[0];
            vSm[1] = (float)((byte *)&iColor)[1];
            vSm[2] = (float)((byte *)&iColor)[2];
            R_CoopSmoothEntityLight(backEnd.currentEntity->e.entityNumber, vSm, NULL);
            ((byte *)&iColor)[0] = (byte)(int)(vSm[0] < 0.0f ? 0.0f : (vSm[0] > 255.0f ? 255.0f : vSm[0]));
            ((byte *)&iColor)[1] = (byte)(int)(vSm[1] < 0.0f ? 0.0f : (vSm[1] > 255.0f ? 255.0f : vSm[1]));
            ((byte *)&iColor)[2] = (byte)(int)(vSm[2] < 0.0f ? 0.0f : (vSm[2] > 255.0f ? 255.0f : vSm[2]));
            ((byte *)&iColor)[3] = 0xff;
        }
    }

    ent = backEnd.currentEntity;
    for (;;) {
        ent->bLightGridCalculated = qtrue;
        ent->iGridLighting        = iColor;
        // HZM gl2 (bug-1131): same bounds guard as the read walk above
        if (ent->e.parentEntity == ENTITYNUM_NONE || ent->e.parentEntity < 0
            || ent->e.parentEntity >= backEnd.refdef.num_entities) {
            break;
        }

        if (ent == &backEnd.refdef.entities[ent->e.parentEntity]) {
            // parentEntity refers to itself
            break;
        }

        ent = &backEnd.refdef.entities[ent->e.parentEntity];
    }
}

/*
===============
RB_SetupStaticModelGridLighting

HZM gl2 re-port (bug-gl2-modellight): gl1 tr_light.c:1365-1422 - one-time flat
grid sample (plus dlights) for a map-placed static model, cached on the model.
===============
*/
void RB_SetupStaticModelGridLighting(trRefdef_t *refdef, cStaticModelUnpacked_t *ent, const vec3_t lightOrigin)
{
    int       i;
    dlight_t *dl;
    float     power;
    vec3_t    vLight;
    vec3_t    dir;
    float     d;

    if (ent->bLightGridCalculated) {
        return;
    }
    ent->bLightGridCalculated = qtrue;

    if (!(refdef->rdflags & RDF_NOWORLDMODEL) && tr.world && tr.world->lightGridData) {
        R_GetLightingGridValueSingle(tr.world, lightOrigin, vLight);
    } else {
        vLight[0] = vLight[1] = vLight[2] = tr.identityLight * 150.0;
    }

    for (i = 0; i < refdef->num_dlights; i++) {
        dl = &refdef->dlights[i];
        VectorSubtract(dl->origin, lightOrigin, dir);
        d = VectorLengthSquared(dir);

        power = dl->radius * dl->radius;
        if (power >= d) {
            d = dl->radius * 7500.0 / d;
            // HZM gl2 [2026-09-26] Phase S1: a spot adds only inside its cone and range (others untouched)
            if (dl->hzmSpot == HZM_SPOT_READY) {
                d *= R_HZM_DlightFactorAt(dl, lightOrigin, 0.0f);
            }
            VectorMA(vLight, d, dl->color, vLight);
        }
    }

    if (tr.overbrightShift) {
        vLight[0] = tr.overbrightMult * vLight[0];
        vLight[1] = tr.overbrightMult * vLight[1];
        vLight[2] = tr.overbrightMult * vLight[2];
    }

    // normalize
    if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
        float scale = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
        VectorScale(vLight, scale, vLight);
    }

    // clamp ambient
    for (i = 0; i < 3; i++) {
        if (vLight[i] > tr.identityLightByte) {
            vLight[i] = tr.identityLightByte;
        }
    }

    // save out the byte packet version
    ((byte *)&ent->iGridLighting)[0] = (byte)(int)vLight[0];
    ((byte *)&ent->iGridLighting)[1] = (byte)(int)vLight[1];
    ((byte *)&ent->iGridLighting)[2] = (byte)(int)vLight[2];
    ((byte *)&ent->iGridLighting)[3] = 0xff;
}

/*
===============
R_PointInLeaf
===============
*/
mnode_t *R_FindPoint( world_t *world, const vec3_t p ) {
	mnode_t		*node;
	float		d;
	cplane_t	*plane;

	node = world->nodes;
	while( 1 ) {
		if (node->contents != -1) {
			break;
		}
		plane = node->plane;
		d = DotProduct (p,plane->normal) - plane->dist;
		if (d > 0) {
			node = node->children[0];
		} else {
			node = node->children[1];
		}
	}
	
	return node;
}

/*
===============
R_GetLightGridPalettedColor
===============
*/
static byte *R_GetLightGridPalettedColor(world_t *world, int iColor)
{
    return &world->lightGridPalette[iColor * 3];
}

/*
===============
R_GetLightingGridValue
===============
*/
void R_GetLightingGridValue(world_t *world, const vec3_t vPos, vec3_t vAmbientLight, vec3_t vDirectedLight)
{
    byte  *pColor;
    int    iBaseOffset;
    int    i;
    int    iOffset;
    int    iRowPos;
    int    iData;
    int    iLen;
    int    iGridPos[3];
    int    iArrayXStep;
    float  fV;
    float  fFrac[3];
    float  fOMFrac[3];
    float  fWeight, fWeight2;
    float  fTotalFactor;
    int    iCurData;
    vec3_t vLightOrigin;
    byte  *pCurData;

    if (!world || !world->lightGridData || !world->lightGridOffsets) {
		vDirectedLight[0] = vDirectedLight[1] = vDirectedLight[2] = tr.identityLightByte;
		vAmbientLight[0] = vAmbientLight[1] = vAmbientLight[2] = tr.identityLightByte;
        return;
    }

    VectorSubtract(vPos, world->lightGridOrigin, vLightOrigin);

    for (i = 0; i < 3; i++) {
        fV          = vLightOrigin[i] * world->lightGridInverseSize[i];
        iGridPos[i] = floor(fV);
        fFrac[i]    = fV - iGridPos[i];
        fOMFrac[i]  = 1.0 - fFrac[i];

        if (iGridPos[i] < 0) {
            iGridPos[i] = 0;
        } else if (iGridPos[i] > world->lightGridBounds[i] - 2) {
            iGridPos[i] = world->lightGridBounds[i] - 2;
        }
    }

    fTotalFactor = 0;
    iArrayXStep  = world->lightGridBounds[1];
    iBaseOffset  = world->lightGridBounds[0] + iGridPos[1] + iArrayXStep * iGridPos[0];
    VectorClear(vDirectedLight);
    VectorClear(vAmbientLight);

    for (i = 0; i < 4; i++) {
        qboolean bContinue = qfalse;

        switch (i) {
        case 0:
            fWeight  = fOMFrac[0] * fOMFrac[1] * fOMFrac[2];
            fWeight2 = fOMFrac[0] * fOMFrac[1] * fFrac[2];
            iOffset  = world->lightGridOffsets[iBaseOffset] + (world->lightGridOffsets[iGridPos[0]] << 8);
            break;
        case 1:
            fWeight  = fOMFrac[0] * fFrac[1] * fOMFrac[2];
            fWeight2 = fOMFrac[0] * fFrac[1] * fFrac[2];
            iOffset  = world->lightGridOffsets[iBaseOffset + 1] + (world->lightGridOffsets[iGridPos[0]] << 8);
            break;
        case 2:
            fWeight  = fFrac[0] * fOMFrac[1] * fOMFrac[2];
            fWeight2 = fFrac[0] * fOMFrac[1] * fFrac[2];
            iOffset  = world->lightGridOffsets[iBaseOffset + iArrayXStep]
                    + (world->lightGridOffsets[iGridPos[0] + 1] << 8);
            break;
        case 3:
            fWeight  = fFrac[0] * fFrac[1] * fOMFrac[2];
            fWeight2 = fFrac[0] * fFrac[1] * fFrac[2];
            iOffset  = world->lightGridOffsets[iBaseOffset + iArrayXStep + 1]
                    + (world->lightGridOffsets[iGridPos[0] + 1] << 8);
            break;
        }

        iRowPos  = iGridPos[2];
        pCurData = &world->lightGridData[iOffset];
        iData    = 0;

        while (1) {
            while (1) {
                iCurData = (char)pCurData[iData];
                iData++;
                if (iCurData >= 0) {
                    break;
                }

                iLen = -iCurData;
                if (iLen > iRowPos) {
                    iData += iRowPos;

                    if (pCurData[iData]) {
                        pColor = R_GetLightGridPalettedColor(world, pCurData[iData]);
                        VectorMA(vAmbientLight, fWeight, pColor, vAmbientLight);
                        fTotalFactor += fWeight;
                    }

                    iData++;
                    if (iLen - 1 == iRowPos) {
                        iData++;
                    }

                    if (pCurData[iData]) {
                        pColor = R_GetLightGridPalettedColor(world, pCurData[iData]);
                        VectorMA(vDirectedLight, fWeight2, pColor, vDirectedLight);
                        fTotalFactor += fWeight2;
                    }

                    bContinue = qtrue;
                    break;
                }

                iRowPos -= iLen;
                iData += iLen;
            }

            if (bContinue) {
                break;
            }

            iLen = iCurData + 2;
            if (iLen - 1 >= iRowPos) {
                break;
            }

            iRowPos -= iLen;
            iData++;
        }

        if (bContinue) {
            continue;
        }

        if (iLen - 1 > iRowPos) {
            if (!pCurData[iData]) {
                continue;
            }

            pColor = R_GetLightGridPalettedColor(world, pCurData[iData]);
            VectorMA(vAmbientLight, fWeight + fWeight2, pColor, vAmbientLight);
            VectorMA(vDirectedLight, fWeight + fWeight2, pColor, vDirectedLight);
            fTotalFactor += fWeight + fWeight2;
        } else {
            if (pCurData[iData]) {
                pColor = R_GetLightGridPalettedColor(world, pCurData[iData]);
                VectorMA(vAmbientLight, fWeight, pColor, vAmbientLight);
                fTotalFactor += fWeight;
            }

            iData += 2;
            if (pCurData[iData]) {
                pColor = R_GetLightGridPalettedColor(world, pCurData[iData]);
                VectorMA(vDirectedLight, fWeight2, pColor, vDirectedLight);
                fTotalFactor += fWeight2;
            }
        }
    }

    if (fTotalFactor > 0.0 && fTotalFactor < 0.99) {
        VectorScale(vAmbientLight, 1.0 / fTotalFactor, vAmbientLight);
        VectorScale(vDirectedLight, 1.0 / fTotalFactor, vDirectedLight);
    }

    if (fTotalFactor) {
		/*
        if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
            float t;
            // normalize color values
            t = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
            VectorScale(vLight, t, vLight);
        }
		*/
    } else {
		vDirectedLight[0] = vDirectedLight[1] = vDirectedLight[2] = tr.identityLightByte;
		vAmbientLight[0] = vAmbientLight[1] = vAmbientLight[2] = tr.identityLightByte;
    }
}

/*
===============
R_GetLightingGridValueSingle

HZM gl2 re-port (bug-gl2-modellight): verbatim port of gl1's
R_GetLightingGridValue (gl1 tr_light.c:747-918). Same RLE paletted-grid
traversal as the split ambient/directed variant above, but both samples fold
into ONE flat color with gl1's renormalization - this is the value gl1 feeds
entity/static-model grid lighting and the sphere ambient level. Field renames
only: lightGridMins->lightGridOrigin, lightGridOOSize->lightGridInverseSize.
===============
*/
void R_GetLightingGridValueSingle(world_t *world, const vec3_t vPos, vec3_t vLight)
{
    byte  *pColor;
    int    iBaseOffset;
    int    i;
    int    iOffset;
    int    iRowPos;
    int    iData;
    int    iLen;
    int    iGridPos[3];
    int    iArrayXStep;
    float  fV;
    float  fFrac[3];
    float  fOMFrac[3];
    float  fWeight, fWeight2;
    float  fTotalFactor;
    int    iCurData;
    vec3_t vLightOrigin;
    byte  *pCurData;

    if (!world || !world->lightGridData || !world->lightGridOffsets) {
        vLight[0] = vLight[1] = vLight[2] = tr.identityLightByte;
        return;
    }

    VectorSubtract(vPos, world->lightGridOrigin, vLightOrigin);

    for (i = 0; i < 3; i++) {
        fV          = vLightOrigin[i] * world->lightGridInverseSize[i];
        iGridPos[i] = floor(fV);
        fFrac[i]    = fV - iGridPos[i];
        fOMFrac[i]  = 1.0 - fFrac[i];

        if (iGridPos[i] < 0) {
            iGridPos[i] = 0;
        } else if (iGridPos[i] > world->lightGridBounds[i] - 2) {
            iGridPos[i] = world->lightGridBounds[i] - 2;
        }
    }

    fTotalFactor = 0;
    iArrayXStep  = world->lightGridBounds[1];
    iBaseOffset  = world->lightGridBounds[0] + iGridPos[1] + iArrayXStep * iGridPos[0];
    VectorClear(vLight);

    for (i = 0; i < 4; i++) {
        qboolean bContinue = qfalse;

        switch (i) {
        case 0:
            fWeight  = fOMFrac[0] * fOMFrac[1] * fOMFrac[2];
            fWeight2 = fOMFrac[0] * fOMFrac[1] * fFrac[2];
            iOffset  = world->lightGridOffsets[iBaseOffset] + (world->lightGridOffsets[iGridPos[0]] << 8);
            break;
        case 1:
            fWeight  = fOMFrac[0] * fFrac[1] * fOMFrac[2];
            fWeight2 = fOMFrac[0] * fFrac[1] * fFrac[2];
            iOffset  = world->lightGridOffsets[iBaseOffset + 1] + (world->lightGridOffsets[iGridPos[0]] << 8);
            break;
        case 2:
            fWeight  = fFrac[0] * fOMFrac[1] * fOMFrac[2];
            fWeight2 = fFrac[0] * fOMFrac[1] * fFrac[2];
            iOffset  = world->lightGridOffsets[iBaseOffset + iArrayXStep]
                    + (world->lightGridOffsets[iGridPos[0] + 1] << 8);
            break;
        case 3:
            fWeight  = fFrac[0] * fFrac[1] * fOMFrac[2];
            fWeight2 = fFrac[0] * fFrac[1] * fFrac[2];
            iOffset  = world->lightGridOffsets[iBaseOffset + iArrayXStep + 1]
                    + (world->lightGridOffsets[iGridPos[0] + 1] << 8);
            break;
        }

        iRowPos  = iGridPos[2];
        pCurData = &world->lightGridData[iOffset];
        iData    = 0;

        while (1) {
            while (1) {
                iCurData = (char)pCurData[iData];
                iData++;
                if (iCurData >= 0) {
                    break;
                }

                iLen = -iCurData;
                if (iLen > iRowPos) {
                    iData += iRowPos;

                    if (pCurData[iData]) {
                        pColor = R_GetLightGridPalettedColor(world, pCurData[iData]);
                        VectorMA(vLight, fWeight, pColor, vLight);
                        fTotalFactor += fWeight;
                    }

                    iData++;
                    if (iLen - 1 == iRowPos) {
                        iData++;
                    }

                    if (pCurData[iData]) {
                        pColor = R_GetLightGridPalettedColor(world, pCurData[iData]);
                        VectorMA(vLight, fWeight2, pColor, vLight);
                        fTotalFactor += fWeight2;
                    }

                    bContinue = qtrue;
                    break;
                }

                iRowPos -= iLen;
                iData += iLen;
            }

            if (bContinue) {
                break;
            }

            iLen = iCurData + 2;
            if (iLen - 1 >= iRowPos) {
                break;
            }

            iRowPos -= iLen;
            iData++;
        }

        if (bContinue) {
            continue;
        }

        if (iLen - 1 > iRowPos) {
            if (!pCurData[iData]) {
                continue;
            }

            pColor = R_GetLightGridPalettedColor(world, pCurData[iData]);
            VectorMA(vLight, fWeight + fWeight2, pColor, vLight);
            fTotalFactor += fWeight + fWeight2;
        } else {
            if (pCurData[iData]) {
                pColor = R_GetLightGridPalettedColor(world, pCurData[iData]);
                VectorMA(vLight, fWeight, pColor, vLight);
                fTotalFactor += fWeight;
            }

            iData += 2;
            if (pCurData[iData]) {
                pColor = R_GetLightGridPalettedColor(world, pCurData[iData]);
                VectorMA(vLight, fWeight2, pColor, vLight);
                fTotalFactor += fWeight2;
            }
        }
    }

    if (fTotalFactor > 0.0 && fTotalFactor < 0.99) {
        VectorScale(vLight, 1.0 / fTotalFactor, vLight);
    }

    if (fTotalFactor) {
        if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
            float t;
            // normalize color values
            t = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
            VectorScale(vLight, t, vLight);
        }
    } else {
        vLight[0] = vLight[1] = vLight[2] = tr.identityLightByte;
    }
}

/*
===============
R_GetLightingForDecal
===============
*/
void R_GetLightingForDecal(vec3_t vLight, const vec3_t vFacing, const vec3_t vOrigin)
{
    // HZM gl2 (bug-gl2-reddecals, #74): was an empty stub, so vLight was left as
    // uninitialized stack garbage. cg_marks.c::CG_ImpactMark then built the impact
    // mark's per-vertex modulate from red*vLight[0]/green*vLight[1]/blue*vLight[2],
    // so every "dolighting" decal got a garbage vertex color. Decals whose shader
    // uses "rgbGen vertex" faithfully render that garbage (perceived as RED); decals
    // with "rgbGen identity" ignore the vertex color and looked correct - and forcing
    // the .tga over the .dds changed nothing because the texture was never at fault.
    // Port gl1's implementation verbatim (gl1 tr_light.c:1092): sample the light grid,
    // apply the overbright multiply + renormalize. gl2's single-value grid lookup is
    // R_GetLightingGridValueSingle (NULL/gridless world -> identityLightByte fallback).
    R_GetLightingGridValueSingle(tr.world, vOrigin, vLight);

    if (!tr.overbrightShift) {
        return;
    }

    VectorScale(vLight, tr.overbrightMult, vLight);

    if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
        float scale = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
        VectorScale(vLight, scale, vLight);
    }
}

/*
===============
R_GetLightingForSmoke
===============
*/
void R_GetLightingForSmoke(vec3_t vLight, const vec3_t vOrigin)
{
    int       i;
    dlight_t *dl;
    float     power;
    vec3_t    dir;
    float     d;

    // HZM gl2: was an empty stub -> vLight was uninitialized stack garbage -> smoke-grenade
    // (and other smoke) lighting was garbage under gl2 (same class as R_GetLightingForDecal,
    // #74). Port gl1's R_GetLightingForSmoke (gl1 tr_light.c:1115). gl2 has no 2-arg
    // R_GetLightingGridValue/Fast, so use the single-value grid lookup like the decal fix
    // (NULL/gridless world -> identityLightByte fallback). Unlike the decal path, smoke's
    // consumer expects [0,1], so keep gl1's trailing /255 normalize.
    R_GetLightingGridValueSingle(tr.world, vOrigin, vLight);

    for (i = 0; i < backEnd.refdef.num_dlights; i++) {
        dl = &backEnd.refdef.dlights[i];
        VectorSubtract(dl->origin, vOrigin, dir);
        d = VectorLengthSquared(dir);

        power = dl->radius * dl->radius;
        if (power >= d) {
            d = dl->radius * 7500.0 / d;
            // HZM gl2 [2026-09-26] Phase S1: a spot lights smoke only inside its cone and range (others untouched)
            if (dl->hzmSpot == HZM_SPOT_READY) {
                d *= R_HZM_DlightFactorAt(dl, vOrigin, 0.0f);
            }
            VectorMA(vLight, d, dl->color, vLight);
        }
    }

    if (tr.overbrightShift) {
        vLight[0] = tr.overbrightMult * vLight[0];
        vLight[1] = tr.overbrightMult * vLight[1];
        vLight[2] = tr.overbrightMult * vLight[2];
    }

    // normalize
    if (vLight[0] > 255.0 || vLight[1] > 255.0 || vLight[2] > 255.0) {
        float scale = 255.0 / Q_max(vLight[0], Q_max(vLight[1], vLight[2]));
        VectorScale(vLight, scale, vLight);
    }

    vLight[0] /= 255.0;
    vLight[1] /= 255.0;
    vLight[2] /= 255.0;
}

qboolean R_FindGridPointForSphere(world_t *world, const vec3_t sphereOrigin, const vec3_t point, vec3_t out) {
    int i, j, k;
    int gridStep[3][3];
	trace_t trace;

    for (i = 0; i < 3; i++) {
        gridStep[i][0] = 0;
        gridStep[i][1] = -world->lightGridSize[i];
        gridStep[i][2] = world->lightGridSize[i];
    }

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			for (k = 0; k < 3; k++) {
				vec3_t vOffset;

                vOffset[0] = point[0] + gridStep[0][i];
                vOffset[1] = point[1] + gridStep[1][j];
                vOffset[2] = point[2] + gridStep[2][k];

				ri.CM_BoxTrace(&trace, point, sphereOrigin, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, 0);

				if (trace.fraction == 1 || trace.surfaceFlags & SURF_SKY) {
					return qtrue;
				}
			}
		}
	}

	return qfalse;
}

static int R_LightDirForPointEx( vec3_t point, vec3_t lightDir, vec3_t normal, world_t *world, qboolean useAreaMask,
    hzmLDirInfo_t *info ) // HZM [bug-3064] info: optional, NULL on every normal path
{
    vec3_t vLight;
    vec3_t vEnd;
    mnode_t* leaf;
    trace_t trace;
    vec3_t summedDir;
    float addSize;
    int iNumLights;
    int i;
	int j, k, l;
    int gridStep[3][3];

	if ( world->lightGridData == NULL )
	  return qfalse;

    VectorClear(summedDir);
	leaf = NULL;
	iNumLights = 0;
    if (info) {
        Com_Memset(info, 0, sizeof(*info));
    }

    if (world->vis) {
        leaf = R_FindPoint(world, point);
        if (leaf->area == -1 || !leaf->numlights) {
            leaf = NULL;
        }
    }

    if (info) {
        info->leafOk  = leaf ? 1 : 0;
        info->sunList = (leaf && leaf->lights[0] == &tr.sSunLight) ? 1 : 0;
    }

    if (leaf && leaf->numlights) {
        spherel_t* sphere;

		for (i = (leaf->lights[0] == &tr.sSunLight ? 1 : 0); i < leaf->numlights; i++) {
			sphere = leaf->lights[i];
			if (sphere->leaf != (mnode_t*)-1) {
                // HZM [bug-3007] the Static variant skips the per-view gate - and the areamask read with it
                if (!useAreaMask || !(backEnd.refdef.areamask[sphere->leaf->area >> 3] & (1 << (sphere->leaf->area & 7)))) {

                    ri.CM_BoxTrace(&trace, point, sphere->origin, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, 0);

                    if (trace.fraction == 1) {
                        vec3_t dir;

                        VectorSubtract(sphere->origin, point, dir);

                        addSize = VectorLength(sphere->color);
                        VectorMA(summedDir, addSize, dir, summedDir);

                        iNumLights++;
                        if (info) {
                            info->numLights++;
                            VectorMA(info->lampSum, addSize, dir, info->lampSum);
                        }
                    }
				}
			}
		}
	}
	
    if (s_sun.exists) {
        if (leaf && leaf->lights[0] == &tr.sSunLight) {
            vec3_t newPoint;

            VectorMA(point, 16384.0, s_sun.direction, vEnd);


            ri.CM_BoxTrace(&trace, point, vEnd, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, 0);

            if (trace.surfaceFlags & SURF_SKY) {
                addSize = VectorLength(s_sun.color);

                VectorMA(summedDir, addSize, s_sun.direction, summedDir);

                iNumLights++;
                if (info) {
                    info->sunHit    = 1;
                    info->sunWeight = addSize;
                }
            }
        }
    }

	VectorNormalize(summedDir);

    if (DotProduct(summedDir, normal) > 0.2f)
        VectorCopy(summedDir, lightDir);
    else
        VectorCopy(normal, lightDir);

	return qtrue;
}

/*
===============
HZM gl2 [render-perf 4] THE LIGHT-DIRECTION MEMO

R_LightDirForPoint is a world query: find the leaf, then one collision trace to every map light in it and one to
the sun. It is asked the same question every frame - on the Omaha maps once per terrain patch per pass, always from
the patch's fixed centre point (the bug-2905 path), and for every entity that has not moved.

The answer is a pure function of the point, the normal handed in, the map (its lights and its solid world: the
trace runs against the world model only) and the view's area mask (a light in a closed-off area is skipped). So it
is kept per (point, normal, area mask): same question, same answer, no trace. The sun and the map are part of a
generation number - any change, and a map load (R_LightDirCacheFlush), empties the memo.

r_hzmLightDirMemo 0 = always ask the world (live). 2 = verify: every answer from the memo is also asked of the
world and compared bit for bit (counted by r_hzmPerfStats; the differing count has to stay 0).
===============
*/
#define LDC_SLOTS	8192

typedef struct {
	vec3_t		point;
	vec3_t		normal;
	unsigned	maskHash;
	int			gen;
	int			ret;
	vec3_t		dir;
} ldcEntry_t;

static ldcEntry_t	s_ldc[LDC_SLOTS];
static int			s_ldcGen = 1;
static world_t		*s_ldcWorld;
static void			*s_ldcGrid;
static vec3_t		s_ldcSunColor, s_ldcSunDir;
static qboolean		s_ldcSunExists;
static byte			s_ldcMask[MAX_MAP_AREA_BYTES];
static unsigned		s_ldcMaskHash;
static cvar_t		*r_hzmLightDirMemo;

void R_LightDirCacheFlush( void )
{
	s_ldcGen++;
	if ( s_ldcGen <= 0 ) {
		Com_Memset( s_ldc, 0, sizeof( s_ldc ) );
		s_ldcGen = 1;
	}
	s_ldcWorld = NULL;
}

int R_LightDirForPoint( vec3_t point, vec3_t lightDir, vec3_t normal, world_t *world )
{
	ldcEntry_t	*e;
	unsigned	h, w[6];
	int			i;

	if ( !r_hzmLightDirMemo ) {
		r_hzmLightDirMemo = ri.Cvar_Get( "r_hzmLightDirMemo", "1", 0 );
	}
	if ( !r_hzmLightDirMemo->integer ) {
		return R_LightDirForPointEx( point, lightDir, normal, world, qtrue, NULL );
	}

	// the map or the sun changed: a new generation
	if ( world != s_ldcWorld || (void *)world->lightGridData != s_ldcGrid || s_sun.exists != s_ldcSunExists
		|| memcmp( s_sun.color, s_ldcSunColor, sizeof( vec3_t ) ) || memcmp( s_sun.direction, s_ldcSunDir, sizeof( vec3_t ) ) ) {
		R_LightDirCacheFlush();
		s_ldcWorld = world;
		s_ldcGrid = (void *)world->lightGridData;
		s_ldcSunExists = s_sun.exists;
		VectorCopy( s_sun.color, s_ldcSunColor );
		VectorCopy( s_sun.direction, s_ldcSunDir );
		s_ldcMaskHash = 0;
		Com_Memset( s_ldcMask, 0, sizeof( s_ldcMask ) );
	}
	// the area mask the query will read
	if ( memcmp( backEnd.refdef.areamask, s_ldcMask, sizeof( s_ldcMask ) ) ) {
		memcpy( s_ldcMask, backEnd.refdef.areamask, sizeof( s_ldcMask ) );
		s_ldcMaskHash = 2166136261u;
		for ( i = 0; i < (int)sizeof( s_ldcMask ); i++ ) {
			s_ldcMaskHash = ( s_ldcMaskHash ^ s_ldcMask[i] ) * 16777619u;
		}
	}

	memcpy( w, point, 12 );
	memcpy( w + 3, normal, 12 );
	h = s_ldcMaskHash;
	for ( i = 0; i < 6; i++ ) {
		h = ( h ^ w[i] ) * 16777619u;
		h ^= h >> 15;
	}
	e = &s_ldc[h & ( LDC_SLOTS - 1 )];
	if ( e->gen == s_ldcGen && e->maskHash == s_ldcMaskHash && !memcmp( e->point, point, sizeof( vec3_t ) )
		&& !memcmp( e->normal, normal, sizeof( vec3_t ) ) ) {
		if ( r_hzmLightDirMemo->integer == 2 ) {
			vec3_t	real;
			int		realRet;

			VectorCopy( lightDir, real );
			realRet = R_LightDirForPointEx( point, real, normal, world, qtrue, NULL );
			hzmPerf.verifyLightDir++;
			if ( realRet != e->ret || ( realRet && memcmp( real, e->dir, sizeof( vec3_t ) ) ) ) {
				hzmPerf.verifyLightDirBad++;
			}
		}
		if ( e->ret ) {
			VectorCopy( e->dir, lightDir );
		}
		return e->ret;
	}

	e->ret = R_LightDirForPointEx( point, lightDir, normal, world, qtrue, NULL );
	VectorCopy( point, e->point );
	VectorCopy( normal, e->normal );
	e->maskHash = s_ldcMaskHash;
	e->gen = s_ldcGen;
	if ( e->ret ) {
		VectorCopy( lightDir, e->dir );
	}
	return e->ret;
}

/*
HZM gl2 [bug-3007] the VIEW-INDEPENDENT variant, for values cached once per map (the terrain light grid,
tr_surface.c R_HZM_TerrainLightGridBuild). Identical sum, minus the per-view gate that drops a sphere light whose
area is not in backEnd.refdef.areamask: at load that mask is whatever the last view left, so a cached value would
depend on where some earlier frame stood. The lightmap it re-shades has no such gate either.
*/
int R_LightDirForPointStatic( vec3_t point, vec3_t lightDir, vec3_t normal, world_t *world )
{
	return R_LightDirForPointEx( point, lightDir, normal, world, qfalse, NULL );
}

int R_LightDirForPointInfo( vec3_t point, vec3_t lightDir, vec3_t normal, world_t *world, qboolean useAreaMask, hzmLDirInfo_t *info )
{
	return R_LightDirForPointEx( point, lightDir, normal, world, useAreaMask, info );
}

static void R_SetupEntityLightingGrid(trRefEntity_t* ent, world_t* world)
{
	vec3_t normal;

    R_GetLightingGridValue(world, ent->e.origin, ent->ambientLight, ent->directedLight);

	VectorSet(normal, 1, 0, 0);
    R_LightDirForPoint(ent->e.origin, ent->lightDir, normal, world);
}
