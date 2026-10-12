#include "StdAfx.h"

#include "PhysicsShell.h"
#include "PHStepDefer.h"

#include "xrScriptEngine/script_space.hpp"

// Mutating methods go through lambdas that open ph_step_defer::ScriptCallScope: with the B6 early physics kick the call
// may be queued until the physics step is joined (PHStepDefer.h), and the scope counts it as physics/defer/lua.
// The call returns right away either way; reads (get_*, is_*) are never deferred.

void CPhysicsShell::script_register(lua_State* L)
{
    using namespace luabind;

    module(L)
    [
        class_<CPhysicsShell>("physics_shell")
            .def("apply_force", +[](CPhysicsShell* self, float x, float y, float z)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->applyForce(x, y, z);
            })
            .def("get_element_by_bone_name", (CPhysicsElement * (CPhysicsShell::*)(LPCSTR))(&CPhysicsShell::get_Element))
            .def("get_element_by_bone_id", (CPhysicsElement * (CPhysicsShell::*)(u16))(&CPhysicsShell::get_Element))
            .def("get_element_by_order", (CPhysicsElement * (CPhysicsShell::*)(u16))(&CPhysicsShell::get_ElementByStoreOrder))
            .def("get_elements_number", &CPhysicsShell::get_ElementsNumber)
            .def("get_joint_by_bone_name", (CPhysicsJoint * (CPhysicsShell::*)(LPCSTR))(&CPhysicsShell::get_Joint))
            .def("get_joint_by_bone_id", (CPhysicsJoint * (CPhysicsShell::*)(u16))(&CPhysicsShell::get_Joint))
            .def("get_joint_by_order", &CPhysicsShell::get_JointByStoreOrder)
            .def("get_joints_number", &CPhysicsShell::get_JointsNumber)
            .def("block_breaking", &CPhysicsShell::BlockBreaking)
            .def("unblock_breaking", &CPhysicsShell::UnblockBreaking)
            .def("is_breaking_blocked", &CPhysicsShell::IsBreakingBlocked)
            .def("is_breakable", &CPhysicsShell::isBreakable)
            .def("get_linear_vel", &CPhysicsShell::get_LinearVel)
            .def("get_angular_vel", &CPhysicsShell::get_AngularVel)
    ];
}

void CPhysicsElement::script_register(lua_State* L)
{
    using namespace luabind;

    module(L)
    [
        class_<CPhysicsElement>("physics_element")
            .def("apply_force", +[](CPhysicsElement* self, float x, float y, float z)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->applyForce(x, y, z);
            })
            .def("is_breakable", &CPhysicsElement::isBreakable)
            .def("get_linear_vel", &CPhysicsElement::get_LinearVel)
            .def("get_angular_vel", &CPhysicsElement::get_AngularVel)
            .def("get_mass", &CPhysicsElement::getMass)
            .def("get_density", &CPhysicsElement::getDensity)
            .def("get_volume", &CPhysicsElement::getVolume)
            .def("fix", +[](CPhysicsElement* self)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->Fix();
            })
            .def("release_fixed", +[](CPhysicsElement* self)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->ReleaseFixed();
            })
            .def("is_fixed", &CPhysicsElement::isFixed)
            .def("global_transform", +[](const CPhysicsElement* E)
            {
                Fmatrix m;
                E->GetGlobalTransformDynamic(&m);
                return m;
            })
    ];
}

void CPhysicsJoint::script_register(lua_State* L)
{
    using namespace luabind;
    using namespace luabind::policy;

    constexpr auto out_value_23 = policy_list<out_value<2>, out_value<3>>();

    module(L)
    [
        class_<CPhysicsJoint>("physics_joint")
            .def("get_bone_id", &CPhysicsJoint::BoneID)
            .def("get_first_element", &CPhysicsJoint::PFirst_element)
            .def("get_stcond_element", &CPhysicsJoint::PSecond_element)
            .def("set_anchor_global", +[](CPhysicsJoint* self, float x, float y, float z)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->SetAnchor(x, y, z);
            })
            .def("set_anchor_vs_first_element", +[](CPhysicsJoint* self, float x, float y, float z)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->SetAnchorVsFirstElement(x, y, z);
            })
            .def("set_anchor_vs_second_element", +[](CPhysicsJoint* self, float x, float y, float z)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->SetAnchorVsSecondElement(x, y, z);
            })
            .def("get_axes_number", &CPhysicsJoint::GetAxesNumber)
            .def("set_axis_spring_dumping_factors", +[](CPhysicsJoint* self, float spring, float damping, int axis)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->SetAxisSDfactors(spring, damping, axis);
            })
            .def("set_joint_spring_dumping_factors", +[](CPhysicsJoint* self, float spring, float damping)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->SetJointSDfactors(spring, damping);
            })
            .def("set_axis_dir_global", +[](CPhysicsJoint* self, float x, float y, float z, int axis)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->SetAxisDir(x, y, z, axis);
            })
            .def("set_axis_dir_vs_first_element", +[](CPhysicsJoint* self, float x, float y, float z, int axis)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->SetAxisDirVsFirstElement(x, y, z, axis);
            })
            .def("set_axis_dir_vs_second_element", +[](CPhysicsJoint* self, float x, float y, float z, int axis)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->SetAxisDirVsSecondElement(x, y, z, axis);
            })
            .def("set_limits", +[](CPhysicsJoint* self, float low, float high, int axis)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->SetLimits(low, high, axis);
            })
            .def("set_max_force_and_velocity", +[](CPhysicsJoint* self, float force, float velocity, int axis)
            {
                ph_step_defer::ScriptCallScope script_call;
                self->SetForceAndVelocity(force, velocity, axis);
            })
            .def("get_max_force_and_velocity", &CPhysicsJoint::GetMaxForceAndVelocity, out_value_23)
            .def("get_axis_angle", &CPhysicsJoint::GetAxisAngle)
            .def("get_limits", &CPhysicsJoint::GetLimits, out_value_23)
            .def("get_axis_dir", &CPhysicsJoint::GetAxisDirDynamic)
            .def("get_anchor", &CPhysicsJoint::GetAnchorDynamic)
            .def("is_breakable", &CPhysicsJoint::isBreakable)
    ];
}
