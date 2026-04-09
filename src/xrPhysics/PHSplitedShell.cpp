#include "StdAfx.h"
#include "PhysicsShell.h"
#include "PHObject.h"
#include "PHWorld.h"
#include "PHInterpolation.h"
#include "PHShell.h"
#include "PHJoint.h"
#include "PHElement.h"
#include "PHSplitedShell.h"
#include "Physics.h"
#include "SpaceUtils.h"
void CPHSplitedShell::CollideDynamicsBroadphase()
{
    m_collide_spatial_overlap.clear();
    m_collide_dyn_candidates.clear();
}
void CPHSplitedShell::CollideStepPostBroadphase()
{
    CollideStatic(dSpacedGeom(), CPHObject::SelfPointer());
    CollideClearDirty();
}

void CPHSplitedShell::get_spatial_params()
{
    spatialParsFromDGeom((dGeomID)m_space, spatial.sphere.P, AABB, spatial.sphere.R);
    if (spatial.sphere.R > m_max_AABBradius)
        spatial.sphere.R = m_max_AABBradius;
}

void CPHSplitedShell::DisableObject() { CPHObject::deactivate(); }
